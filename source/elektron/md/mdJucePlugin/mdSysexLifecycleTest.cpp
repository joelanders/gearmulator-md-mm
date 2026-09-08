#include "mdAutomationTestSupport.h"
#include "mdLibTest/sdsTestData.h"
#include "synthLib/romLoader.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace
{
	using namespace mdAutomationTest;
	using Bytes = std::vector<uint8_t>;
	using Start = md::SysexImportStartResult;
	using Stage = md::SysexImportStage;
	using State = md::MidiSysexTransferState;
	Bytes load(const std::string& path)
	{
		std::ifstream file(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	}

	// Own processor/config/cache directory; never read or write the user's active
	// machine storage. ROMs remain external, supplied explicitly by the test runner.
	class ProcessorHarness
	{
	public:
		StoppedPlayHead playHead;
		mdJucePlugin::AudioPluginAudioProcessor processor;
		juce::AudioProcessor& audioProcessor;
		ProcessorHarness(md::MachineModel model, const std::string& directory)
			: processor(model, mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig{directory}, false)
			, audioProcessor(processor)
		{
			audioProcessor.setPlayHead(&playHead);
			audioProcessor.setNonRealtime(true);
			audioProcessor.prepareToPlay(48000, 512);
		}
		~ProcessorHarness() { audioProcessor.releaseResources(); }
		template<class F> auto withDevice(F&& function)
		{
			return processor.getPlugin().withDeviceLocked([&](synthLib::Device* base)
			{
				auto* device = dynamic_cast<md::Device*>(base);
				require(device != nullptr, "local device disappeared");
				return function(*device);
			});
		}
		void frames(unsigned count)
		{
			// Exercise non-power-of-two blocks and host buffer size changes. No
			// direct Hardware::advance shortcut: all scheduling is the product path.
			constexpr unsigned sizes[] = {64, 127, 256, 511, 128};
			while(count)
			{
				const auto n = std::min(count, sizes[block++ % std::size(sizes)]);
				juce::AudioBuffer<float> audio(2, int(n));
				juce::MidiBuffer midi;
				audio.clear();
				audioProcessor.processBlock(audio, midi);
				count -= n;
			}
		}
		md::SysexImportProgress progress()
		{
			return withDevice([](md::Device& d) { return d.userSysexImportProgress(); });
		}
		md::SysexImportTicket begin()
		{
			const auto ticket = withDevice([](md::Device& d) { return d.beginUserSysexImport(); });
			require(ticket.has_value(), "cannot reserve import");
			return *ticket;
		}
		Start start(const md::SysexImportTicket& ticket, md::PreparedMidiSysexTransfer& data, bool confirmed = true)
		{
			return withDevice([&](md::Device& d) { return d.startUserSysexImport(ticket, data, confirmed); });
		}
		bool cancel(const md::SysexImportTicket& ticket)
		{
			Bytes retired; // free outside the device lock, as in the editor
			return withDevice([&](md::Device& d) { return d.cancelUserSysexImport(ticket, retired); });
		}
		void settledBoot()
		{
			// A fixture setup interval, NOT a readiness policy in production.
			frames(48000 * 20);
			require(withDevice([](md::Device& d) { return d.getHardware().isFirmwareMidiReady(); }), "MIDI never ready");
		}
		void terminal()
		{
			for(unsigned i = 0; i < 1800; ++i)
			{
				const auto p = progress();
				if(p.stage == Stage::DeliveredUnverified || p.stage == Stage::Cancelled || p.stage == Stage::Failed) return;
				frames(4800);
			}
			throw std::runtime_error("processor transfer deadline");
		}
		void payloadStarted()
		{
			for(unsigned i = 0; i < 600; ++i)
			{
				const auto p = progress();
				if(p.sent >= 300 && p.sent < p.total && p.stage == Stage::Transferring) return;
				require(p.stage == Stage::Transferring, "transfer terminated before partial-payload fixture");
				frames(2400);
			}
			throw std::runtime_error("processor never sent partial payload");
		}
		void restore(const juce::MemoryBlock& state)
		{
			audioProcessor.setStateInformation(state.getData(), int(state.getSize()));
		}
	private:
		unsigned block = 0;
	};

	md::PreparedMidiSysexTransfer sample()
	{
		auto result = md::prepareMidiSysexTransfer(md::test::sdsSample());
		require(result.has_value(), "generated SDS validation failed");
		return std::move(*result);
	}
	Bytes expectedSample()
	{
		// Independent mathematical PCM reference for sdsSample's generated ramp;
		// no decoder, private firmware layout, or guessed sample address.
		Bytes pcm;
		for(unsigned i = 0; i < 5201; ++i)
		{
			const auto word = (((i % 97) * 40 + 100) << 4) ^ 0x8000;
			pcm.push_back(uint8_t(word >> 8)); pcm.push_back(uint8_t(word));
		}
		return pcm;
	}
	bool contains(const Bytes& flash, const Bytes& pcm)
	{
		return std::search(flash.begin(), flash.end(), pcm.begin(), pcm.end()) != flash.end();
	}
	void mdTests(const std::string& home, const Bytes& baseline)
	{
		ProcessorHarness h(md::MachineModel::Machinedrum, home);
		require(h.withDevice([&](md::Device& d) { return d.getHardware().copyFlashData() == baseline; }),
			"processor did not materialize factory cache");
		auto data = sample();
		const auto early = h.begin();
		require(h.start(early, data) == Start::NotReady, "pre-boot import accepted");
		h.settledBoot();
		require(h.start(early, data, false) == Start::ConfirmationRequired, "SDS started without storage-readiness confirmation");
		const auto replacement = h.begin();
		require(h.start(early, data) == Start::StaleRequest, "superseded file dialog accepted");
		require(h.cancel(replacement), "pending cancellation failed");
		require(h.start(replacement, data) == Start::StaleRequest, "cancelled confirmation accepted");
		std::cout << "LIFECYCLE MD startup, coherent cache, confirmation, supersession, pending cancellation PASS\n";

		juce::MemoryBlock saved;
		h.audioProcessor.getStateInformation(saved);
		require(saved.getSize() != 0, "empty processor project state");
		const auto beforeRestore = h.begin();
		const auto epoch = h.withDevice([](md::Device& d) { return d.hardwareEpoch(); });
		h.restore(saved);
		require(h.withDevice([&](md::Device& d) { return d.hardwareEpoch() != epoch; }), "processor state did not replace hardware");
		require(h.progress().stage == Stage::Invalidated, "restore did not invalidate pending import");
		require(h.start(beforeRestore, data) == Start::StaleRequest, "late confirmation crossed project restore");
		h.settledBoot();

		const auto ticket = h.begin();
		require(h.start(ticket, data) == Start::Started, "processor SDS start failed");
		require(h.start(ticket, data) == Start::StaleRequest, "duplicate confirmation accepted");
		require(!h.withDevice([](md::Device& d) { return d.beginUserSysexImport().has_value(); }), "second sender stole active transfer");
		h.payloadStarted();
		const auto paused = h.progress();
		require(paused.stage == Stage::Transferring, "fixture did not pause an active transfer");
		for(unsigned i = 0; i < 10000; ++i)
		{
			const auto p = h.progress();
			require(p.serviceSerial == paused.serviceSerial && p.sent == paused.sent && p.state == paused.state,
				"control-plane polling advanced suspended transport");
		}
		h.terminal();
		const auto done = h.progress();
		require(done.stage == Stage::DeliveredUnverified && !done.contentsVerified && done.acknowledgedSamples == 1,
			"delivery/verification contract failed");
		h.frames(48000 * 10);
		const auto pcm = expectedSample();
		require(h.withDevice([&](md::Device& d) { return contains(d.getHardware().copyFlashData(), pcm); }),
			"complete processor SDS PCM missing");
		std::cout << "LIFECYCLE MD processor delivery, variable blocks, suspended callbacks, ACK, complete PCM PASS\n";

		// Transport ids do not represent project identity or pending dialogs.
		// Reject stale actions even when a newer transfer owns the MIDI wire.
		h.restore(saved);
		h.settledBoot();
		require(h.withDevice([&](md::Device& d) { return !contains(d.getHardware().copyFlashData(), pcm); }), "restored baseline contains imported sample");
		data = sample();
		const auto active = h.begin();
		require(h.start(active, data) == Start::Started, "post-restore start failed");
		require(!h.cancel(ticket), "old cancellation targeted new hardware");
		require(!h.withDevice([&](md::Device& d) { return d.resumeUserSysexImport(ticket, done.transferId, 0, true); }), "old resume targeted new hardware");
		Bytes staleRetired;
		require(!h.withDevice([&](md::Device& d) { return d.retireUserSysexImport(ticket, staleRetired); }) && staleRetired.empty(), "old retirement targeted new hardware");
		h.payloadStarted();
		h.restore(saved);
		require(h.progress().stage == Stage::Invalidated && !h.cancel(active), "active restore did not invalidate ownership");
		h.settledBoot();
		require(h.withDevice([&](md::Device& d) { return !contains(d.getHardware().copyFlashData(), pcm) && d.getHardware().isMidiIngressIdle(); }),
			"old payload leaked into replaced hardware");
		std::cout << "LIFECYCLE MD processor project replacement, active interruption, stale cancel/resume, no payload leak PASS\n";

		data = sample();
		const auto cancelled = h.begin();
		require(h.start(cancelled, data) == Start::Started && h.cancel(cancelled), "queued cancellation failed");
		h.terminal();
		require(h.progress().stage == Stage::Cancelled, "queued cancellation did not terminate");
		data = sample();
		const auto partial = h.begin();
		require(h.start(partial, data) == Start::Started, "partial cancellation fixture start failed");
		h.payloadStarted();
		require(h.cancel(partial), "partial payload cancellation failed");
		h.terminal();
		require(h.progress().stage == Stage::Cancelled, "partial payload cancellation did not terminate");
		h.frames(48000 * 10);
		require(h.withDevice([](md::Device& d) { return d.getHardware().isMidiIngressIdle(); }), "partial cancellation left MIDI blocked");
		data = sample();
		const auto failedRestore = h.begin();
		require(h.start(failedRestore, data) == Start::Started, "failed-restore fixture start failed");
		h.payloadStarted();
		// Invalid raw machine state exercises a genuine Device transaction failure,
		// not an outer JUCE container rejected before selecting any project.
		require(!h.processor.getPlugin().setState(Bytes{1, 2, 3}), "invalid state unexpectedly accepted");
		require(h.progress().stage == Stage::Invalidated, "failed restore left old import authorized");
		h.frames(48000);
		require(h.start(failedRestore, data) == Start::StaleRequest, "failed restore allowed late confirmation");
		std::cout << "LIFECYCLE MD queued/partial-payload cancellation and failed state preparation PASS\n";
	}
	void mmTests(const std::string& home)
	{
		ProcessorHarness a(md::MachineModel::Monomachine, home);
		const auto old = a.begin();
		// A minimal legal MM dump frame is sufficient for authorization tests.
		const Bytes frame{0xf0, 0, 0x20, 0x3c, 3, 0, 0x52, 2, 1, 0, 0, 0, 0, 5, 0xf7};
		auto data = md::prepareMidiSysexTransfer(frame, md::MachineModel::Monomachine);
		require(data.has_value(), "MM authorization fixture invalid");
		require(a.start(old, *data) == Start::NotReady, "pre-boot MM import accepted");
		a.settledBoot();
		require(a.start(old, *data, false) == Start::ConfirmationRequired, "MM receive confirmation bypassed");
		auto wrong = sample();
		require(a.start(old, wrong) == Start::WrongModel, "wrong-model prepared data accepted");
		ProcessorHarness b(md::MachineModel::Monomachine, home);
		const auto other = b.begin();
		require(other != old && b.start(old, *data) == Start::StaleRequest && !b.cancel(old), "ticket crossed processor instances");
		require(a.start(old, *data) == Start::Started, "confirmed MM transfer did not start");
		require(a.cancel(old), "MM cancellation failed");
		a.terminal();
		require(a.progress().stage == Stage::Cancelled, "MM cancellation did not finish");
		std::cout << "LIFECYCLE MM startup, receive confirmation, model, instance isolation and cancellation PASS\n";
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	if(argc != 5)
	{
		std::cout << "usage: mdSysexLifecycleTest <MD-ROM> <MD-factory.cache> <MM-ROM> <MM-patch.bin>\n";
		if(argc != 1) return 2;
		std::cout << (firmwareTestsRequired() ? "FAIL: required fixtures were not supplied\n" : "SKIP: explicit firmware fixtures required\n");
		return firmwareTestsRequired() ? 1 : SkipReturnCode;
	}
	try
	{
		juce::ScopedJuceInitialiser_GUI gui;
		const auto directory = juce::File::getCurrentWorkingDirectory().getChildFile("temp")
			.getNonexistentChildFile("md-mm-sysex-lifecycle", "", false);
		require(directory.getChildFile("nvram").createDirectory().wasOk(), "cannot create isolated fixture directory");
		std::cout << "LIFECYCLE isolated cache directory: " << directory.getFullPathName() << '\n';
		require(juce::File(argv[2]).copyFileTo(directory.getChildFile("nvram/md-uw-1.63-factory-v2.cache")), "cache copy failed");
		require(juce::File(argv[4]).copyFileTo(directory.getChildFile("nvram/mm-factory-live3-be.bin")), "patch copy failed");
		synthLib::RomLoader::addSearchPath(juce::File(argv[1]).getParentDirectory().getFullPathName().toStdString());
		synthLib::RomLoader::addSearchPath(juce::File(argv[3]).getParentDirectory().getFullPathName().toStdString());
		Bytes baseline;
		require(md::decodeFactoryFlashCache(baseline, load(argv[2]), load(argv[1])), "invalid MD cache");
		mdTests(directory.getFullPathName().toStdString(), baseline);
		mmTests(directory.getFullPathName().toStdString());
		std::cout << "LIFECYCLE ALL PASS (delivery is not a firmware commit guarantee)\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "LIFECYCLE FAIL: " << error.what() << '\n';
		return 1;
	}
}
