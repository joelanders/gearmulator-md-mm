#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdsysexautomation.h"
#include "dsp56kBase/logging.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <cmath>
#include <ctime>
#include <stdexcept>
#include <map>

std::vector<uint8_t> read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot read " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}
std::ofstream checkpoints("checkpoints.bin",std::ios::binary);
uint64_t hostFrames=0;
uint64_t stateHash(dsp56k::DSP& dsp) {
    const auto& r=dsp.regs(); uint64_t hash=1469598103934665603ull;
    auto add=[&](uint64_t v) {hash^=v;hash*=1099511628211ull;};
    add(r.x.var);add(r.y.var);add(r.a.var);add(r.b.var);
    for(unsigned i=0;i<8;++i) {add(r.r[i].var);add(r.n[i].var);add(r.m[i].var);add(r.mMask[i]);add(r.mModulo[i]);}
    add(r.sr.var);add(r.omr.var);add(r.pc.var);add(r.la.var);add(r.lc.var);add(r.sp.var);add(r.sc.var);
    for(const auto& v:r.ss) add(v.var);
    add(r.sz.var);add(r.vba.var);add(r.ep.var); return hash;
}
void checkpoint(md::Hardware& h) {
    if(hostFrames==1025112 || hostFrames==1025240 || hostFrames==2341784 || hostFrames==2341912) {
        for(auto* d : {&h.getDspMixer().dsp(), &h.getDspProducer().dsp()}) {
            const auto& r=d->regs();
            std::cout << "DETAIL " << hostFrames << " " << (d==&h.getDspMixer().dsp()?"mixer":"producer") << std::hex;
            auto print=[&](const std::string& n,uint64_t v){std::cout << " " << n << "=" << v;};
            print("x",r.x.var);print("y",r.y.var);print("a",r.a.var);print("b",r.b.var);
            for(unsigned i=0;i<8;++i) {print("r"+std::to_string(i),r.r[i].var);print("n"+std::to_string(i),r.n[i].var);print("m"+std::to_string(i),r.m[i].var);print("mask"+std::to_string(i),r.mMask[i]);print("modulo"+std::to_string(i),r.mModulo[i]);}
            print("sr",r.sr.var);print("omr",r.omr.var);print("pc",r.pc.var);print("la",r.la.var);print("lc",r.lc.var);print("sp",r.sp.var);print("sc",r.sc.var);
            for(unsigned i=0;i<16;++i) print("ss"+std::to_string(i),r.ss[i].var);
            print("sz",r.sz.var);print("vba",r.vba.var);print("ep",r.ep.var);std::cout << std::dec << std::endl;
        }
    }
    uint64_t hash=1469598103934665603ull;
    for(unsigned i=0;i<8;++i) {hash^=h.getUC().getAReg(i);hash*=1099511628211ull;hash^=h.getUC().getDReg(i);hash*=1099511628211ull;}
    auto& a=h.getDspMixer().dsp();auto& b=h.getDspProducer().dsp();
    uint64_t v[]={hostFrames,h.getUC().getCycles(),h.getUC().getPC(),hash,
       a.getCycles(),a.getInstructionCounter(),uint64_t(a.getPC().var),stateHash(a),
       b.getCycles(),b.getInstructionCounter(),uint64_t(b.getPC().var),stateHash(b)};
    checkpoints.write(reinterpret_cast<const char*>(v),sizeof(v));
    if(!checkpoints) throw std::runtime_error("Cannot write checkpoints");
}
void advance(md::Hardware& h, unsigned frames) {
    while(frames) { auto n=std::min(frames,128u); h.processAudio(n,0); hostFrames+=n; checkpoint(h); frames-=n; }
}
void sysex(md::Hardware& h, std::vector<uint8_t> bytes) {
    synthLib::SMidiEvent e(synthLib::MidiEventSource::Host); e.sysex.assign(bytes.begin(), bytes.end());
    if(!h.sendMidi(e)) throw std::runtime_error("SysEx rejected");
    advance(h,8192);
}
void cc(md::Hardware& h, uint8_t number, uint8_t value) {
    if(!h.sendMidi({synthLib::MidiEventSource::Host,0xb0,number,value})) throw std::runtime_error("CC rejected");
    advance(h,4096);
}
void capture(md::Hardware& h, md::MachineModel model, const std::string& name,
             uint8_t note, uint8_t velocity, const std::map<unsigned,unsigned>& expected={}) {
    namespace sx=md::automation::sysex;
    std::vector<synthLib::SMidiEvent> messages;
    h.readMidiOut(messages);
    sysex(h,sx::kitSave(model,0));
    messages.clear(); h.readMidiOut(messages);
    sysex(h,sx::kitRequest(model,0));
    messages.clear(); h.readMidiOut(messages);
    bool verified=false;
    for(const auto& message:messages) if(auto kit=sx::parseKitDump(model,message.sysex)) {
        unsigned found=0;
        std::cout << "PARAMETERS " << name;
        for(const auto& p:kit->parameters) if(p.track==0) {
            std::cout << " p" << unsigned(p.page) << '.' << unsigned(p.index) << '=' << unsigned(p.value);
            if(p.page==0 && expected.count(p.index)) {
                if(p.value!=expected.at(p.index)) throw std::runtime_error("Parameter readback mismatch: "+name);
                ++found;
            }
        }
        if(found!=expected.size()) throw std::runtime_error("Incomplete parameter readback");
        std::cout << '\n';
        std::ofstream dump(name+".syx",std::ios::binary);
        dump.write(reinterpret_cast<const char*>(message.sysex.data()),message.sysex.size());
        verified=true;
    }
    if(!verified) throw std::runtime_error("No kit readback: "+name);
    advance(h,md::g_samplerate*2);
    constexpr unsigned frames=32768;
    std::array<std::vector<float>,6> audio;
    for(auto& c:audio) c.resize(frames);
    std::cout << "TRIGGER " << name << " frames=" << hostFrames << std::endl;
    const auto start=std::clock();
    h.sendMidi({synthLib::MidiEventSource::Host,0x90,note,velocity});
    for(unsigned i=0;i<frames;i+=128) {
        if(i==8192) h.sendMidi({synthLib::MidiEventSource::Host,0x80,note,0});
        synthLib::TAudioOutputs out{};
        for(unsigned c=0;c<6;++c) out[c]=audio[c].data()+i;
        h.processAudio(out,128,0); hostFrames+=128; checkpoint(h);
    }
    const double cpu=double(std::clock()-start)/CLOCKS_PER_SEC;
    std::cout << "CASE " << name << " cpu=" << cpu;
    for(unsigned c=0;c<6;++c) {
        double peak=0,energy=0;
        for(float v:audio[c]) {
            if(!std::isfinite(v)) throw std::runtime_error("Nonfinite audio: "+name);
            peak=std::max(peak,std::abs(double(v))); energy+=double(v)*v;
        }
        std::cout << " ch" << c << " peak=" << peak << " rms=" << std::sqrt(energy/frames);
    }
    std::cout << std::endl;
    std::ofstream raw(name+".f32",std::ios::binary);
    for(unsigned i=0;i<frames;++i) for(unsigned c=0;c<2;++c)
        raw.write(reinterpret_cast<const char*>(&audio[c][i]),sizeof(float));
    raw.close(); if(!raw) throw std::runtime_error("Cannot write audio");
}
int main(int argc,char** argv) try {
    Logging::setLogFunc([](const std::string&){});
    if(argc!=4) throw std::runtime_error("Usage: confidence-audio md|mm firmware cache-or-dash");
    const bool mm=std::string(argv[1])=="mm";
    const auto model=mm?md::MachineModel::Monomachine:md::MachineModel::Machinedrum;
    auto rom=read(argv[2]); std::vector<uint8_t> cache,flash,patch;
    if(std::string(argv[3])!="-") {
        if(mm) patch=read(argv[3]);
        else {
            cache=read(argv[3]);
            if(!md::decodeFactoryFlashCache(flash,cache,rom)) throw std::runtime_error("Invalid cache");
        }
    }
    if(!md::RomLoader::isRomForModel(rom,model)) throw std::runtime_error("Invalid firmware");
    auto h=std::make_unique<md::Hardware>(rom,argv[2],model,patch,std::shared_ptr<md::FrontPanelPublisher>{},flash,cache);
    checkpoint(*h);
    if(mm) for(unsigned i=0;i<md::g_samplerate*20/128;++i) h->advance(128);
    else advance(*h,md::g_samplerate*20);
    if(mm ? !(h->isAudioReady() && h->getUC().isPanelHandshakeComplete()) : !h->isFirmwareMidiReady()) {
        std::cerr << "Boot status audio=" << h->isAudioReady() << " panel=" << h->getUC().isPanelHandshakeComplete()
                  << " midi=" << h->getUC().isMidiReceiveReady() << '\n';
        throw std::runtime_error("Firmware not ready");
    }
    if(mm) {
        cc(*h,7,100);
        // Manual Appendix C: GND-SIN, SID-6581, SWAVE-SAW, FM+STAT, DPRO-DDRW.
        for(uint8_t machine:{1,3,4,8,32}) {
            sysex(*h,{0xf0,0,0x20,0x3c,3,0,0x5b,0,machine,1,0xf7});
            for(uint8_t note:{36,60,96}) {
                capture(*h,model,"mm-"+std::to_string(machine)+"-note-"+std::to_string(note),note,110);
            }
        }
    } else {
        cc(*h,8,100); cc(*h,12,0);
        const char* names[]={"bd","sd","xt","cp","rs","cb","hh","cy"};
        const unsigned settings[][3]={{0,0,1},{0,127,127},{64,0,110},{64,32,110},{64,127,110},{127,0,127},{127,127,1}};
        unsigned caseCount=0;
        for(uint8_t machine=32;machine<40;++machine) {
            sysex(*h,{0xf0,0,0x20,0x3c,2,0,0x5b,0,machine,0,2,0xf7});
            if(machine==33) {
                const uint8_t values[]={64,96,0,64,0,64,127,0};
                for(unsigned i=0;i<8;++i) cc(*h,16+i,values[i]);
            }
            cc(*h,17,96);
            const unsigned modIndex=machine==36?2:4;
            for(const auto& v:settings) {
                cc(*h,16,v[0]); cc(*h,16+modIndex,v[1]);
                const auto name=std::string("efm-")+names[machine-32]+"-p"+std::to_string(v[0])+"-m"+std::to_string(v[1])+"-v"+std::to_string(v[2]);
                capture(*h,model,name,36,v[2],{{0,v[0]},{1,96},{modIndex,v[1]}});
                ++caseCount;
            }
        }
        for(uint8_t machine:{1,16,17,48,64}) {
            sysex(*h,{0xf0,0,0x20,0x3c,2,0,0x5b,0,machine,0,2,0xf7});
            capture(*h,model,"md-control-"+std::to_string(machine),36,110);
        }
    }
    return 0;
} catch(const std::exception& e) {std::cerr << e.what() << std::endl; return 1;}
