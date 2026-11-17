#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstunits.h"
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

static void require(bool condition, const char* message)
{
	if(!condition) throw std::runtime_error(message);
}

template<class T> static IPtr<T> query(FUnknown* object)
{
	T* result = nullptr;
	require(object->queryInterface(T::iid, reinterpret_cast<void**>(&result)) == kResultOk, "Missing VST3 interface");
	return owned(result);
}

struct Queue final : IParamValueQueue
{
	ParamID id = 0;
	std::vector<std::pair<int32, ParamValue>> points;
	tresult PLUGIN_API queryInterface(const TUID, void**) override { return kNoInterface; }
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
	ParamID PLUGIN_API getParameterId() override { return id; }
	int32 PLUGIN_API getPointCount() override { return static_cast<int32>(points.size()); }
	tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override
	{
		if(index < 0 || index >= getPointCount()) return kResultFalse;
		offset = points[index].first; value = points[index].second; return kResultOk;
	}
	tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32& index) override
	{
		index = getPointCount(); points.emplace_back(offset, value); return kResultOk;
	}
};

struct Changes final : IParameterChanges
{
	std::array<Queue, 17> queues;
	int32 count = 0;
	tresult PLUGIN_API queryInterface(const TUID, void**) override { return kNoInterface; }
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
	int32 PLUGIN_API getParameterCount() override { return count; }
	IParamValueQueue* PLUGIN_API getParameterData(int32 index) override { return &queues.at(index); }
	IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override
	{
		index = count++; auto& q = queues.at(index); q.id = id; q.points.clear(); return &q;
	}
	void add(ParamID id, std::initializer_list<std::pair<int32, ParamValue>> points)
	{
		auto& q = queues.at(count++); q.id = id; q.points = points;
	}
};

