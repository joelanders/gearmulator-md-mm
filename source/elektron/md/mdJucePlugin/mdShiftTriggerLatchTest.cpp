#include "mdPanelAffordances.h"
#include "mdFrontPanelPresentation.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
	static_assert(static_cast<uint8_t>(md::FrontPanel::StatusLed::Page1) == 0);
	static_assert(static_cast<uint8_t>(md::FrontPanel::StatusLed::Page2) == 1);
	static_assert(static_cast<uint8_t>(md::FrontPanel::StatusLed::Page3) == 2);
	static_assert(static_cast<uint8_t>(md::FrontPanel::ModeLed::Tempo) == 5);
	static_assert(static_cast<uint8_t>(md::FrontPanel::ModeLed::Page4) == 6);

	using mdJucePlugin::panelAffordances::ShiftPanelLatch;
	using PressAction = ShiftPanelLatch::PressAction;

	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;

		std::cerr << "mdShiftTriggerLatchTest: " << _message << '\n';
		std::exit(1);
	}

	void checkLatchPolicy()
	{
		ShiftPanelLatch latch;
		expect(latch.empty() && latch.size() == 0, "new latch is not empty");
		expect(latch.press(md::PanelControl::Trigger1, true) == PressAction::Latched,
			"first trig was rejected");
		expect(latch.press(md::PanelControl::Trigger1, true) == PressAction::Ignored,
			"duplicate trig was accepted");
		expect(latch.press(md::PanelControl::Trigger8, true) == PressAction::Latched,
			"second trig was rejected");
		expect(latch.press(md::PanelControl::Trigger16, true) == PressAction::Latched,
			"last trig was rejected");
		expect(latch.press(md::PanelControl::Play, true) == PressAction::Momentary,
			"action after held trigs was not momentary");
		expect(latch.size() == 3, "latch size is wrong");
		expect(latch.contains(md::PanelControl::Trigger1)
			&& latch.contains(md::PanelControl::Trigger8)
			&& latch.contains(md::PanelControl::Trigger16),
			"latched trig was not retained");

		std::vector<md::PanelControl> released;
		latch.releaseAll([&released](const md::PanelControl _control)
		{
			released.push_back(_control);
		});

		const std::vector<md::PanelControl> expected
		{
			md::PanelControl::Trigger16,
			md::PanelControl::Trigger8,
			md::PanelControl::Trigger1,
		};
		expect(released == expected, "trigs were not released in reverse order");
		expect(latch.empty() && !latch.contains(md::PanelControl::Trigger1),
			"release did not clear latch state");
	}

	void checkSingleModifierPolicy()
	{
		ShiftPanelLatch latch;
		expect(latch.press(md::PanelControl::Function, false) == PressAction::Momentary,
			"unmodified press was not momentary");
		expect(latch.empty(), "unmodified press changed latch state");
		expect(latch.press(md::PanelControl::Function, true) == PressAction::Latched,
			"non-trig modifier was not latched");
		expect(latch.contains(md::PanelControl::Function) && latch.size() == 1,
			"held modifier state is wrong");
		expect(latch.press(md::PanelControl::Tempo, true) == PressAction::Momentary,
			"target after held modifier was also latched");
		expect(latch.press(md::PanelControl::Trigger4, true) == PressAction::Momentary,
			"trig target after held modifier was also latched");
		expect(latch.press(md::PanelControl::Function, true) == PressAction::Ignored,
			"duplicate held modifier was not ignored");

		std::vector<md::PanelControl> released;
		latch.releaseAll([&released](const auto control) { released.push_back(control); });
		expect(released == std::vector<md::PanelControl>{md::PanelControl::Function},
			"single modifier release was not exact");
		expect(latch.empty(), "single modifier release left state behind");
	}

	void checkChordRows(const md::MachineModel _model,
		const md::PanelControl _held, const md::PanelControl _target)
	{
		ShiftPanelLatch latch;
		md::PanelRowState rows;
		const auto heldPacket = md::panelPacket(_model, _held);
		const auto targetPacket = md::panelPacket(_model, _target);
		expect(heldPacket.has_value() && targetPacket.has_value(),
			"chord control has no panel packet");
		expect(latch.press(_held, true) == PressAction::Latched,
			"chord modifier was not latched");
		rows.press(*heldPacket);
		expect(latch.press(_target, true) == PressAction::Momentary,
			"chord target was not momentary");
		rows.press(*targetPacket);
		const auto combinedMask = heldPacket->row == targetPacket->row
			? static_cast<uint8_t>(heldPacket->mask | targetPacket->mask)
			: heldPacket->mask;
		expect(rows.mask(heldPacket->row) == combinedMask,
			"chord press did not preserve the held control");
		if(heldPacket->row != targetPacket->row)
			expect(rows.mask(targetPacket->row) == targetPacket->mask,
				"chord target was not present in its row");

		// Action first, modifier second: this is the editor's forced-release order.
		rows.release(*targetPacket);
		expect(rows.mask(heldPacket->row) == heldPacket->mask,
			"releasing the chord target also released its modifier");
		if(heldPacket->row != targetPacket->row)
			expect(rows.mask(targetPacket->row) == 0,
				"releasing the chord target left its row held");
		latch.releaseAll([&](const auto control)
		{
			const auto packet = md::panelPacket(_model, control);
			expect(packet.has_value(), "held chord control lost its packet");
			rows.release(*packet);
		});
		for(uint8_t row = 0x20; row <= 0x25; ++row)
			expect(rows.mask(row) == 0, "chord release left a panel row held");
	}

	void checkPanelRows(const md::MachineModel _model)
	{
		ShiftPanelLatch latch;
		md::PanelRowState rows;
		constexpr std::array controls
		{
			md::PanelControl::Trigger1,
			md::PanelControl::Trigger6,
			md::PanelControl::Trigger11,
			md::PanelControl::Trigger16,
		};

		for(const auto control : controls)
		{
			const auto packet = md::panelPacket(_model, control);
			expect(packet.has_value(), "trig has no panel packet");
			expect(latch.press(control, true) == PressAction::Latched,
				"unique trig was rejected");
			rows.press(*packet);
		}

		size_t releaseCount = 0;
		latch.releaseAll([&](const md::PanelControl _control)
		{
			const auto packet = md::panelPacket(_model, _control);
			expect(packet.has_value(), "latched trig lost its panel packet");
			rows.release(*packet);
			++releaseCount;
		});

		expect(releaseCount == controls.size(), "not every held trig was released");
		for(uint8_t row = 0x20; row <= 0x25; ++row)
			expect(rows.mask(row) == 0, "release left a panel row held");
	}

	void checkLedPulsePresentation()
	{
		md::FrontPanel panel;
		mdJucePlugin::FrontPanelLedPresentation presentation;
		presentation.reset(panel);
		expect(!presentation.isLit(0x23, 5), "tempo LED baseline is not off");

		constexpr double now = 1000.0;
		presentation.apply({1, 100, 0x23, 0xdf}, now);
		presentation.apply({2, 120, 0x23, 0xff}, now);
		presentation.advance(now);
		expect(presentation.isLit(0x23, 5),
			"pulse collapsed inside one UI frame");
		presentation.advance(now
			+ mdJucePlugin::FrontPanelLedPresentation::g_minimumVisibleMilliseconds
			- 0.01);
		expect(presentation.isLit(0x23, 5),
			"short pulse expired before one slow-renderer frame");
		presentation.advance(now
			+ mdJucePlugin::FrontPanelLedPresentation::g_minimumVisibleMilliseconds);
		expect(!presentation.isLit(0x23, 5),
			"short pulse did not return to firmware state");

		presentation.apply({3, 200, 0x26, 0x7f}, now + 100.0);
		presentation.advance(now + 100.0);
		expect(presentation.isLit(0x26, 7),
			"steady Monomachine tempo LED did not light");
		presentation.advance(now + 1000.0);
		expect(presentation.isLit(0x26, 7),
			"steady LED was treated as a finite pulse");

		constexpr uint64_t cyclesPerMillisecond =
			md::g_frontPanelEmulationClockHz / 1000;
		const auto oldPulseStart =
			mdJucePlugin::FrontPanelLedPresentation::eventMilliseconds(
				now + 2000.0, 2000 * cyclesPerMillisecond,
				100 * cyclesPerMillisecond);
		const auto oldPulseEnd =
			mdJucePlugin::FrontPanelLedPresentation::eventMilliseconds(
				now + 2000.0, 2000 * cyclesPerMillisecond,
				110 * cyclesPerMillisecond);
		presentation.apply({4, 100 * cyclesPerMillisecond, 0x23, 0xdf},
			oldPulseStart);
		presentation.apply({5, 110 * cyclesPerMillisecond, 0x23, 0xff},
			oldPulseEnd);
		presentation.advance(now + 2000.0);
		expect(!presentation.isLit(0x23, 5),
			"stale pulse was restamped as a new UI-frame flash");

		// MD 1.63 emits the tempo lamp as an approximately 110 ms pulse once
		// per 428 ms beat at the factory test pattern's 140 BPM. Preserve that
		// firmware duty cycle instead of stretching it to a 50/50 blink.
		md::FrontPanel tempoPanel;
		mdJucePlugin::FrontPanelLedPresentation tempoPresentation;
		tempoPresentation.reset(tempoPanel);
		tempoPresentation.apply({1, 0, 0x23, 0xdf}, 0.0);
		for(int frame = 0; frame <= 6; ++frame)
		{
			tempoPresentation.advance(frame * 16.0);
			expect(tempoPresentation.isLit(0x23, 5),
				"real-duration tempo pulse went dark too early");
		}
		tempoPresentation.apply({2, 110, 0x23, 0xff}, 110.0);
		tempoPresentation.advance(112.0);
		expect(!tempoPresentation.isLit(0x23, 5),
			"tempo pulse was stretched past the firmware off edge");
		tempoPresentation.advance(416.0);
		expect(!tempoPresentation.isLit(0x23, 5),
			"tempo LED relit before the next firmware beat");
		tempoPresentation.apply({3, 428, 0x23, 0xdf}, 428.0);
		tempoPresentation.advance(432.0);
		expect(tempoPresentation.isLit(0x23, 5),
			"next tempo beat did not produce a visible pulse");

		const auto recentPulseStart =
			mdJucePlugin::FrontPanelLedPresentation::eventMilliseconds(
				now + 3000.0, 3000 * cyclesPerMillisecond,
				2990 * cyclesPerMillisecond);
		const auto recentPulseEnd =
			mdJucePlugin::FrontPanelLedPresentation::eventMilliseconds(
				now + 3000.0, 3000 * cyclesPerMillisecond,
				2995 * cyclesPerMillisecond);
		presentation.apply({6, 2990 * cyclesPerMillisecond, 0x23, 0xdf},
			recentPulseStart);
		presentation.apply({7, 2995 * cyclesPerMillisecond, 0x23, 0xff},
			recentPulseEnd);
		presentation.advance(now + 3000.0);
		expect(presentation.isLit(0x23, 5),
			"recent sub-frame pulse was not held for its remaining visibility time");
		presentation.advance(now + 3024.0);
		expect(!presentation.isLit(0x23, 5),
			"recent sub-frame pulse outlived its timestamped visibility window");

		const auto futureEvent =
			mdJucePlugin::FrontPanelLedPresentation::eventMilliseconds(
				now + 3000.0, 2000 * cyclesPerMillisecond,
				2001 * cyclesPerMillisecond);
		expect(futureEvent == now + 3000.0,
			"transition newer than the snapshot was not clamped to the UI frame");
	}

	void checkIndependentTimerCadence()
	{
		expect(mdJucePlugin::panelAffordances::g_presentationTimerIntervalMilliseconds == 16,
			"presentation timer no longer models JUCE 60 Hz quantization");
		expect(mdJucePlugin::panelAffordances::g_panelTimerIntervalMilliseconds == 33,
			"panel timer no longer preserves its established cadence");
		expect(mdJucePlugin::panelAffordances::g_panelTimerIntervalMilliseconds
			> mdJucePlugin::panelAffordances::g_presentationTimerIntervalMilliseconds * 2,
			"panel cadence was derived from two 16 ms presentation callbacks");
	}

}

int main()
{
	checkLatchPolicy();
	checkSingleModifierPolicy();
	checkPanelRows(md::MachineModel::Machinedrum);
	checkPanelRows(md::MachineModel::Monomachine);
	checkChordRows(md::MachineModel::Machinedrum,
		md::PanelControl::Record, md::PanelControl::Play);
	checkChordRows(md::MachineModel::Machinedrum,
		md::PanelControl::Scale, md::PanelControl::Stop);
	checkChordRows(md::MachineModel::Monomachine,
		md::PanelControl::Stop, md::PanelControl::Record);
	checkChordRows(md::MachineModel::Monomachine,
		md::PanelControl::DataPageBackward, md::PanelControl::DataPageForward);
	checkLedPulsePresentation();
	checkIndependentTimerCadence();

	std::cout << "mdShiftTriggerLatchTest: PASS\n";
	return 0;
}
