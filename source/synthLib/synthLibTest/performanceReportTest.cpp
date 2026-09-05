#include "synthLib/performanceReport.h"
#include "synthLib/plugin.h"
#include "synthLib/syntheticAudioTestDevice.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <set>

namespace synthLib
{
	struct RealtimeInstrumentationTestAccess
	{
		static void record(RealtimeInstrumentation& owner, RealtimeSlowCallback callback)
		{ owner.recordHostCallback(callback); }
		static void holdEventProducer(RealtimeInstrumentation& owner) { owner.m_eventProducer.test_and_set(); }
		static void releaseEventProducer(RealtimeInstrumentation& owner) { owner.m_eventProducer.clear(); }
	};
}
namespace
{
	using RI = synthLib::RealtimeInstrumentation;
	using Report = synthLib::PerformanceReport;
	void require(bool condition, const char* message)
	{ if(!condition) throw std::runtime_error(message); }

	void checkTraceAndOfflineAccounting()
	{
		RI ri;
		ri.setEnabled(true);
		ri.reset();
		synthLib::RealtimeSlowCallback callback;
		callback.durationNanoseconds = 200;
		callback.budgetNanoseconds = 100;
		callback.offline = true;
		synthLib::RealtimeInstrumentationTestAccess::record(ri, callback);
		callback.offline = false;
		synthLib::RealtimeInstrumentationTestAccess::record(ri, callback);
		auto summary = ri.snapshot();
		require(summary.offlineCallbackCount == 1 && summary.outerHostCallbackOverrunCount == 1,
			"offline work was counted as a realtime deadline miss");
		require(summary.realtimeBudgetHistogram[5] == 1, "load histogram included offline work");
		while(ri.popSlowCallback(callback)) {}
		{
			RI::CallbackScope scope(ri, 64, 48000);
			scope.setHostState(true, false);
			scope.setMidiInputSummary(2, 6);
			scope.setActiveOutputLayout(3, 6);
			RI::setCurrentDeviceContext(44100, 2, 100);
			ri.recordSynthProcess(200);
			ri.recordSynthProcessLockWait(30);
			ri.recordResampler(150, 100, 64, true);
			RI::recordCurrentCallbackJitCompilation();
			{ RI::DeferredCandidateScope deferred(64); RI::recordCurrentCallbackJitCompilation(); }
		}
		require(ri.popSlowCallback(callback), "interesting callback was not captured");
		require(callback.liveJitCompilations == 1 && callback.deferredJitCompilations == 1
			&& callback.dualMachine && callback.midiBytes == 6 && callback.midiEvents == 2
			&& callback.outputChannels == 6 && callback.lockWaitNanoseconds == 30
			&& callback.synthNanoseconds == 200 && callback.resamplerNanoseconds == 150
			&& callback.deviceNanoseconds == 100 && callback.deviceSampleRate == 44100
			&& callback.resamplerMode == 2 && callback.playing,
			"slow callback lost correlated phase or workload information");
		callback.durationNanoseconds = 200;
		callback.budgetNanoseconds = 100;
		for(size_t i = 0; i < RI::SlowCallbackCapacity + 7; ++i)
			synthLib::RealtimeInstrumentationTestAccess::record(ri, callback);
		require(ri.snapshot().slowCallbacksDropped == 7, "full trace queue did not report dropped events");
		size_t drained = 0;
		while(ri.popSlowCallback(callback)) ++drained;
		require(drained == RI::SlowCallbackCapacity, "trace queue exceeded its capacity");
	}

