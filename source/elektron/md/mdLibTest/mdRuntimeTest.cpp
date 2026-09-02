#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdsim.h"
#include "dsp56kEmu/memory.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace
{
	bool check(const bool _condition, const char* const _message)
	{
		if(_condition)
			return true;
		std::cerr << _message << '\n';
		return false;
	}

	bool testDspMemoryFallback()
	{
		dsp56k::DefaultMemoryValidator validator;
		dsp56k::Memory memory(validator, 16, 16, 16);
		if(!check(!memory.hasMmuSupport(), "fallback memory unexpectedly uses an MMU mapping"))
			return false;

		memory.set(dsp56k::MemArea_Y, 3, 0x123456);
		return check(memory.get(dsp56k::MemArea_Y, 3) == 0x123456,
			"fallback memory did not preserve the written value")
			&& check(memory.get(dsp56k::MemArea_X, 3) == 0,
				"fallback memory did not preserve separate memory areas");
	}

	bool testMk2PortAInvertedLoopback()
	{
		md::Sim sim;
		sim.write8(md::Sim::g_ppddr, 0x04);
		sim.setMk2PortAInvertedLoopback(true);
		sim.write8(md::Sim::g_ppdat, 0x04);
		if(!check((sim.read8(md::Sim::g_ppdat) & 0x01) == 0,
			"MKII Port A loopback did not invert HIGH"))
			return false;
		sim.write8(md::Sim::g_ppdat, 0x00);
		return check((sim.read8(md::Sim::g_ppdat) & 0x01) != 0,
			"MKII Port A loopback did not invert LOW");
	}

	bool testFrontPanelStepLeds()
	{
		using LedColor = md::FrontPanel::LedColor;
		struct MmStepFixture
		{
			uint8_t bank;
			uint8_t greenMask;
			uint8_t redMask;
		};
		// Fixed protocol fixtures captured from OS 1.32b UART output while toggling
		// TRIG 1..16 in grid-recording mode. Four green/red LED pairs occupy each
		// active-low bank. Keep this table explicit so a production indexing error
		// is not copied into the expected values.
		constexpr std::array<MmStepFixture, 16> mmSteps{{
			{0x20, 0x01, 0x02}, {0x20, 0x04, 0x08},
			{0x20, 0x10, 0x20}, {0x20, 0x40, 0x80},
			{0x21, 0x01, 0x02}, {0x21, 0x04, 0x08},
			{0x21, 0x10, 0x20}, {0x21, 0x40, 0x80},
			{0x22, 0x01, 0x02}, {0x22, 0x04, 0x08},
			{0x22, 0x10, 0x20}, {0x22, 0x40, 0x80},
			{0x23, 0x01, 0x02}, {0x23, 0x04, 0x08},
			{0x23, 0x10, 0x20}, {0x23, 0x40, 0x80},
		}};
		// The firmware produced ff/7f/3f/bf for step 16 when it was respectively
		// off/red/yellow/green, confirming that red is odd, green even, and yellow both.
		struct ColorFixture
		{
			uint8_t activeMask;
			LedColor color;
		};
		for(uint32_t step = 0; step < mmSteps.size(); ++step)
		{
			const auto& fixture = mmSteps[step];
			const std::array<ColorFixture, 4> colors{{
				{0x00, LedColor::Off},
				{fixture.greenMask, LedColor::Green},
				{fixture.redMask, LedColor::Red},
				{static_cast<uint8_t>(fixture.greenMask | fixture.redMask), LedColor::Yellow},
			}};
			for(const auto& color : colors)
			{
				md::FrontPanel panel;
				const uint8_t message[] = {
					fixture.bank, static_cast<uint8_t>(~color.activeMask)};
				panel.processBytes(message, sizeof(message));
				for(uint32_t candidate = 0; candidate < mmSteps.size(); ++candidate)
				{
					const auto expected = candidate == step ? color.color : LedColor::Off;
					if(!check(panel.getMonomachineStepLedColor(candidate) == expected,
						"Monomachine step LED color mapping is wrong"))
						return false;
				}
			}
		}

		md::FrontPanel panel;
		const uint8_t mdMessage[] = {0x20, 0xfe, 0x21, 0x7f};
		panel.processBytes(mdMessage, sizeof(mdMessage));
		return check(panel.getStepLed(0) && panel.getStepLed(15),
			"Machinedrum step LED mapping changed")
			&& check(!panel.getStepLed(1) && !panel.getStepLed(14),
				"Machinedrum unlit step decoding changed")
			&& check(!panel.getStepLed(16)
				&& panel.getMonomachineStepLedColor(16) == LedColor::Off,
				"out-of-range step LED was accepted");
	}

	bool testFrontPanelTransitionPublication()
	{
		md::FrontPanel panel;
		if(!check(!panel.processByte(0x26),
			"LED command byte was reported as a complete write"))
			return false;
		const auto on = panel.processByte(0x7f);
		if(!check(on && on->command == 0x26 && on->value == 0x7f,
			"completed LED change was not reported"))
			return false;
		panel.processByte(0x26);
		if(!check(!panel.processByte(0x7f),
			"unchanged LED bank produced a transition"))
			return false;
		panel.processByte(0x26);
		const auto off = panel.processByte(0xff);
		if(!check(off && off->command == 0x26 && off->value == 0xff,
			"LED off transition was not reported"))
			return false;

		md::FrontPanelPublisher publisher;
		if(!check(publisher.tryPushLedTransition(0x26, 0x7f, 100),
			"first LED transition was rejected")
			|| !check(publisher.tryPushLedTransition(0x26, 0xff, 120),
				"second LED transition was rejected")
			|| !check(publisher.tryPublish(panel),
				"front-panel snapshot was not published"))
			return false;

		const auto published = publisher.readPublishedState();
		std::array<md::FrontPanelLedTransition, 2> transitions;
		const auto count = publisher.drainLedTransitions(
			transitions.data(), transitions.size());
		if(!check(published.ledSequence == 2,
			"snapshot did not carry its LED sequence")
			|| !check(count == 2 && transitions[0].sequence == 1
				&& transitions[0].value == 0x7f
				&& transitions[1].sequence == 2
				&& transitions[1].value == 0xff,
				"LED transitions were not drained losslessly in order"))
			return false;

		for(size_t i = 0; i < md::FrontPanelPublisher::g_ledTransitionCapacity; ++i)
			if(!publisher.tryPushLedTransition(0x20,
				static_cast<uint8_t>(i), i))
				return check(false, "LED transition queue filled too early");
		if(!check(!publisher.tryPushLedTransition(0x20, 0xff, 9999),
			"LED transition overflow was not reported")
			|| !check(publisher.getLedTransitionStatus().dropped == 1,
				"LED transition drop telemetry is wrong"))
			return false;

		const auto epoch = publisher.getLedTransitionStatus().epoch;
		publisher.reset();
		return check(publisher.getLedTransitionStatus().epoch == epoch + 1,
			"LED transition reset did not advance its epoch")
			&& check(publisher.getLedTransitionStatus().dropped == 0,
				"LED transition reset retained drop telemetry");
	}
}

int main()
{
	if(!testDspMemoryFallback() || !testMk2PortAInvertedLoopback()
		|| !testFrontPanelStepLeds() || !testFrontPanelTransitionPublication())
		return 1;
	std::cout << "mdLib tests passed\n";
	return 0;
}
