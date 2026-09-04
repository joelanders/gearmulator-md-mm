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

		std::cerr << "mdShiftPanelLatchTest: " << _message << '\n';
		std::exit(1);
	}

	void checkTriggerChordPolicy()
	{
		ShiftPanelLatch latch;
		expect(latch.empty(), "new latch is not empty");
		expect(latch.press(md::PanelControl::Trigger1, true) == PressAction::Latched,
			"first Shift-trig was not latched");
		expect(latch.press(md::PanelControl::Trigger1, true) == PressAction::Ignored,
			"duplicate Shift-trig was accepted");
		expect(latch.press(md::PanelControl::Trigger8, true) == PressAction::Latched,
			"second Shift-trig was not latched");
		expect(latch.press(md::PanelControl::Trigger16, true) == PressAction::Latched,
			"last Shift-trig was not latched");
		expect(latch.press(md::PanelControl::Play, true) == PressAction::Momentary,
			"non-trig after a trig chord was latched");
		expect(latch.size() == 3, "trigger chord size is wrong");

		std::vector<md::PanelControl> released;
		latch.releaseAll([&released](const auto control) { released.push_back(control); });
		const std::vector<md::PanelControl> expected
		{
			md::PanelControl::Trigger16,
			md::PanelControl::Trigger8,
			md::PanelControl::Trigger1,
		};
		expect(released == expected, "held trigs were not released in reverse order");
		expect(latch.empty(), "trigger chord release left latch state behind");
	}

	void checkModifierChordPolicy()
	{
		ShiftPanelLatch latch;
		expect(latch.press(md::PanelControl::Function, false) == PressAction::Momentary,
			"ordinary press was not momentary");
		expect(latch.empty(), "ordinary press changed latch state");
		expect(latch.press(md::PanelControl::Function, true) == PressAction::Latched,
			"Shift-held modifier was not latched");
		expect(latch.press(md::PanelControl::Tempo, true) == PressAction::Momentary,
			"target after a held modifier was also latched");
		expect(latch.press(md::PanelControl::Trigger4, true) == PressAction::Momentary,
			"trig after a held modifier was also latched");
		expect(latch.contains(md::PanelControl::Function) && latch.size() == 1,
			"held modifier state is wrong");

		std::vector<md::PanelControl> released;
		latch.releaseAll([&released](const auto control) { released.push_back(control); });
		expect(released == std::vector<md::PanelControl>{md::PanelControl::Function},
			"modifier release was not exact");
	}

	void checkChordRows(const md::MachineModel _model,
		const md::PanelControl _held, const md::PanelControl _target)
	{
		ShiftPanelLatch latch;
		md::PanelRowState rows;
		const auto heldPacket = md::panelPacket(_model, _held);
		const auto targetPacket = md::panelPacket(_model, _target);
		expect(heldPacket && targetPacket, "chord control has no panel packet");
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
			"target press did not preserve the held modifier");
		if(heldPacket->row != targetPacket->row)
			expect(rows.mask(targetPacket->row) == targetPacket->mask,
				"target was not present in its panel row");

		// The editor releases the action before its modifier.
		rows.release(*targetPacket);
		expect(rows.mask(heldPacket->row) == heldPacket->mask,
			"target release also released its modifier");
		latch.releaseAll([&](const auto control)
		{
			const auto packet = md::panelPacket(_model, control);
			expect(packet.has_value(), "held control lost its panel packet");
			rows.release(*packet);
		});

		for(uint8_t row = 0x20; row <= 0x25; ++row)
			expect(rows.mask(row) == 0, "chord release left a panel row held");
	}

	void checkTriggerRows(const md::MachineModel _model)
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
				"unique Shift-trig was rejected");
			rows.press(*packet);
		}

		latch.releaseAll([&](const auto control)
		{
			const auto packet = md::panelPacket(_model, control);
			expect(packet.has_value(), "latched trig lost its panel packet");
			rows.release(*packet);
		});
		for(uint8_t row = 0x20; row <= 0x25; ++row)
			expect(rows.mask(row) == 0, "trig release left a panel row held");
	}
}

int main()
{
	checkTriggerChordPolicy();
	checkModifierChordPolicy();
	checkTriggerRows(md::MachineModel::Machinedrum);
	checkTriggerRows(md::MachineModel::Monomachine);
	checkChordRows(md::MachineModel::Machinedrum,
		md::PanelControl::Record, md::PanelControl::Play);
	checkChordRows(md::MachineModel::Machinedrum,
		md::PanelControl::Scale, md::PanelControl::Stop);
	checkChordRows(md::MachineModel::Monomachine,
		md::PanelControl::Stop, md::PanelControl::Record);
	checkChordRows(md::MachineModel::Monomachine,
		md::PanelControl::DataPageBackward, md::PanelControl::DataPageForward);

	std::cout << "mdShiftPanelLatchTest: PASS\n";
	return 0;
}
