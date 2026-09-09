#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>
#include <cstdlib>
#include <limits>
#if JUCE_MAC
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

struct Event { int64_t sample; juce::MidiMessage message; };
struct BlockRecord { int64_t sample; int size; double lateMs; double renderMs; int latency; };

struct TestPlayHead final : juce::AudioPlayHead
{
    PositionInfo position;
    juce::Optional<PositionInfo> getPosition() const override { return position; }
};

juce::var memorySnapshot()
{
#if JUCE_MAC
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return {};
    auto* result = new juce::DynamicObject();
    result->setProperty("physical_footprint_bytes", (juce::int64)info.phys_footprint);
    result->setProperty("resident_bytes", (juce::int64)info.resident_size);
    result->setProperty("virtual_bytes", (juce::int64)info.virtual_size);
    return juce::var(result);
#else
    return {};
#endif
}

int main(int argc, char** argv)
{
    if (argc < 6 || argc > 16) {
        std::fprintf(stderr, "usage: latency_host PLUGIN OUTPUT_PREFIX RATE BLOCK SECONDS [reprepare_seconds] [variable] [offline_after] [paced|fast] [note_number] [phase] [notes|chords|input|transport] [messages|no-messages] [restore_seconds]\n");
        return 2;
    }
    const juce::String pluginPath(argv[1]), prefix(argv[2]);
    const double rate = std::atof(argv[3]), seconds = std::atof(argv[5]);
    const int block = std::atoi(argv[4]);
    const double reprepare = argc > 6 ? std::atof(argv[6]) : -1;
    const bool variable = argc > 7 && std::string(argv[7]) == "variable";
    const double offlineAfter = argc > 8 ? std::atof(argv[8]) : -1;
    const bool paceOffline = argc > 9 && std::string(argv[9]) == "paced";
    const int noteNumber = argc > 10 ? std::atoi(argv[10]) : 60;
    const int phase = argc > 11 ? std::atoi(argv[11]) : 0;
    const std::string scenario = argc > 12 ? argv[12] : "notes";
    const bool transport = scenario == "transport";
    const bool chords = scenario == "chords";
    const bool input = scenario == "input";
    const bool suppressMessages = argc > 13 && std::string(argv[13]) == "no-messages";
    const double restoreAt = argc > 14 ? std::atof(argv[14]) : -1;
    if (!std::getenv("GEARMULATOR_DATA_ROOT") || !*std::getenv("GEARMULATOR_DATA_ROOT")) {
        std::fprintf(stderr, "Use run.py to provide an isolated plugin data directory\n"); return 2;
    }
    if (!std::isfinite(rate) || !std::isfinite(seconds) || rate > 192000 || seconds > 600
        || !std::isfinite(reprepare) || !std::isfinite(offlineAfter) || !std::isfinite(restoreAt)
        || block > 8192 || (scenario != "notes" && !transport && !chords && !input)) return 2;
    for (auto suffix : {".wav", ".input.wav", ".blocks.csv", ".json"})
        if (juce::File(prefix + suffix).exists()) {
            std::fprintf(stderr, "Output prefix must be unused\n"); return 2;
        }
    if (phase < 0 || phase >= block || noteNumber < 0 || noteNumber > 127) return 2;
    if (rate < 8000 || block < 1 || seconds < 20) return 2;
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    juce::AudioPluginFormatManager formats;
    const bool isAU = pluginPath.endsWith(".component");
#if JUCE_PLUGINHOST_AU
    if (isAU) formats.addFormat(new juce::AudioUnitPluginFormat());
    else
#else
    if (isAU) { std::fprintf(stderr, "AU requires macOS\n"); return 2; }
#endif
    formats.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> descriptions;
    formats.getFormat(0)->findAllTypesForFile(descriptions, pluginPath);
    if (descriptions.size() != 1) {
        std::fprintf(stderr, "Expected one description, got %d\n", descriptions.size());
        return 1;
    }
    juce::String error;
    auto instance = formats.createPluginInstance(*descriptions[0], rate, block, error);
    if (!instance) { std::fprintf(stderr, "%s\n", error.toRawUTF8()); return 1; }
    instance->setPlayConfigDetails(input ? 2 : 0, 2, rate, block);
    TestPlayHead playhead;
    if (transport) instance->setPlayHead(&playhead);
    instance->setNonRealtime(offlineAfter == 0);
    instance->prepareToPlay(rate, block);
    const auto memoryPrepared = memorySnapshot();
    const int initialLatency = instance->getLatencySamples();
    std::printf("Loaded %s %s; rate=%.0f block=%d reported=%d (%.6f ms)\n",
        descriptions[0]->name.toRawUTF8(), descriptions[0]->pluginFormatName.toRawUTF8(),
        rate, block, initialLatency, initialLatency * 1000.0 / rate);
    std::fflush(stdout);

    const int64_t total = std::llround(seconds * rate);
    std::vector<Event> events;
    juce::Array<juce::var> noteRecords;
    juce::Array<juce::var> transportRecords;
    juce::Array<juce::var> stateRecords;
    if(input && noteNumber==36) {
        // MD INP-GA=80, ordinary SPS-1 machine, initialize synth/effects/routing.
        const juce::uint8 assign[]{0,0x20,0x3c,2,0,0x5b,0,80,0,2};
        events.push_back({std::llround(8*rate),juce::MidiMessage::createSysExMessage(assign,sizeof(assign))});
        // Track-one machine VOL/GATE and level. The note below arms the gate.
        for(auto cc : {std::pair<int,int>{16,64},{17,0},{8,100}})
            events.push_back({std::llround(8.2*rate),juce::MidiMessage::controllerEvent(1,cc.first,cc.second)});
    }
    if (input && noteNumber == 60) {
        // Monomachine Appendix C: track 1 FX THRU, AB outputs, A+B inputs.
        const juce::uint8 assign[]{0,0x20,0x3c,3,0,0x5b,0,12,1};
        const juce::uint8 route[]{0,0x20,0x3c,3,0,0x5c,0,1,3};
        events.push_back({std::llround(8*rate),juce::MidiMessage::createSysExMessage(assign,sizeof(assign))});
        events.push_back({std::llround(9*rate),juce::MidiMessage::createSysExMessage(route,sizeof(route))});
        for(auto cc : {std::pair<int,int>{55,127},{56,0},{57,0},{58,127},{59,127},{7,100},{84,64},{85,0}})
            events.push_back({std::llround(9.2*rate),juce::MidiMessage::controllerEvent(1,cc.first,cc.second)});
    }
    // Identical fresh instances and sample timeline; move only the note offsets.
    for (double time = 10.0; !transport && time + 2.0 < seconds; time += chords ? .251 : input ? seconds : 3.137) {
        const int64_t base = (std::llround(time * rate) / block) * block;
        const int64_t at = base + phase;
        for (int voice=0; voice<(chords ? 6 : 1); ++voice) {
            const int channel = noteNumber == 36 ? 1 : voice + 1;
            const int pitch = noteNumber + (noteNumber == 36 ? voice : 0);
            events.push_back({at, juce::MidiMessage::noteOn(channel, pitch, (juce::uint8)100)});
            events.push_back({at + std::llround((input ? seconds - 11 : .15) * rate), juce::MidiMessage::noteOff(channel, pitch)});
        }
        auto* n = new juce::DynamicObject();
        n->setProperty("sample", (juce::int64)at);
        n->setProperty("seconds", at / rate);
        noteRecords.add(juce::var(n));
    }
    std::stable_sort(events.begin(), events.end(), [](auto& a, auto& b) { return a.sample < b.sample; });
    juce::Array<juce::var> midiRecords;
    for (const auto& event : events) {
        auto* record = new juce::DynamicObject();
        record->setProperty("sample", (juce::int64)event.sample);
        record->setProperty("bytes", juce::String::toHexString(event.message.getRawData(), event.message.getRawDataSize()));
        midiRecords.add(juce::var(record));
    }
    juce::AudioBuffer<float> capture(2, (int)total);
    capture.clear();
    juce::AudioBuffer<float> inputCapture(input ? 2 : 0, input ? (int)total : 0);
    if (input) {
        inputCapture.clear();
        uint32_t state = 0x19e38ab7;
        // A deterministic low-level broadband probe, after the THRU gate opens.
        // Save the actual input alongside output for an independent correlation.
        for (int64_t sample=std::llround(10.5*rate); sample<std::llround((seconds-2)*rate); ++sample) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            const float value = (double(state)/4294967296.0 - .5) * .1;
            for(int ch=0;ch<2;++ch) inputCapture.setSample(ch,(int)sample,value);
        }
    }
    juce::AudioBuffer<float> audio(2, block);
    juce::MidiBuffer midi;
    midi.ensureSize(4096);
    std::vector<BlockRecord> blocks;
    blocks.reserve((size_t)(total / std::max(1, block/2) + 100));
    std::atomic<bool> done{false};
    std::atomic<bool> lifecycleCall{false};
    std::thread render([&] {
        using Clock = std::chrono::steady_clock;
#if JUCE_MAC
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        const double ticksPerSecond = 1e9 * tb.denom / tb.numer;
        thread_time_constraint_policy_data_t policy{};
        policy.period = (uint32_t)(ticksPerSecond * block / rate);
        policy.computation = std::min(policy.period/2, (uint32_t)(ticksPerSecond*.001));
        policy.constraint = policy.period;
        policy.preemptible = true;
        if (offlineAfter != 0 || paceOffline) {
            const auto policyResult = thread_policy_set(pthread_mach_thread_np(pthread_self()),
                THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy),
                THREAD_TIME_CONSTRAINT_POLICY_COUNT);
            std::printf("Host realtime scheduling result=%d\n", policyResult);
        }
        std::fflush(stdout);
        const uint64_t machEpoch = mach_absolute_time();
#endif
        const auto epoch = Clock::now();
        size_t eventIndex = 0, blockIndex = 0;
        bool reprepared = false;
        bool offline = offlineAfter == 0;
        bool stateSaved = false, stateRestored = false;
        juce::MemoryBlock savedState;
        int transportStage = -1;
        double transportPpq = 0, transportBpm = 120;
        for (int64_t start = 0; start < total; ++blockIndex) {
            const int sizes[] = {block, std::max(1, block/2), block-1, std::max(1, block/3)};
            const int count = (int)std::min<int64_t>(variable ? sizes[blockIndex % 4] : block, total - start);
            const auto due = epoch + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(start/rate));
            if (offlineAfter >= 0 && !offline && start >= std::llround(offlineAfter*rate)) {
                lifecycleCall.store(true);
                instance->releaseResources();
                instance->setNonRealtime(true);
                instance->prepareToPlay(rate, block);
                offline = true;
                lifecycleCall.store(false);
                if (!paceOffline) {
                    // An unpaced offline renderer must not monopolize the CPU
                    // with a realtime time-constraint policy.
#if JUCE_MAC
                    const auto policyResult = thread_policy_set(pthread_mach_thread_np(pthread_self()),
                        THREAD_STANDARD_POLICY, nullptr, 0);
                    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
                    std::printf("Host offline standard scheduling result=%d\n", policyResult);
#endif
                }
            }
            if (!offline || paceOffline) {
#if JUCE_MAC
                mach_wait_until(machEpoch + (uint64_t)std::llround(start/rate*ticksPerSecond));
#else
                std::this_thread::sleep_until(due);
#endif
            }
            const auto arrived = Clock::now();
            if (restoreAt >= 0 && ((!stateSaved && start >= std::llround(8*rate))
                || (stateSaved && !stateRestored && start >= std::llround(restoreAt*rate)))) {
                const auto operationStart = Clock::now();
                lifecycleCall.store(true);
                const bool restoring = stateSaved;
                if (restoring) { instance->setStateInformation(savedState.getData(), (int)savedState.getSize()); stateRestored=true; }
                else { instance->getStateInformation(savedState); stateSaved=true; }
                lifecycleCall.store(false);
                auto* record = new juce::DynamicObject();
                record->setProperty("sample", (juce::int64)start);
                record->setProperty("operation", restoring ? "restore" : "save");
                record->setProperty("bytes", (juce::int64)savedState.getSize());
                record->setProperty("duration_ms", std::chrono::duration<double,std::milli>(Clock::now()-operationStart).count());
                stateRecords.add(juce::var(record));
            }
            if (reprepare >= 0 && !reprepared && start >= std::llround(reprepare*rate)) {
                lifecycleCall.store(true);
                instance->releaseResources();
                instance->prepareToPlay(rate, block);
                lifecycleCall.store(false);
                reprepared = true;
            }
            midi.clear();
            while (eventIndex < events.size() && events[eventIndex].sample < start + count) {
                midi.addEvent(events[eventIndex].message, (int)(events[eventIndex].sample-start));
                ++eventIndex;
            }
            audio.setSize(2, count, false, false, true);
            audio.clear();
            if(input) for(int ch=0;ch<2;++ch) audio.copyFrom(ch,0,inputCapture,ch,(int)start,count);
            if (transport) {
                const int stage = start < std::llround(10 * rate) ? 0
                    : start < std::llround(14 * rate) ? 1
                    : start < std::llround(16.173 * rate) ? 2
                    : start < std::llround((seconds - 2) * rate) ? 3 : 4;
                if (stage != transportStage) {
                    if (stage == 1 || stage == 3) transportPpq = 0;
                    if (stage == 2) transportBpm = 180;
                    auto* record = new juce::DynamicObject();
                    record->setProperty("sample", (juce::int64)start);
                    record->setProperty("stage", stage);
                    record->setProperty("ppq", transportPpq);
                    record->setProperty("bpm", transportBpm);
                    record->setProperty("playing", stage > 0 && stage < 4);
                    transportRecords.add(juce::var(record));
                    transportStage = stage;
                }
                playhead.position.setTimeInSamples(start);
                playhead.position.setTimeInSeconds(start / rate);
                playhead.position.setPpqPosition(transportPpq);
                playhead.position.setBpm(transportBpm);
                playhead.position.setTimeSignature(juce::AudioPlayHead::TimeSignature{4, 4});
                playhead.position.setIsPlaying(stage > 0 && stage < 4);
            }
            const auto before = Clock::now();
            instance->processBlock(audio, midi);
            const auto after = Clock::now();
            for (int ch = 0; ch < 2; ++ch) capture.copyFrom(ch, (int)start, audio, ch, 0, count);
            blocks.push_back({start, count,
                std::chrono::duration<double, std::milli>(arrived-due).count(),
                std::chrono::duration<double, std::milli>(after-before).count(),
                instance->getLatencySamples()});
            if (transport && transportStage > 0 && transportStage < 4)
                transportPpq += count * transportBpm / (60 * rate);
            start += count;
        }
        done.store(true);
    });
    while (!done.load()) {
        if (suppressMessages && !lifecycleCall.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        else juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
    render.join();
    const auto memoryRendered = memorySnapshot();
    const int finalLatency = instance->getLatencySamples();
    instance->releaseResources();
    instance.reset();

    double rawPeak = 0;
    for (int ch = 0; ch < capture.getNumChannels(); ++ch)
        for (int sample = 0; sample < capture.getNumSamples(); ++sample) {
            rawPeak = std::max(rawPeak, std::abs(double(capture.getSample(ch, sample))));
            if (!std::isfinite(capture.getSample(ch, sample))) {
                std::fprintf(stderr, "Non-finite plugin output\n"); return 1;
            }
        }
    juce::File wav(prefix + ".wav");
    if (wav.existsAsFile()) { std::fprintf(stderr, "Refusing to replace existing recording\n"); return 1; }
    std::unique_ptr<juce::OutputStream> stream = wav.createOutputStream();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), rate, 2, 24, {}, 0));
    if (writer) stream.release();
    if (!writer || !writer->writeFromAudioSampleBuffer(capture, 0, (int)total)) return 1;
    writer.reset();
    if (input) {
        juce::File inputFile(prefix + ".input.wav");
        std::unique_ptr<juce::OutputStream> inputStream = inputFile.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> inputWriter(format.createWriterFor(inputStream.get(), rate, 2, 24, {}, 0));
        if (inputWriter) inputStream.release();
        if(!inputWriter || !inputWriter->writeFromAudioSampleBuffer(inputCapture,0,(int)total)) return 1;
    }
    juce::FileOutputStream csv(juce::File(prefix + ".blocks.csv"));
    csv.writeText("sample,count,late_ms,render_ms,reported_latency_samples\n", false, false, nullptr);
    for (const auto& b : blocks) {
        auto row = juce::String((juce::int64)b.sample) + "," + juce::String(b.size) + ","
            + juce::String(b.lateMs, 6) + "," + juce::String(b.renderMs, 6) + "," + juce::String(b.latency) + "\n";
        csv.writeText(row, false, false, nullptr);
    }
    juce::DynamicObject::Ptr receipt = new juce::DynamicObject();
    receipt->setProperty("plugin", juce::File(pluginPath).getFileName());
    receipt->setProperty("audio_finite_before_quantization", true);
    receipt->setProperty("host_os", juce::SystemStats::getOperatingSystemName());
    receipt->setProperty("format", isAU ? "AU" : "VST3");
    receipt->setProperty("sample_rate", rate);
    receipt->setProperty("block_size", block);
    receipt->setProperty("seconds", seconds);
    receipt->setProperty("variable_blocks", variable);
    receipt->setProperty("reprepare_seconds", reprepare);
    receipt->setProperty("offline_after_seconds", offlineAfter);
    receipt->setProperty("pace_offline", paceOffline);
    receipt->setProperty("initial_latency_samples", initialLatency);
    receipt->setProperty("final_latency_samples", finalLatency);
    receipt->setProperty("memory_after_prepare", memoryPrepared);
    receipt->setProperty("memory_after_render", memoryRendered);
    receipt->setProperty("notes", noteRecords);
    receipt->setProperty("midi_input", midiRecords);
    receipt->setProperty("audio_peak_before_quantization", rawPeak);
    receipt->setProperty("note_number", noteNumber);
    receipt->setProperty("note_phase", phase);
    receipt->setProperty("scenario", juce::String(scenario));
    receipt->setProperty("state_operations", stateRecords);
    receipt->setProperty("suppress_message_loop", suppressMessages);
    receipt->setProperty("transport", transportRecords);
    receipt->setProperty("patch_preparation", "See run.json for initial data hashes; input setup uses SysEx/CC; notes velocity100 duration150ms");
    juce::File(prefix + ".json").replaceWithText(juce::JSON::toString(juce::var(receipt.get()), true));
    std::printf("Recorded %s\n", wav.getFullPathName().toRawUTF8());
    return 0;
}
