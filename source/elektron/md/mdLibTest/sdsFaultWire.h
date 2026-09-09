#pragma once

#include "mdLib/mdhardware.h"

#include <cstdio>
#include <string>
#include <vector>

namespace md::test
{
	// Test-only proxy. The production state machine talks to the real emulated
	// UART; firmware generates the replies. No fake ACK receiver or ROM patches.
	class SdsFaultWire final : public MidiByteSink
	{
	public:
		SdsFaultWire(Hardware& hardware, TurboMidiTransfer& transfer, std::string mode)
			: m_hardware(hardware), m_transfer(transfer), m_mode(std::move(mode))
		{
			m_hardware.getUC().setMidiTransmitTap([this](uint8_t b) { receive(b); });
		}
		~SdsFaultWire() override { m_hardware.getUC().setMidiTransmitTap({}); }
		bool tryWriteMidiByte(uint8_t b) override
		{
			const auto index = b == 0xf0 ? 0 : m_out.size();
			const bool target = m_out.size() >= 5 && m_out[1] == 0x7e
				&& m_out[3] == 2 && m_out[4] == 5;
			const bool corrupt = target && index == 32 && !m_injected && m_mode == "corrupt-packet";
			if(!m_hardware.getUC().tryWriteMidiByte(corrupt ? b ^ 1 : b)) return false;
			if(corrupt) { ++m_injected; std::puts("Injected packet corruption at packet 5 byte 32"); }
			if(b < 0xf8)
			{
				if(b == 0xf0) m_out.clear();
				m_out.push_back(b);
				if(b == 0xf7)
				{
					if(m_out.size() == 21 && m_out[1] == 0x7e && m_out[3] == 1)
					{
						const size_t words = m_out[10] | (size_t(m_out[11]) << 7) | (size_t(m_out[12]) << 14);
						const size_t perPacket = 120 / ((m_out[6] + 6) / 7);
						m_packetCount = (words + perPacket - 1) / perPacket;
						m_packetsDelivered = 0;
						m_header = true;
					}
					if(m_out.size() == 127 && m_out[1] == 0x7e && m_out[3] == 2)
					{
						if(m_out[4] == (m_packetsDelivered & 0x7f)) ++m_packetsDelivered;
						m_header = false;
					}
					m_out.clear();
				}
			}
			return true;
		}
		size_t queuedMidiByteCount() const override { return m_hardware.queuedMidiRxBytes(); }
		void tick(uint64_t cycles)
		{
			m_cycles += cycles;
			if(!m_delayed.empty() && m_cycles >= m_release)
			{
				forward(m_delayed);
				m_delayed.clear();
				++m_released;
			}
		}
		size_t injected() const { return m_injected; }
		size_t naks() const { return m_naks; }
		size_t released() const { return m_released; }
		void disableFault()
		{
			m_mode = "none";
			m_out.clear(); m_in.clear(); m_delayed.clear();
		}
	private:
		void forward(const std::vector<uint8_t>& bytes)
		{
			for(auto b : bytes) m_transfer.observeTransmitByte(b);
		}
		void receive(uint8_t b)
		{
			if(b >= 0xf8) { m_transfer.observeTransmitByte(b); return; }
			if(b == 0xf0) m_in.clear();
			if(m_in.empty() && b != 0xf0) return;
			m_in.push_back(b);
			if(b != 0xf7) return;
			auto message = std::move(m_in);
			m_in.clear();
			const bool sds = message.size() == 6 && message[1] == 0x7e;
			if(sds && message[3] != 0x7f)
			{
				std::printf("Firmware SDS reply: command=%02x packet=%u\n", message[3], message[4]);
				if(message[3] == 0x7e) ++m_naks;
			}
			if(sds && (m_mode == "silence" || m_mode == "drop-final-ack") && m_injected) return;
			const bool target = m_mode == "drop-header-ack" ? m_header
				: m_mode == "drop-final-ack" ? (!m_header && m_packetCount && m_packetsDelivered == m_packetCount)
				: message.size() > 4 && message[4] == 5;
			const bool ack = sds && message[3] == 0x7f && target;
			if(ack && ((!m_injected && m_mode != "none" && m_mode != "corrupt-packet") || m_mode == "silence"))
			{
				++m_injected;
				std::printf("Injected %s at ACK packet %u\n", m_mode.c_str(), message[4]);
				if(m_mode == "drop-ack" || m_mode == "silence" || m_mode == "drop-header-ack" || m_mode == "drop-final-ack") return;
				if(m_mode == "delay-ack" || m_mode == "wait")
				{
					m_delayed = message;
					m_release = m_cycles + 40'000'000ull * 3;
					if(m_mode == "wait") { message[3] = 0x7c; forward(message); }
					return;
				}
				if(m_mode == "duplicate-ack") forward(message);
			}
			forward(message);
		}
		Hardware& m_hardware;
		TurboMidiTransfer& m_transfer;
		std::string m_mode;
		std::vector<uint8_t> m_out, m_in, m_delayed;
		uint64_t m_cycles = 0, m_release = 0;
		size_t m_injected = 0, m_naks = 0, m_released = 0;
		size_t m_packetCount = 0, m_packetsDelivered = 0;
		bool m_header = false;
	};
}