static void test(IComponent* component, IEditController* controller, bool fixture, bool legacy)
{
	auto units = query<IUnitInfo>(controller);
	auto mapping = query<IMidiMapping>(controller);
	std::map<UnitID, UnitInfo> unitInfo;
	std::map<ProgramListID, ProgramListInfo> lists;
	std::map<UnitID, ParameterInfo> selections;
	for(int32 i = 0; i < units->getUnitCount(); ++i)
	{
		UnitInfo info{}; require(units->getUnitInfo(i, info) == kResultOk, "Unit info failed");
		require(unitInfo.emplace(info.id, info).second, "Duplicate unit ID");
	}
	for(int32 i = 0; i < units->getProgramListCount(); ++i)
	{
		ProgramListInfo info{}; require(units->getProgramListInfo(i, info) == kResultOk, "Program list failed");
		require(lists.emplace(info.id, info).second, "Duplicate program list ID");
	}
	for(int32 i = 0; i < controller->getParameterCount(); ++i)
	{
		ParameterInfo info{}; require(controller->getParameterInfo(i, info) == kResultOk, "Parameter info failed");
		if(info.flags & ParameterInfo::kIsProgramChange)
			require(selections.emplace(info.unitId, info).second, "Multiple selections in one unit");
	}
	if(legacy)
	{
		require(unitInfo.size() == 1 && lists.size() == 1 && selections.size() == 1, "Opt-out preset metadata changed");
		require(lists.at(unitInfo.at(kRootUnitId).programListId).programCount == 3, "Opt-out host presets changed");
		for(int channel = 0; channel < 16; ++channel)
		{
			UnitID unit = -1;
			require(units->getUnitByBus(kEvent, kInput, 0, channel, unit) == kResultOk && unit == kRootUnitId,
				"Opt-out unit mapping changed");
		}
		std::puts("PASS: opt-out keeps existing host-preset and root-unit behavior");
		return;
	}
	std::array<ParamID, 16> ids{};
	std::set<UnitID> channelUnits;
	for(int channel = 0; channel < 16; ++channel)
	{
		UnitID unit = -1;
		require(units->getUnitByBus(kEvent, kInput, 0, channel, unit) == kResultOk, "MIDI channel has no unit");
		require(unit != kRootUnitId && channelUnits.insert(unit).second, "MIDI channels collapse to one unit");
		const auto& info = unitInfo.at(unit);
		require(info.parentUnitId == kRootUnitId, "Incorrect MIDI unit parent");
		require(lists.at(info.programListId).programCount == 128, "MIDI program count is not 128");
		const auto& selection = selections.at(unit);
		require(selection.stepCount == 127, "Incorrect program-selection steps");
		require(selection.defaultNormalizedValue >= 0 && selection.defaultNormalizedValue <= 1, "Invalid normalized default");
		ids[channel] = selection.id;
		require(ids[channel] == 0x6d637520u + channel, "Historical MIDI PC parameter ID changed");
		for(int program = 0; program < 128; ++program)
		{
			String128 name{};
			require(units->getProgramName(info.programListId, program, name) == kResultOk && name[0], "Missing MIDI program name");
		}
		// Deliberately use only the SDK's normal controller range, like REAPER.
		for(int control = 0; control < 130; ++control)
		{
			ParamID id{}; require(mapping->getMidiControllerAssignment(0, channel, control, id) == kResultOk, "CC mapping regressed");
		}
	}
	UnitID invalid{};
	require(units->getUnitByBus(kAudio, kInput, 0, 0, invalid) != kResultOk, "Audio bus accepted as MIDI");
	require(units->getUnitByBus(kEvent, kOutput, 0, 0, invalid) != kResultOk, "Output bus accepted as input");
	for(int channel : {-1, 16})
		require(units->getUnitByBus(kEvent, kInput, 0, channel, invalid) != kResultOk, "Invalid MIDI channel accepted");
	require(units->getUnitByBus(kEvent, kInput, 1, 0, invalid) != kResultOk, "Invalid MIDI bus accepted");
	ParamID invalidParameter{};
	require(mapping->getMidiControllerAssignment(0, 16, 0, invalidParameter) != kResultOk, "Invalid CC channel accepted");
	require(mapping->getMidiControllerAssignment(0, 0, 131, invalidParameter) != kResultOk, "Invalid CC number accepted");
	std::puts("PASS: 16 independent MIDI units, 128 programs, stable parameter IDs, normal CC mappings");
	if(!fixture) return;

	const auto presetId = selections.at(kRootUnitId).id;
	require(lists.at(unitInfo.at(kRootUnitId).programListId).programCount == 3, "Host presets were replaced");
	auto processor = query<IAudioProcessor>(component);
	SpeakerArrangement stereo = SpeakerArr::kStereo;
	require(processor->setBusArrangements(nullptr, 0, &stereo, 1) == kResultOk, "Bus arrangement failed");
	require(component->activateBus(kAudio, kOutput, 0, true) == kResultOk, "Bus activation failed");
	ProcessSetup setup{kRealtime, kSample32, 64, 48000.0};
	require(processor->setupProcessing(setup) == kResultOk, "Processing setup failed");
	require(component->setActive(true) == kResultOk, "Activation failed");
	require(processor->setProcessing(true) == kResultOk, "Processing activation failed");
	std::array<float, 64> left{}, right{};
	float* channels[]{left.data(), right.data()};
	AudioBusBuffers bus{}; bus.numChannels = 2; bus.channelBuffers32 = channels;
	Changes changes;
	ProcessData data{}; data.processMode = kRealtime; data.symbolicSampleSize = kSample32;
	data.numSamples = 64; data.numOutputs = 1; data.outputs = &bus; data.inputParameterChanges = &changes;
	auto process = [&]
	{
		require(processor->process(data) == kResultOk, "Process failed");
		changes.count = 0;
	};
	// Host preset selection updates the edit controller as well as the audio queue.
	require(controller->setParamNormalized(presetId, 1.0) == kResultOk, "Host preset selection failed");
	changes.add(presetId, {{0, 1.0}}); process();
	require(right[0] == 2, "Host preset 3 was not selected before MIDI test");
	for(int channel = 0; channel < 16; ++channel)
		for(int program = 0; program < 128; ++program)
		{
			// Two identical selections separated by another selection in one queue;
			// no MIDI/controller-130 input shortcut is used.
			const int other = (program + 37) % 128;
			changes.add(ids[channel], {{3, program / 127.0}, {17, other / 127.0}, {47, program / 127.0}});
			process();
			for(int sample = 0; sample < 64; ++sample)
			{
				const int expected = sample == 3 || sample == 47 ? channel * 128 + program + 1
					: sample == 17 ? channel * 128 + other + 1 : 0;
				require(left[sample] == expected, "PC channel/program/sample offset/repeat mismatch");
				require(right[sample] == 2, "MIDI PC changed host preset");
			}
		}
	for(int channel = 15; channel >= 0; --channel)
		changes.add(ids[channel], {{channel * 3, (127 - channel) / 127.0}});
	process();
	for(int channel = 0; channel < 16; ++channel)
		require(left[channel * 3] == channel * 128 + 128 - channel, "Interleaved channel queues failed");
	changes.add(ids[0], {{7, 42.25 / 128.0}}); process();
	require(left[7] == 43, "Legacy normalized automation changed");
	changes.add(ids[0], {{7, 42.25 / 128.0}}); process();
	require(left[7] == 43, "Repeated selection across blocks was suppressed");
	processor->setProcessing(false); component->setActive(false);
	std::puts("PASS: all 2048 channel/program combinations, repeated events, offsets, queue ordering, independent host presets");
}

int main(int argc, char** argv)
{
	try
	{
		require(argc == 2 || argc == 3, "Usage: vst3ProgramChangeTest BUNDLE [--fixture|--legacy]");
		const std::string mode = argc == 3 ? argv[2] : "";
		require(mode.empty() || mode == "--fixture" || mode == "--legacy", "Unknown test mode");
		std::string error;
		auto module = VST3::Hosting::Module::create(argv[1], error);
		require(module != nullptr, error.c_str());
		auto& factory = module->getFactory();
		auto host = owned(new HostApplication);
		factory.setHostContext(host);
		for(const auto& info : factory.classInfos())
		{
			if(info.category() != kVstAudioEffectClass) continue;
			auto component = factory.createInstance<IComponent>(info.ID());
			require(component && component->initialize(host) == kResultOk, "Component initialization failed");
			TUID controllerId{};
			require(component->getControllerClassId(controllerId) == kResultOk, "Controller ID missing");
			auto controller = factory.createInstance<IEditController>(VST3::UID(controllerId));
			require(controller && controller->initialize(host) == kResultOk, "Controller initialization failed");
			auto componentConnection = query<IConnectionPoint>(component);
			auto controllerConnection = query<IConnectionPoint>(controller);
			require(componentConnection->connect(controllerConnection) == kResultOk, "Component connection failed");
			require(controllerConnection->connect(componentConnection) == kResultOk, "Controller connection failed");
			test(component, controller, mode == "--fixture", mode == "--legacy");
			controllerConnection->disconnect(componentConnection);
			componentConnection->disconnect(controllerConnection);
			controller->terminate(); component->terminate();
			return 0;
		}
		throw std::runtime_error("No audio component in bundle");
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
	}
}
