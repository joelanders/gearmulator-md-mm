#include "mdfrontpanel.h"

namespace md
{
	FrontPanel::FrontPanel()
	{
		reset();
	}

	void FrontPanel::reset()
	{
		for (auto& half : m_lcdVram)
			for (auto& page : half)
				page.fill(0);

		m_ledBank.fill(0xff);      // active-low: all LEDs off at power-on
		m_ledBankWritten.fill(false);

		m_parseState = 0;
		m_cmd = 0;
		m_lcdXaddr = 0;
		m_lcdYaddr = 0;
		m_lcdCol = 0;
		m_tilePayload.fill(0);

		m_byteCount = 0;
		m_tileWriteCount = 0;
		m_ledCommandCount = 0;
	}

	void FrontPanel::processByte(uint8_t _byte)
	{
		++m_byteCount;
		decode(_byte);
	}

	void FrontPanel::processBytes(const uint8_t* _data, size_t _size)
	{
		for (size_t i = 0; i < _size; ++i)
			processByte(_data[i]);
	}

	// Streaming decode of the host->panel command stream: skip LCD tile writes
	// (0x1x + valid column base + 8 payload bytes), decode LED banks as [cmd][arg].
	void FrontPanel::decode(uint8_t _byte)
	{
		switch (m_parseState)
		{
		case 0: // idle: a tile start (0x1x), an LED command (0x20-0x25), or an ignorable byte
			if (_byte >= 0x10 && _byte <= 0x1f)
			{
				m_lcdXaddr = _byte;
				m_parseState = 1;
			}
			else if (_byte >= g_firstLedBank && _byte <= g_lastLedBank)
			{
				m_cmd = _byte;
				m_parseState = 3;
			}
			break;

		case 1: // column base after a 0x1x: valid = 0,8,..,56 -> a real tile write
			if ((_byte & 0x07) == 0 && _byte < 64)
			{
				m_lcdYaddr = _byte;
				m_lcdCol = 0;
				m_parseState = 2;
			}
			else
			{
				// not a tile after all: drop back to idle and reprocess this byte
				m_parseState = 0;
				decode(_byte);
			}
			break;

		case 2: // 8 payload bytes = one atomically committed LCD tile
		{
			m_tilePayload[m_lcdCol] = _byte;
			if (++m_lcdCol >= 8)
			{
				const uint32_t half = (m_lcdXaddr >> 3) & 1;
				const uint32_t page = m_lcdXaddr & 0x07;
				for(uint32_t i = 0; i < m_tilePayload.size(); ++i)
				{
					const auto col = static_cast<uint32_t>(m_lcdYaddr) + i;
					if(col < 64)
						m_lcdVram[half][page][col] = m_tilePayload[i];
				}
				m_parseState = 0;
				++m_tileWriteCount;
			}
			break;
		}

		default: // case 3: LED command argument
		{
			const uint32_t idx = bankIndex(m_cmd);
			m_ledBank[idx] = _byte;
			m_ledBankWritten[idx] = true;
			m_parseState = 0;
			++m_ledCommandCount;
			break;
		}
		}
	}

	bool FrontPanel::getLcdPixel(uint32_t _x, uint32_t _y) const
	{
		if (_x >= g_lcdWidth || _y >= g_lcdHeight)
			return false;

		const uint32_t half = (_x >> 6) & 1;   // 0..63 -> controller 0, 64..127 -> controller 1
		const uint32_t col = _x & 0x3f;
		const uint32_t page = (_y >> 3) & 7;
		const uint32_t bit = _y & 7;           // LSB = topmost pixel of the page
		return ((m_lcdVram[half][page][col] >> bit) & 1) != 0;
	}

	uint32_t FrontPanel::countLitPixels() const
	{
		uint32_t count = 0;
		for (const auto& half : m_lcdVram)
			for (const auto& page : half)
				for (const uint8_t byte : page)
				{
					uint8_t v = byte;
					while (v)
					{
						v &= static_cast<uint8_t>(v - 1);
						++count;
					}
				}
		return count;
	}

