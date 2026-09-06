#pragma once

#include <memory>

namespace juce { class Image; class Graphics; }
namespace juceRmlUi { class RmlComponent; class ElemCanvas; }

namespace mdJucePlugin
{
	// Single owner/gate for the removable rendering experiment. The permanent
	// aspect-ratio and settings-resource identity fixes do not depend on it.
	class PixelPerfectPanel
	{
	public:
		static constexpr const char* configKey = "pixelPerfectPanel";
		static constexpr bool defaultEnabled = false;
		static constexpr const char* ruleClass = "elektronPixelRule";

		PixelPerfectPanel();
		~PixelPerfectPanel();
		void apply(juceRmlUi::RmlComponent& _component, juceRmlUi::ElemCanvas* _canvas, bool _enabled);
		// Returns false to use the editor's normal aspect-fit painter.
		bool paintLcd(const juce::Image& _lcd, juce::Graphics& _graphics) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
