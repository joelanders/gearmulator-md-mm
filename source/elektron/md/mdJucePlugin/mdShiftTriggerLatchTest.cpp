#include "mdPanelAffordances.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
	using mdJucePlugin::panelAffordances::ShiftTriggerLatch;

	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;

		std::cerr << "mdShiftTriggerLatchTest: " << _message << '\n';
		std::exit(1);
	}

	void checkLatchPolicy()
	{
		ShiftTriggerLatch latch;
		expect(latch.empty() && latch.size() == 0, "new latch is not empty");
		expect(!latch.latch(md::PanelControl::Function),
			"non-trig control was accepted");
		expect(latch.latch(md::PanelControl::Trigger1), "first trig was rejected");
		expect(!latch.latch(md::PanelControl::Trigger1),
			"duplicate trig was accepted");
		expect(latch.latch(md::PanelControl::Trigger8), "second trig was rejected");
		expect(latch.latch(md::PanelControl::Trigger16), "last trig was rejected");
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

	void checkPanelRows(const md::MachineModel _model)
	{
		ShiftTriggerLatch latch;
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
			expect(latch.latch(control), "unique trig was rejected");
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
	checkPanelRows(md::MachineModel::Machinedrum);
	checkPanelRows(md::MachineModel::Monomachine);

	std::cout << "mdShiftTriggerLatchTest: PASS\n";
	return 0;
}
