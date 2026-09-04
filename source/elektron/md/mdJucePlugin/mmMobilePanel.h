#pragma once

#include <array>
#include <functional>
#include <memory>

#include "juce_gui_basics/juce_gui_basics.h"

#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdpanel.h"

namespace mdJucePlugin
{
	class MmMobilePanel final : public juce::Component
	{
	public:
		struct Callbacks
		{
			std::function<void(juce::Graphics&, juce::Rectangle<int>)> paintLcd;
			std::function<void(md::PanelControl, bool)> setControlPressed;
			std::function<void(md::PanelEncoder, int)> turnEncoder;
			std::function<void(int)> selectDataPage;
		};

		explicit MmMobilePanel(Callbacks _callbacks);
		~MmMobilePanel() override;

		void refresh(const md::FrontPanel& _frontPanel, bool _lcdChanged);

		void paint(juce::Graphics& _graphics) override;
		void resized() override;
		void parentSizeChanged() override;

	private:
		class DisplaySection;
		class ButtonRow;
		class KnobGrid;
		class StepGrid;

		Callbacks m_callbacks;
		std::unique_ptr<DisplaySection> m_displaySection;
		std::unique_ptr<ButtonRow> m_trackButtons;
		std::unique_ptr<ButtonRow> m_editPageButtons;
		std::unique_ptr<ButtonRow> m_setupButtons;
		std::unique_ptr<ButtonRow> m_navigationButtons;
		std::unique_ptr<ButtonRow> m_bankButtons;
		std::unique_ptr<ButtonRow> m_transportButtons;
		std::unique_ptr<KnobGrid> m_knobGrid;
		std::unique_ptr<StepGrid> m_stepGrid;

		std::array<juce::Component*, 6> m_trackButtonIndicators{};
		std::array<juce::Component*, 7> m_editPageIndicators{};
		juce::Component* m_patternSongIndicator = nullptr;
		juce::Component* m_recordIndicator = nullptr;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MmMobilePanel)
	};
}
