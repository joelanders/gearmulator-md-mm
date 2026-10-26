#include "mdLib/mdsim.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
	void sendCommand(md::Sim& _sim, const uint8_t _command, const uint32_t _argument,
		const uint8_t _crc)
	{
		const std::array<uint8_t, 6> bytes{{
			static_cast<uint8_t>(0x40 | _command),
			static_cast<uint8_t>(_argument >> 24),
			static_cast<uint8_t>(_argument >> 16),
			static_cast<uint8_t>(_argument >> 8),
			static_cast<uint8_t>(_argument), _crc}};
		_sim.write8(md::Sim::g_ppddr, 0x0c);
		bool clock = (_sim.getParallelData() & 0x04) != 0;
		for(const auto byte : bytes)
			for(int bit = 7; bit >= 0; --bit)
			{
				clock = !clock;
				const uint8_t data = (byte & (1u << bit)) ? 0x08 : 0;
				_sim.write8(md::Sim::g_ppdat,
					static_cast<uint8_t>(data | (clock ? 0x04 : 0)));
			}
	}

	std::vector<uint8_t> readResponse(md::Sim& _sim, const size_t _size)
	{
		_sim.write8(md::Sim::g_ppddr, 0x04);
		std::vector<uint8_t> response(_size);
		bool clock = (_sim.getParallelData() & 0x04) != 0;
		for(size_t i = 0; i < 2; ++i)
		{
			clock = !clock;
			_sim.write8(md::Sim::g_ppdat, clock ? 0x04 : 0x00);
		}
		for(auto& byte : response)
			for(int bit = 7; bit >= 0; --bit)
			{
				clock = !clock;
				_sim.write8(md::Sim::g_ppdat, clock ? 0x04 : 0x00);
				if(_sim.read8(md::Sim::g_ppdat) & 0x08)
					byte |= static_cast<uint8_t>(1u << bit);
			}
		return response;
	}

	bool shortResponse(md::Sim& _sim, const uint8_t _command,
		const uint32_t _argument, const uint32_t _expected)
	{
		sendCommand(_sim, _command, _argument, 0xff);
		const auto response = readResponse(_sim, 5);
		return response.size() == 5 && response[0] == 0x3f
			&& (static_cast<uint32_t>(response[1]) << 24
				| static_cast<uint32_t>(response[2]) << 16
				| static_cast<uint32_t>(response[3]) << 8
				| response[4]) == _expected;
	}
}