	void checkConcurrentConsumer()
	{
		RI ri;
		ri.setEnabled(true);
		std::atomic<bool> done{false};
		std::thread producer([&]
		{
			for(size_t i = 0; i < 20'000; ++i)
			{
				RI::CallbackScope callback(ri, 64, 48000);
				RI::recordCurrentCallbackJitCompilation();
			}
			done.store(true, std::memory_order_release);
		});
		synthLib::RealtimeSlowCallback callback;
		uint64_t previous = 0, count = 0;
		bool ordered = true;
		while(!done.load(std::memory_order_acquire))
		{
			while(ri.popSlowCallback(callback))
			{
				ordered &= callback.index > previous && callback.liveJitCompilations == 1;
				previous = callback.index;
				++count;
			}
			(void)ri.snapshot();
		}
		producer.join();
		while(ri.popSlowCallback(callback)) { ordered &= callback.index > previous; previous = callback.index; ++count; }
		require(ordered && count + ri.snapshot().slowCallbacksDropped == 20'000,
			"concurrent trace transport lost or tore records without accounting for them");
	}

	void checkEventTimeline()
	{
		RI ri;
		ri.setEnabled(false);
		require(ri.beginPanelInput(1, 0x25, 1).id == 0, "disabled capture recorded panel input");
		ri.setEnabled(true);
		ri.reset();
		const auto input = ri.beginPanelInput(1, 0x25, 1);
		ri.endPanelInput(input, 1, 0x25, 1, true);
		{
			RI::CallbackScope scope(ri, 64, 48000);
			scope.setHostState(false, false);
			RI::recordCurrentPanelDelivery(1, 0x25, 1);
		}
		using Kind = synthLib::RealtimeEventKind;
		synthLib::RealtimeEvent event;
		require(ri.popTimelineEvent(event) && event.kind == Kind::PanelInput
			&& event.inputId == input.id && event.callbackIndex == 0, "submission missing");
		const auto submittedAt = event.timeNanoseconds;
		require(ri.popTimelineEvent(event) && event.kind == Kind::PanelInputResult
			&& event.inputId == input.id && event.accepted, "input result not paired");
		require(ri.popTimelineEvent(event) && event.kind == Kind::HostTransport
			&& event.initial && event.transportKnown && !event.playing && event.callbackIndex == 1, "initial host state missing");
		require(ri.popTimelineEvent(event) && event.kind == Kind::PanelDelivery
			&& event.callbackIndex == 1 && event.timeNanoseconds >= submittedAt
			&& event.command == 0x25 && event.argument == 1, "delivery not aligned with input/callback");
		for(int i = 0; i < 3; ++i) { RI::CallbackScope scope(ri, 64, 48000); scope.setHostState(false, false); }
		require(!ri.popTimelineEvent(event), "unchanged host state flooded the timeline");
		{ RI::CallbackScope scope(ri, 64, 48000); scope.setHostState(true, false); }
		require(ri.popTimelineEvent(event) && !event.initial && event.playing && event.callbackIndex == 5, "host play transition missing");
		{ RI::CallbackScope scope(ri, 64, 48000, true); scope.setHostState(false, true, false); }
		require(ri.popTimelineEvent(event) && event.bypassed && event.offline && !event.transportKnown,
			"unknown/bypassed/offline host state was misrepresented");
		const auto previousSessionInput = ri.beginPanelInput(1, 0x25, 0);
		while(ri.popTimelineEvent(event)) {}
		ri.reset();
		ri.endPanelInput(previousSessionInput, 1, 0x25, 0, true);
		require(!ri.popTimelineEvent(event), "old input result leaked into a new capture");
		{ RI::CallbackScope scope(ri, 64, 48000); scope.setHostState(false, true, false); }
		require(ri.popTimelineEvent(event), "new session lost its initial transport snapshot");
		ri.reset();
		for(size_t i = 0; i < RI::TimelineCapacity + 5; ++i) ri.beginPanelInput(1, 0x25, 1);
		require(ri.snapshot().timelineEventsDropped == 5, "timeline overflow was not counted");
		size_t count = 0;
		while(ri.popTimelineEvent(event)) ++count;
		require(count == RI::TimelineCapacity, "timeline capacity was not bounded");
		synthLib::RealtimeInstrumentationTestAccess::holdEventProducer(ri);
		ri.beginPanelInput(1, 0x25, 1);
		synthLib::RealtimeInstrumentationTestAccess::releaseEventProducer(ri);
		require(ri.snapshot().timelineEventsDropped == 6 && !ri.popTimelineEvent(event),
			"contended producer must drop and return without waiting");
		synthLib::RealtimeInstrumentationTestAccess::holdEventProducer(ri);
		{ RI::CallbackScope scope(ri, 64, 48000); scope.setHostState(true, false); }
		synthLib::RealtimeInstrumentationTestAccess::releaseEventProducer(ri);
		require(!ri.popTimelineEvent(event), "contended transport producer unexpectedly queued");
		{ RI::CallbackScope scope(ri, 64, 48000); scope.setHostState(true, false); }
		require(ri.popTimelineEvent(event) && event.initial && event.playing,
			"dropped initial transport state was not retried");
	}

	void checkConcurrentEventProducers()
	{
		RI ri;
		ri.setEnabled(true);
		std::atomic<unsigned> finished{0};
		std::vector<std::thread> producers;
		for(unsigned source = 0; source < 4; ++source)
			producers.emplace_back([&, source] {
				for(unsigned i = 0; i < 3000; ++i) {
					const auto input = ri.beginPanelInput(source, static_cast<uint8_t>(source), static_cast<uint8_t>(i));
					ri.endPanelInput(input, source, static_cast<uint8_t>(source), static_cast<uint8_t>(i), true);
				}
				finished.fetch_add(1, std::memory_order_release);
			});
		std::set<uint64_t> sequences;
		bool intact = true;
		size_t count = 0;
		auto drain = [&] {
			synthLib::RealtimeEvent event;
			while(ri.popTimelineEvent(event)) {
				intact &= event.inputId != 0 && event.model == event.command && event.model < 4;
				intact &= sequences.insert(event.sequence).second;
				++count;
			}
		};
		while(finished.load(std::memory_order_acquire) != 4) drain();
		for(auto& producer : producers) producer.join();
		drain();
		require(intact && count + ri.snapshot().timelineEventsDropped == 24000
			&& ri.snapshot().timelineEvents == 24000, "multi-producer events tore or disappeared without accounting");
	}

	void checkProcessingIntegration()
	{
		for(float rate : {44100.f, 48000.f, 96000.f})
		{
			synthLib::test::SyntheticAudioDevice device(2, 6, 0);
			synthLib::Plugin plugin(&device, [](synthLib::Device*) {});
			plugin.setHostSamplerate(rate, 44100.f);
			plugin.setBlockSize(128);
			plugin.reserveMidiEventCapacity();
			auto& ri = plugin.getRealtimeInstrumentation();
			ri.setEnabled(true);
			synthLib::TAudioInputs inputs{};
			synthLib::TAudioOutputs outputs{};
			std::array<std::array<float, 128>, 6> storage{};
			for(size_t i = 0; i < storage.size(); ++i) outputs[i] = storage[i].data();
			for(int i = 0; i < 32; ++i)
			{
				RI::CallbackScope callback(ri, 128, rate);
				callback.setActiveOutputLayout(3, 6);
				plugin.process(inputs, outputs, 128, 0, 0, false);
			}
			const auto summary = ri.snapshot();
			require(summary.outerHostCallbackCount == 32 && summary.synthProcessCount == 32
				&& summary.resamplerCallCount == 32 && summary.resamplerHostFrames == 4096,
				"plugin processing was not captured");
			require(summary.resamplingActiveCallbackCount == (rate == 44100.f ? 0 : 32),
				"resampling was misclassified");
			synthLib::RealtimeSlowCallback callback;
			require(ri.popSlowCallback(callback) && callback.deviceSampleRate == 44100
				&& callback.sampleRate == rate && callback.outputChannels == 6
				&& callback.synthNanoseconds >= callback.resamplerNanoseconds
				&& callback.resamplerNanoseconds >= callback.deviceNanoseconds
				&& callback.deviceNanoseconds > 0, "pipeline timings or context are missing");
		}
	}

	void waitFor(Report& report, Report::Status target)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while(report.status() != target && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		require(report.status() == target, "report worker did not reach its expected state");
	}

	std::string read(const std::string& path)
	{
		std::ifstream input(path);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	void checkReport(const std::string& path)
	{
		RI ri;
		const auto callerThread = std::this_thread::get_id();
		std::atomic<bool> formattedOffThread{false};
		Report report(ri, [&](const synthLib::RealtimeEvent&) {
			formattedOffThread.store(std::this_thread::get_id() != callerThread);
			return Report::Context{{"buttonsDown", "Play"}};
		});
		report.start(path, {{"host", "Test \"Host\"\n"}, {"cpu", "test CPU"}});
		waitFor(report, Report::Status::Recording);
		const auto panelInput = ri.beginPanelInput(1, 0x25, 1);
		ri.endPanelInput(panelInput, 1, 0x25, 1, true);
		{
			RI::CallbackScope callback(ri, 64, 48000);
			RI::recordCurrentCallbackJitCompilation();
		}
		report.stop();
		require(!ri.isEnabled() && report.status() == Report::Status::Stopped, "stop left recording enabled");
		const auto content = read(path);
		require(content.find("\"phase\":\"submitted\"") != std::string::npos
			&& content.find("\"buttonsDown\":\"Play\"") != std::string::npos
			&& content.find("\"accepted\":true") != std::string::npos && formattedOffThread.load(),
			"timeline output or off-thread event labeling failed");
		require(content.find("Test \\\"Host\\\"\\u000a") != std::string::npos,
			"report context is not JSON escaped");
		require(content.find("\"type\":\"callback\"") != std::string::npos
			&& content.find("\"liveJitCompilations\":1") != std::string::npos
			&& content.find("\"reason\":\"stopped\"") != std::string::npos,
			"final report is missing captured work or stop reason");
		report.start(path + "/missing/report.jsonl", {});
		waitFor(report, Report::Status::Error);
		require(!ri.isEnabled(), "file error left recording enabled");
		report.stop();

		Report::Limits limits;
		limits.bytes = 16384;
		limits.duration = std::chrono::milliseconds(30);
		limits.flushInterval = std::chrono::milliseconds(10);
		report.start(path + ".limited", {}, limits);
		waitFor(report, Report::Status::LimitReached);
		require(!ri.isEnabled() && read(path + ".limited").size() <= limits.bytes,
			"automatic capture limit did not bound output or stop instrumentation");
		report.stop();
		for(int i = 0; i < 10; ++i)
		{
			report.start(path + ".restart", {});
			report.stop();
			require(!ri.isEnabled(), "immediate stop raced worker startup");
		}
		limits.duration = std::chrono::seconds(5);
		report.start(path + ".size-limit", {}, limits);
		waitFor(report, Report::Status::Recording);
		for(size_t i = 0; i < 2048; ++i)
		{
			RI::CallbackScope callback(ri, 64, 48000);
			RI::recordCurrentCallbackJitCompilation();
		}
		waitFor(report, Report::Status::LimitReached);
		require(read(path + ".size-limit").size() <= limits.bytes,
			"trace volume exceeded the file size cap");
		report.stop();
		std::remove((path + ".size-limit").c_str());
		std::remove((path + ".limited").c_str());
		std::remove((path + ".restart").c_str());
	}
}
int main(int argc, char** argv)
{
	try
	{
		require(argc == 2, "expected a report output filename");
		checkTraceAndOfflineAccounting();
		checkConcurrentConsumer();
		checkEventTimeline();
		checkConcurrentEventProducers();
		checkProcessingIntegration();
		checkReport(argv[1]);
		std::cout << "performance report: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "performance report: " << error.what() << '\n';
		return 1;
	}
}
