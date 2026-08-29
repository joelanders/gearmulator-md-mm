#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdromdata.h"
#include "mdLib/mdtypes.h"

#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/opcodetypes.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace
{
	constexpr uint32_t g_syntheticProgramCounter = 0x00000100;
	constexpr uint32_t g_syntheticScratch = md::memorymap::g_mainRam.begin + 0x100;
	constexpr dsp56k::TWord g_syntheticDspWords = 32;

	void write32(std::vector<uint8_t>& _data, const size_t _offset, const uint32_t _value)
	{
		_data[_offset + 0] = static_cast<uint8_t>(_value >> 24);
		_data[_offset + 1] = static_cast<uint8_t>(_value >> 16);
		_data[_offset + 2] = static_cast<uint8_t>(_value >> 8);
		_data[_offset + 3] = static_cast<uint8_t>(_value);
	}

	void write16(std::vector<uint8_t>& _data, const size_t _offset, const uint16_t _value)
	{
		_data[_offset + 0] = static_cast<uint8_t>(_value >> 8);
		_data[_offset + 1] = static_cast<uint8_t>(_value);
	}

	std::vector<uint8_t> makeSyntheticImage()
	{
		constexpr uint32_t stackPointer = 0x002ff000;
		std::vector<uint8_t> image(md::g_romSize, 0);
		write32(image, 0, stackPointer);
		write32(image, 4, g_syntheticProgramCounter);

		// Independently authored ColdFire exercise over generated memory.
		size_t pc = g_syntheticProgramCounter;
		write16(image, pc, 0x203c); pc += 2; // MOVE.L #0,D0
		write32(image, pc, 0); pc += 4;
		constexpr uint32_t loop = g_syntheticProgramCounter + 6;
		write16(image, pc, 0x5280); pc += 2; // ADDQ.L #1,D0
		write16(image, pc, 0x23c0); pc += 2; // MOVE.L D0,(abs).L
		write32(image, pc, g_syntheticScratch); pc += 4;
		write16(image, pc, 0x2239); pc += 2; // MOVE.L (abs).L,D1
		write32(image, pc, g_syntheticScratch); pc += 4;
		write16(image, pc, 0x6000); pc += 2; // BRA.W loop
		write16(image, pc, static_cast<uint16_t>(loop - (pc + 2)));
		return image;
	}

	void installSyntheticDspProgram(md::Dsp& _dsp)
	{
		// Independently authored cached-execution exercise.
		constexpr dsp56k::TWord nop = 0x000000;
		constexpr dsp56k::TWord jumpToZero = 0x0c0000;
		auto& core = _dsp.dsp();
		for(dsp56k::TWord pc = 0; pc < g_syntheticDspWords - 1; ++pc)
		{
			core.memory().set(dsp56k::MemArea_P, pc, nop);
			core.getJit().notifyProgramMemWrite(pc);
		}
		core.memory().set(dsp56k::MemArea_P, g_syntheticDspWords - 1, jumpToZero);
		core.getJit().notifyProgramMemWrite(g_syntheticDspWords - 1);
		core.setPC(0);
		_dsp.onDspBootFinished();
	}

	void exerciseGenericTraffic(md::Hardware& _hardware, const uint32_t _iteration)
	{
		const std::array<uint8_t, 3> midi = {
			static_cast<uint8_t>((_iteration & 1) ? 0x80 : 0x90),
			static_cast<uint8_t>(36 + (_iteration % 24)),
			static_cast<uint8_t>(32 + (_iteration % 64))
		};
		(void)_hardware.trySendRealtimeMidi(midi);

		for(auto* dsp : {&_hardware.getDspMixer(), &_hardware.getDspProducer()})
		{
			const dsp56k::TWord word = (_iteration * 0x010101u) & 0x00ffffffu;
			dsp->hdi08().writeRX(&word, 1);
			(void)dsp->hdi08().readRX(dsp56k::Nop);

			if(_iteration == 0)
			{
				auto& essi = dsp->getPeriph().getEssi1();
				essi.writeRX(word);
				(void)essi.readRX();
				essi.writeTX(0, word);
				(void)essi.readTX(0);
			}
		}
	}

	bool run(const md::MachineModel _model, const uint32_t _frames)
	{
		const auto image = makeSyntheticImage();
		auto hardware = std::make_unique<md::Hardware>(
			md::SyntheticProfileHardwareTag{}, image, _model);
		if(!hardware->isValid())
			return false;

		installSyntheticDspProgram(hardware->getDspMixer());
		installSyntheticDspProgram(hardware->getDspProducer());

		constexpr uint32_t chunkFrames = 256;
		const auto start = std::chrono::steady_clock::now();
		uint32_t remaining = _frames;
		uint32_t iteration = 0;
		while(remaining)
		{
			exerciseGenericTraffic(*hardware, iteration++);
			const auto chunk = remaining < chunkFrames ? remaining : chunkFrames;
			hardware->advance(chunk);
			remaining -= chunk;
		}
		const auto elapsed = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - start).count();

		std::printf("model=%s frames=%u seconds=%.6f dsp1_cycles=%llu dsp2_cycles=%llu\n",
			_model == md::MachineModel::Monomachine ? "MM" : "MD", _frames, elapsed,
			static_cast<unsigned long long>(hardware->getDspMixer().dsp().getCycles()),
			static_cast<unsigned long long>(hardware->getDspProducer().dsp().getCycles()));
		const uint32_t scratchValue =
			(static_cast<uint32_t>(hardware->getUC().read16(g_syntheticScratch)) << 16)
			| hardware->getUC().read16(g_syntheticScratch + 2);
		return hardware->getUC().getPC() >= g_syntheticProgramCounter + 6
			&& hardware->getUC().getPC() <= g_syntheticProgramCounter + 24
			&& scratchValue != 0
			&& hardware->getDspMixer().dsp().getPC().toWord() < g_syntheticDspWords
			&& hardware->getDspProducer().dsp().getPC().toWord() < g_syntheticDspWords;
	}
}

int main(int argc, char** argv)
{
	const uint32_t frames = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10))
		: 44100u;
	if(!frames)
		return 2;
	const char* model = argc > 2 ? argv[2] : "both";
	if(std::strcmp(model, "md") == 0)
		return run(md::MachineModel::Machinedrum, frames) ? 0 : 1;
	if(std::strcmp(model, "mm") == 0)
		return run(md::MachineModel::Monomachine, frames) ? 0 : 1;
	if(std::strcmp(model, "both") != 0)
		return 2;
	return run(md::MachineModel::Machinedrum, frames)
		&& run(md::MachineModel::Monomachine, frames) ? 0 : 1;
}
