// Independently authored DSP programs. No firmware or product-derived fixtures.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"

static uint64_t threadNanoseconds()
{
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if(!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetThreadTimes failed");
    const auto ticks = [](FILETIME time) {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user))*100;
#else
    timespec time{};
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time))
        throw std::runtime_error("clock_gettime failed");
    return uint64_t(time.tv_sec)*1000000000ull + time.tv_nsec;
#endif
}

int main(int argc, char** argv)
{
    try
    {
        if(argc != 4) throw std::runtime_error("usage: dspCorePerf alu|memory jit|bounded|interpreter iterations");
        const std::string program = argv[1], mode = argv[2];
        const uint64_t iterations = std::stoull(argv[3]);
        if(iterations == 0 || iterations > 1000000000ull)
            throw std::runtime_error("iterations out of range");
        if(mode != "jit" && mode != "bounded" && mode != "interpreter")
            throw std::runtime_error("invalid execution mode");
        dsp56k::DefaultMemoryValidator validator;
        dsp56k::Memory memory(validator, 0x10000);
        dsp56k::PeripheralsNop x, y;
        auto core = std::make_unique<dsp56k::DSP>(memory, &x, &y);
        dsp56k::Assembler assembler;
        std::vector<std::string> body;
        if(program == "alu")
            body = {"add x0,a", "sub y0,b", "mac x0,y0,a", "asr b"};
        else if(program == "memory")
            body = {"move x:>$400,x0", "move y:>$401,y0", "mac x0,y0,a", "move a,x:>$402"};
        else
            throw std::runtime_error("invalid program");
        dsp56k::TWord pc = 0x100;
        const auto emit = [&](const std::string& instruction) {
            const auto result = assembler.assemble(instruction.c_str());
            if(!result.success()) throw std::runtime_error("assembly failed: " + instruction);
            for(uint32_t word=0; word<result.wordCount; ++word)
                core->memWriteP(pc++, result.word[word]);
        };
        for(int repeat=0; repeat<4; ++repeat)
            for(const auto& instruction : body) emit(instruction);
        emit("jmp $100");
        core->setPC(0x100);
        core->regs().x.var = 0x123456;
        core->regs().y.var = 0x345678;
        memory.set(dsp56k::MemArea_X, 0x400, 0x123456);
        memory.set(dsp56k::MemArea_Y, 0x401, 0x345678);

        const auto run = [&](uint64_t count) {
            if(mode == "jit")
                for(uint64_t i=0; i<count; ++i) core->exec();
            else if(mode == "bounded")
                for(uint64_t i=0; i<count; ++i) core->execUntilCycles(core->getCycles()+128);
            else
                for(uint64_t i=0; i<count; ++i) core->execInterpreter();
        };
        run(10000); // Compile and warm the JIT before measuring.
        const auto cpuStart = threadNanoseconds();
        const auto wallStart = std::chrono::steady_clock::now();
        run(iterations);
        const auto wallEnd = std::chrono::steady_clock::now();
        const auto cpu = threadNanoseconds()-cpuStart;
        const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(wallEnd-wallStart).count();
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&](uint64_t value) { hash = (hash^value)*1099511628211ull; };
        const auto& regs = core->regs();
        mix(regs.x.var); mix(regs.y.var); mix(regs.a.var); mix(regs.b.var);
        mix(regs.pc.var); mix(regs.sr.var); mix(regs.omr.var);
        for(size_t i=0; i<8; ++i) { mix(regs.r[i].var); mix(regs.n[i].var); mix(regs.m[i].var); }
        for(uint32_t i=0; i<0x10000; ++i)
        {
            mix(memory.get(dsp56k::MemArea_X, i));
            mix(memory.get(dsp56k::MemArea_Y, i));
        }
        std::cout << "@RESULT {\"program\":\"" << program << "\",\"mode\":\"" << mode
            << "\",\"iterations\":" << iterations << ",\"cpu_ns\":" << cpu
            << ",\"wall_ns\":" << wall << ",\"cycles\":" << core->getCycles()
            << ",\"instructions\":" << core->getInstructionCounter()
            << ",\"checksum\":\"" << hash << "\",\"pc\":" << regs.pc.var << "}\n";
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
