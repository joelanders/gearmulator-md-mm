#include "fixture_helpers.h"

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "usage: mdLatencyFixture MM_ROM MM_PATCH_RAM NEW_OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        const auto out = baseLib::filesystem::validatePath(argv[3]);
        require(!baseLib::filesystem::exists(out), "output directory must be new");
        require(baseLib::filesystem::createDirectory(out.substr(0, out.size() - 1)), "cannot create output directory");
        synthLib::DeviceCreateParams params;
        params.romName = argv[1];
        params.homePath = out;
        params.customData = md::deviceCustomData(md::MachineModel::Monomachine);
        require(baseLib::filesystem::readFile(params.romData, argv[1]), "cannot read ROM");
        Bytes seed;
        require(baseLib::filesystem::readFile(seed, argv[2]), "cannot read seed");
        auto device = std::make_unique<md::Device>(params, seed);
        require(device->isValid(), "invalid firmware device");
        auto& hardware = device->getHardware();
        advance(hardware, 20 * 44100);
        require(hardware.isFirmwareMidiReady(), "firmware did not become ready");
        auto globalWire = request(hardware, 0x50);
        auto global = unpack(globalWire);
        require(global.size() >= 0x108, "unexpected global layout");
        std::cout << "Initial external sync=" << unsigned(global[5])
                  << " transport in=" << unsigned(global[6]) << std::endl;
        global[1] = 0; // Track one on MIDI channel 1.
        global[5] |= 1; global[6] = 1;
        globalWire = pack(globalWire, global);
        save(out + "global.syx", globalWire);
        auto patternWire = request(hardware, 0x67);
        auto pattern = unpack(patternWire);
        require(pattern.size() == 0x1978, "unexpected pattern layout");
        std::fill(pattern.begin(), pattern.begin() + 0x270, 0);
        std::fill(pattern.begin() + 0x274, pattern.begin() + 0x2a4, 0);
        std::fill(pattern.begin() + 0x2a4, pattern.begin() + 0x424, 60);
        pattern[0x424] = 16; pattern[0x425] = 0; pattern[0x426] = 0;
        pattern[0x427] = 0; pattern[0x556] = 0;
        std::fill(pattern.begin() + 0x428, pattern.begin() + 0x446, 0);
        std::fill(pattern.begin() + 0x452, pattern.begin() + 0x458, 0);
        auto emptyPatternWire = pack(patternWire, pattern);
        save(out + "empty-pattern.syx", emptyPatternWire);
        enterReceive(hardware);
        send(hardware, globalWire); advance(hardware, 44100);
        send(hardware, emptyPatternWire); advance(hardware, 44100);
        for(unsigned n=0;n<4;++n) tap(hardware, md::PanelControl::Exit);
        auto actualGlobal = unpack(request(hardware, 0x50));
        if(actualGlobal != global) {
            std::cout << "Global bytes requested=" << global.size() << " actual=" << actualGlobal.size() << std::endl;
            for(size_t i=0;i<std::min(global.size(),actualGlobal.size());++i)
                if(global[i]!=actualGlobal[i]) std::cout << "global offset " << i << ": " << unsigned(global[i]) << " != " << unsigned(actualGlobal[i]) << std::endl;
        }
        require(actualGlobal == global, "global readback differs");
        require(unpack(request(hardware, 0x67)) == pattern, "empty pattern readback differs");
        send(hardware, {0xf0,0,0x20,0x3c,3,0,0x71,1,0,0xf7});
        send(hardware, {0xf0,0,0x20,0x3c,3,0,0x71,4,0,0xf7});
        advance(hardware, 44100);
        tap(hardware, md::PanelControl::Track1);
        tap(hardware, md::PanelControl::Record);
        for (auto control : {md::PanelControl::Trigger1, md::PanelControl::Trigger5,
                             md::PanelControl::Trigger9, md::PanelControl::Trigger13}) tap(hardware, control);
        tap(hardware, md::PanelControl::Record);
        advance(hardware, 44100);
        // Assign GND-SIN and route track one to AB through public Appendix C
        // commands, independent of the seed's previous instrument/effects.
        send(hardware, {0xf0,0,0x20,0x3c,3,0,0x5b,0,1,1,0xf7});
        advance(hardware, 44100);
        send(hardware, {0xf0,0,0x20,0x3c,3,0,0x5c,0,1,0,0xf7});
        require(hardware.sendMidi({synthLib::MidiEventSource::Host,0xb0,7,100}), "level CC rejected");
        advance(hardware, 44100);
        // Public CC56..59 are AMP attack/hold/decay/release. A short decay
        // makes every sequencer beat measurable without changing DSP memory.
        // CC84 uses a bipolar send: 64 is zero; CC85=0 removes feedback.
        const uint8_t hold = 0, decay = 32, release = 0;
        for(auto pair : {std::pair<uint8_t,uint8_t>{56,0}, {57,hold}, {58,decay}, {59,release}, {84,64}, {85,0}})
            require(hardware.sendMidi({synthLib::MidiEventSource::Host,
                static_cast<uint8_t>(0xb0 | global[1]), pair.first, pair.second}), "envelope CC rejected");
        advance(hardware, 44100);
        send(hardware, {0xf0,0,0x20,0x3c,3,0,0x59,0,0xf7});
        advance(hardware, 44100);
        auto kitWire = request(hardware, 0x52);
        auto kit = unpack(kitWire);
        require(kit.size() > 0x1c1 && kit[0x1c1] == 1 && kit[0x19] == 0 && kit[0x1a] == hold
                && kit[0x1b] == decay && kit[0x1c] == release, "saved envelope did not match CC values");
        require(kit[0x2d] == 64 && kit[0x2e] == 0, "saved delay send/feedback did not match CC values");
        save(out + "kit.syx", kitWire);
        auto finalPattern = request(hardware, 0x67);
        auto finalData = unpack(finalPattern);
        require(finalData.size() == 0x1978, "unexpected final pattern layout");
        unsigned trigs = 0;
        for(unsigned i=0;i<8;++i) for(unsigned bit=0;bit<8;++bit) trigs += (finalData[i] >> bit) & 1;
        require(trigs == 4, "panel did not create four track-one amplitude trigs");
        save(out + "pattern.syx", finalPattern);
        save(out + "mm-factory-live3-be.bin", hardware.copyPatchRam());
        synthLib::Plugin plugin(device.get(), [](synthLib::Device*){});
        plugin.setHostSamplerate(44100,44100); plugin.setBlockSize(256); plugin.setLatencyBlocks(0);
        plugin.reserveMidiEventCapacity();
        std::array<std::array<float,256>,2> audio{};
        synthLib::TAudioOutputs outputs{};
        outputs[0]=audio[0].data(); outputs[1]=audio[1].data();
        std::ofstream capture(out + "clock-test.f32",std::ios::binary);
        constexpr uint32_t total = 6 * 44100, start = 2 * 44100;
        double peak=0;
        for(uint32_t sample=0; sample<total;) {
            auto count=std::min(256u,total-sample);
            if(sample<start) count=std::min(count,start-sample);
            plugin.process({},outputs,count,120,sample<start?0:(sample-start)/22050.0,sample>=start);
            for(unsigned i=0;i<count;++i) for(unsigned ch=0;ch<2;++ch) {
                peak=std::max(peak,std::abs(double(audio[ch][i])));
                capture.write(reinterpret_cast<const char*>(&audio[ch][i]),sizeof(float));
            }
            sample+=count;
        }
        std::cout << "Clock test starts at sample " << start << "; peak=" << peak << std::endl;
        require(peak > 1e-4, "prepared sequence produced no audible clock-driven audio");
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}