	uint8_t FrontPanel::getLcdVram(uint32_t _half, uint32_t _page, uint32_t _col) const
	{
		if (_half >= 2 || _page >= 8 || _col >= 64)
			return 0;
		return m_lcdVram[_half][_page][_col];
	}

	uint8_t FrontPanel::getLedBankRaw(LedBank _bank) const
	{
		return getLedBankRaw(static_cast<uint8_t>(_bank));
	}

	uint8_t FrontPanel::getLedBankRaw(uint8_t _command) const
	{
		if (_command < g_firstLedBank || _command > g_lastLedBank)
			return 0xff;
		return m_ledBank[bankIndex(_command)];
	}

	bool FrontPanel::wasLedBankWritten(LedBank _bank) const
	{
		return wasLedBankWritten(static_cast<uint8_t>(_bank));
	}

	bool FrontPanel::wasLedBankWritten(uint8_t _command) const
	{
		if (_command < g_firstLedBank || _command > g_lastLedBank)
			return false;
		return m_ledBankWritten[bankIndex(_command)];
	}

	bool FrontPanel::getStepLed(uint32_t _index) const
	{
		if (_index >= 16)
			return false;
		const uint8_t raw = m_ledBank[bankIndex(_index < 8 ? LedBank::Step0 : LedBank::Step1)];
		const uint32_t bit = _index & 7;
		return ((raw >> bit) & 1) == 0;   // active-low: 0 bit = lit
	}

	FrontPanel::LedColor FrontPanel::getMonomachineStepLedColor(uint32_t _index) const
	{
		if(_index >= 16)
			return LedColor::Off;

		// MM packs four bicolor TRIG LEDs into each bank. Within each pair the
		// even bit drives green and the odd bit drives red; both are active-low.
		const auto bank = static_cast<uint8_t>(g_firstLedBank + (_index >> 2));
		const uint8_t raw = m_ledBank[bankIndex(bank)];
		const uint32_t greenBit = (_index & 3) << 1;
		const bool green = ((raw >> greenBit) & 1) == 0;
		const bool red = ((raw >> (greenBit + 1)) & 1) == 0;
		if(green && red)
			return LedColor::Yellow;
		if(green)
			return LedColor::Green;
		if(red)
			return LedColor::Red;
		return LedColor::Off;
	}

	bool FrontPanel::getDrumLed(uint32_t _index) const
	{
		if (_index >= 16)
			return false;
		const uint8_t raw = m_ledBank[bankIndex(_index < 8 ? LedBank::Drum0 : LedBank::Drum1)];
		const uint32_t bit = _index & 7;
		return ((raw >> bit) & 1) == 0;
	}

	bool FrontPanel::getStatusLed(StatusLed _led) const
	{
		const uint8_t raw = m_ledBank[bankIndex(LedBank::Status)];
		return ((raw >> static_cast<uint8_t>(_led)) & 1) == 0;
	}

	bool FrontPanel::getModeLed(ModeLed _led) const
	{
		const uint8_t raw = m_ledBank[bankIndex(LedBank::Mode)];
		return ((raw >> static_cast<uint8_t>(_led)) & 1) == 0;
	}

	bool FrontPanelPublisher::tryPublish(const FrontPanel& _panel)
	{
		std::unique_lock lock(m_mutex, std::try_to_lock);
		if(!lock.owns_lock())
			return false;
		m_snapshot = _panel;
		return true;
	}

	bool FrontPanelPublisher::tryRead(FrontPanel& _panel) const
	{
		std::unique_lock lock(m_mutex, std::try_to_lock);
		if(!lock.owns_lock())
			return false;
		_panel = m_snapshot;
		return true;
	}

	FrontPanel FrontPanelPublisher::read() const
	{
		const std::lock_guard lock(m_mutex);
		return m_snapshot;
	}

	void FrontPanelPublisher::reset()
	{
		const std::lock_guard lock(m_mutex);
		m_snapshot.reset();
	}
}