int main()
{
	md::Sim sim;
	sim.setPlusDriveEnabled(true);
	if((sim.read8(md::Sim::g_ppdat) & 0xf8) != 0xf8)
	{
		std::fputs("mdplusdrive_test: idle bus is not pulled high\n", stderr);
		return 1;
	}

	sendCommand(sim, 0, 0, 0x95);
	if(sim.getPlusDrive().commandCount() != 1
		|| sim.getPlusDrive().lastCommand() != 0)
	{
		std::fputs("mdplusdrive_test: GO_IDLE_STATE was not decoded\n", stderr);
		return 1;
	}
	if(!shortResponse(sim, 1, 0x40ff8000u, 0xc0ff8080u))
	{
		std::fputs("mdplusdrive_test: SEND_OP_COND response is invalid\n", stderr);
		return 1;
	}

	sendCommand(sim, 2, 0, 0xff);
	const auto cid = readResponse(sim, 17);
	if(cid[0] != 0x3f || cid[4] != 'G' || cid[5] != 'M' || cid[6] != 'D')
	{
		std::fputs("mdplusdrive_test: CID response is invalid\n", stderr);
		return 1;
	}
	if(!shortResponse(sim, 3, 0x00020000u, 0x00000700u)
		|| !shortResponse(sim, 7, 0x00020000u, 0x00000900u))
	{
		std::fputs("mdplusdrive_test: card selection failed\n", stderr);
		return 1;
	}

	sendCommand(sim, 9, 0x00020000u, 0xff);
	const auto csd = readResponse(sim, 17);
	if(csd[0] != 0x3f || (csd[1] & 0xc0) != 0xc0)
	{
		std::fputs("mdplusdrive_test: CSD response is invalid\n", stderr);
		return 1;
	}
	if(!shortResponse(sim, 6, 0x03b70100u, 0x00000900u)
		|| !shortResponse(sim, 16, 512, 0x00000900u)
		|| !sim.getPlusDrive().initialized())
	{
		std::fputs("mdplusdrive_test: initialization sequence did not complete\n", stderr);
		return 1;
	}
	if(!shortResponse(sim, 17, 4u * 1024u * 1024u, 0x80000900u)
		|| !shortResponse(sim, 24, 4u * 1024u * 1024u, 0x80000900u))
	{
		std::fputs("mdplusdrive_test: out-of-range block was accepted\n", stderr);
		return 1;
	}

	sendCommand(sim, 17, 0, 0xff);
	if(readResponse(sim, 5)[0] != 0x3f)
		return 1;
	// Consume the command routine's trailing clocks, then wait for the four-bit
	// data start token and verify one erased 512-byte block.
	bool clock = false;
	for(size_t i = 0; i < 64; ++i)
	{
		clock = !clock;
		sim.write8(md::Sim::g_ppdat, clock ? 0x04 : 0x00);
		if((sim.read8(md::Sim::g_ppdat) & 0xf0) == 0)
			break;
		if(i == 63)
		{
			std::fputs("mdplusdrive_test: data token did not arrive\n", stderr);
			return 1;
		}
	}
	for(size_t nibble = 0; nibble < 1024; ++nibble)
	{
		clock = !clock;
		sim.write8(md::Sim::g_ppdat, clock ? 0x04 : 0x00);
		if((sim.read8(md::Sim::g_ppdat) & 0xf0) != 0xf0)
		{
			std::fputs("mdplusdrive_test: erased block data is invalid\n", stderr);
			return 1;
		}
	}

	sendCommand(sim, 24, 7, 0xff);
	if(readResponse(sim, 5)[0] != 0x3f)
		return 1;
	clock = (sim.getParallelData() & 0x04) != 0;
	for(size_t i = 0; i < 8; ++i)
	{
		clock = !clock;
		sim.write8(md::Sim::g_ppdat, clock ? 0x04 : 0x00);
	}
	sim.write8(md::Sim::g_ppddr, 0xf4);
	sim.write8(md::Sim::g_ppdat, 0xf0);
	sim.write8(md::Sim::g_ppdat, 0xf4);
	sim.write8(md::Sim::g_ppdat, 0x00);
	clock = (sim.getParallelData() & 0x04) != 0;
	for(size_t nibble = 0; nibble < 1024; ++nibble)
	{
		clock = !clock;
		const uint8_t data = static_cast<uint8_t>((nibble & 1 ? 0x0a : 0x05) << 4);
		sim.write8(md::Sim::g_ppdat,
			static_cast<uint8_t>(data | (clock ? 0x04 : 0)));
	}
	for(size_t nibble = 0; nibble < 16; ++nibble)
	{
		clock = !clock;
		sim.write8(md::Sim::g_ppdat, clock ? 0x04 : 0x00);
	}
	if(sim.getPlusDrive().storedSectorCount() != 1)
	{
		std::fputs("mdplusdrive_test: written block was not stored\n", stderr);
		return 1;
	}
	const auto image = sim.getPlusDrive().copyStorage();
	if(image.size() < 21 || image[19] != 7 || image[20] != 0x5a)
	{
		std::fprintf(stderr, "mdplusdrive_test: stored record is %02x/%02x\n",
			image.size() > 19 ? image[19] : 0xff,
			image.size() > 20 ? image[20] : 0xff);
		return 1;
	}
	md::Sim restored;
	restored.setPlusDriveEnabled(true);
	if(!restored.getPlusDrive().replaceStorage(image))
		return 1;
	auto malformed = image;
	malformed[16] = 0xff;
	if(restored.getPlusDrive().replaceStorage(malformed))
	{
		std::fputs("mdplusdrive_test: malformed storage was accepted\n", stderr);
		return 1;
	}

	md::PlusDrive generations;
	if(!generations.replaceStorage(image)
		|| generations.storageDirty())
	{
		std::fputs("mdplusdrive_test: clean image started dirty\n", stderr);
		return 1;
	}
	auto secondImage = image;
	secondImage[20] ^= 0x01;
	if(!generations.replaceStorage(secondImage, true)
		|| !generations.storageDirty())
	{
		std::fputs("mdplusdrive_test: changed image was not dirty\n", stderr);
		return 1;
	}
	const auto firstGeneration = generations.storageGeneration();
	auto thirdImage = secondImage;
	thirdImage[20] ^= 0x02;
	if(!generations.replaceStorage(thirdImage, true)
		|| generations.storageGeneration() <= firstGeneration)
	{
		std::fputs("mdplusdrive_test: generation did not advance\n", stderr);
		return 1;
	}
	const auto secondGeneration = generations.storageGeneration();
	generations.markStoragePersisted(firstGeneration);
	if(!generations.storageDirty())
	{
		std::fputs("mdplusdrive_test: stale flush cleared a newer write\n", stderr);
		return 1;
	}
	generations.markStoragePersisted(secondGeneration);
	if(generations.storageDirty())
	{
		std::fputs("mdplusdrive_test: current flush remained dirty\n", stderr);
		return 1;
	}
	sendCommand(restored, 17, 7, 0xff);
	if(readResponse(restored, 5)[0] != 0x3f)
		return 1;
	clock = (restored.getParallelData() & 0x04) != 0;
	for(size_t i = 0; i < 64; ++i)
	{
		clock = !clock;
		restored.write8(md::Sim::g_ppdat, clock ? 0x04 : 0x00);
		if((restored.read8(md::Sim::g_ppdat) & 0xf0) == 0)
			break;
		if(i == 63)
			return 1;
	}
	for(size_t nibble = 0; nibble < 1024; ++nibble)
	{
		clock = !clock;
		restored.write8(md::Sim::g_ppdat, clock ? 0x04 : 0x00);
		const uint8_t expected = (nibble & 1) ? 0xa0 : 0x50;
		if((restored.read8(md::Sim::g_ppdat) & 0xf0) != expected)
		{
			std::fprintf(stderr,
				"mdplusdrive_test: persisted block mismatch at nibble %zu: %02x != %02x\n",
				nibble, restored.read8(md::Sim::g_ppdat) & 0xf0, expected);
			return 1;
		}
	}

	std::puts("mdplusdrive_test: PASS");
	return 0;
}
