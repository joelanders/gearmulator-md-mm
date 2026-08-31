#include "mdPanelAffordances.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
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

	std::cout << "mdShiftTriggerLatchTest: PASS\n";
	return 0;
}
