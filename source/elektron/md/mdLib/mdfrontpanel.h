#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace md
{
	// FrontPanel decodes the Elektron Machinedrum/Monomachine host->panel UART
	// byte stream and reconstructs the two things the panel controller drives:
	//
	//   (a) a 128x64 graphic LCD (Winstar WG12864A, a KS0107/KS0108-compatible
	//       controller pair). The host sends compact "tile" writes: a byte in the
	//       range 0x10..0x1f (bit3 = controller half, bits0-2 = KS0108 page 0-7),
	//       a column base 0x00..0x38 in steps of 8, then 8 payload bytes, one per
	//       LCD column. Each payload byte is 8 vertical pixels, LSB = top.
	//
	//   (b) LED bank command bytes, each sent as [cmd][arg]. All are active-low
	//       (a 0 bit lights the LED). MD defines the first six:
	//         0x20 = step/trig LEDs 1-8       0x21 = step/trig LEDs 9-16
	//         0x22 = status LEDs              0x23 = mode LEDs
	//         0x24 = sound/DRUM LEDs 1-8      0x25 = sound/DRUM LEDs 9-16
	//       MM extends the protocol through 0x2d; raw accessors expose those banks.
	//
	// The stream interleaves both. Decoder behavior follows the public MAME
	// Elektron driver and the documented LCD controller protocol. It carries no
	// emulator, MCU/DSP state, or I/O.
	// Feed it host->panel bytes with processByte()/processBytes() and read the
	// reconstructed framebuffer and LED banks back through the accessors.
	class FrontPanel
	{
	public:
		static constexpr uint32_t g_lcdWidth  = 128;
		static constexpr uint32_t g_lcdHeight = 64;
		static constexpr uint8_t g_firstLedBank = 0x20;
		static constexpr uint8_t g_lastLedBank = 0x2d;
		static constexpr uint32_t g_ledBankCount =
			g_lastLedBank - g_firstLedBank + 1;

		// LED bank command bytes (host->panel direction). Active-low.
		enum class LedBank : uint8_t
		{
			Step0  = 0x20, // step/trig LEDs 1-8
			Step1  = 0x21, // step/trig LEDs 9-16
			Status = 0x22, // status LEDs (see StatusLed)
			Mode   = 0x23, // mode LEDs (see ModeLed)
			Drum0  = 0x24, // sound-selection / DRUM LEDs, tracks 1-8
			Drum1  = 0x25, // sound-selection / DRUM LEDs, tracks 9-16
		};

		// Bit positions inside the 0x22 status bank (active-low, 0 = lit).
		enum class StatusLed : uint8_t
		{
			Clear     = 0, // lit by CLEAR
			Tempo     = 1, // flashes with the clock
			// bit 2 is unused
			Pattern   = 3,
			Song      = 4,
			Routing   = 5,
			Effects   = 6,
			Synthesis = 7,
		};

		// Bit positions inside the 0x23 mode bank (active-low, 0 = lit).
		enum class ModeLed : uint8_t
		{
			Extended    = 0,
			Classic     = 1,
			BankGroupAD = 2,
			BankGroupEH = 3,
			Record      = 4, // grid-edit
			GridBlink   = 5, // grid-edit blinker
			// bits 6/7 are unused
		};

		FrontPanel();

		// Clear the framebuffer, LED banks and parser back to power-on state.
		void reset();

		// Consume the host->panel byte stream.
		void processByte(uint8_t _byte);
		void processBytes(const uint8_t* _data, size_t _size);
		void processBytes(const std::vector<uint8_t>& _data) { processBytes(_data.data(), _data.size()); }

		// LCD framebuffer, 128x64. _x in [0,127], _y in [0,63]. true = pixel lit.
		bool getLcdPixel(uint32_t _x, uint32_t _y) const;

		// Number of lit LCD pixels (0 until the boot logo/UI has been decoded).
		uint32_t countLitPixels() const;

		// Raw 8-vertical-pixel VRAM byte for a controller half (0/1), page (0-7),
		// column (0-63). LSB = topmost pixel of the page.
		uint8_t getLcdVram(uint32_t _half, uint32_t _page, uint32_t _col) const;

		// Raw active-low bank byte as last written by the stream (0xff at reset).
		uint8_t getLedBankRaw(LedBank _bank) const;
		uint8_t getLedBankRaw(uint8_t _command) const;

		// Did the stream ever issue a command for this bank?
		bool wasLedBankWritten(LedBank _bank) const;
		bool wasLedBankWritten(uint8_t _command) const;

		// Decoded LED state. All return true when the LED is lit.
		bool getStepLed(uint32_t _index) const; // Machinedrum steps 1..16
		bool getMonomachineStepLed(uint32_t _index) const; // Monomachine steps 1..16
		bool getDrumLed(uint32_t _index) const;  // _index 0..15 -> tracks 1..16
		bool getStatusLed(StatusLed _led) const;
		bool getModeLed(ModeLed _led) const;

		// Stream diagnostics.
		uint32_t getByteCount() const { return m_byteCount; }
		uint32_t getTileWriteCount() const { return m_tileWriteCount; }
		uint32_t getLedCommandCount() const { return m_ledCommandCount; }

	private:
		void decode(uint8_t _byte);
		static uint32_t bankIndex(uint8_t _command) { return _command - g_firstLedBank; }
		static uint32_t bankIndex(LedBank _bank)
		{
			return bankIndex(static_cast<uint8_t>(_bank));
		}

		// VRAM [controller-half 0..1][KS0108 page 0..7][column 0..63].
		std::array<std::array<std::array<uint8_t, 64>, 8>, 2> m_lcdVram{};

		// MD writes 0x20..0x25; MM extends the same active-low protocol through 0x2d.
		std::array<uint8_t, g_ledBankCount> m_ledBank{};
		std::array<bool, g_ledBankCount> m_ledBankWritten{};

		// Streaming parser (mirrors the MAME state machine):
		//   0 idle, 1 tile column-base, 2 tile payload, 3 command argument.
		uint8_t m_parseState = 0;
		uint8_t m_cmd = 0;
		uint8_t m_lcdXaddr = 0;
		uint8_t m_lcdYaddr = 0;
		uint8_t m_lcdCol = 0;
		std::array<uint8_t, 8> m_tilePayload{};

		uint32_t m_byteCount = 0;
		uint32_t m_tileWriteCount = 0;
		uint32_t m_ledCommandCount = 0;
	};

	// Cross-thread publication boundary for the reconstructed display. The
	// emulation thread owns and mutates its live FrontPanel, then offers a complete
	// value copy without waiting. Readers may briefly wait while copying the last
	// published value, but can never observe a torn copy. Display mutations become
	// visible only after a complete LED command or LCD tile has been decoded.
	class FrontPanelPublisher
	{
	public:
		bool tryPublish(const FrontPanel& _panel);
		bool tryRead(FrontPanel& _panel) const;
		FrontPanel read() const;
		void reset();

	private:
		mutable std::mutex m_mutex;
		FrontPanel m_snapshot;
	};
}
