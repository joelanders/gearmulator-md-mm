#include "mdPixelPerfectPanel.h"

#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/juceRmlLookAndFeel.h"
#include "juceRmlUi/rmlDataProvider.h"
#include "juceRmlUi/rmlElemCanvas.h"
#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/ElementDocument.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace juceRmlUi
{
	struct RenderingTestAccess
	{
		static void update(RmlComponent& _component) { _component.update(); }
		static void requirePaddedTextures(RmlComponent& _component)
		{
			_component.m_renderProxy->setTextureParameters(4096, false);
		}
	};
}

namespace
{
	void require(bool _condition, const char* _message)
	{
		if (!_condition)
			throw std::runtime_error(_message);
	}

	class Resources : public juceRmlUi::DataProvider
	{
		const std::string rml = R"(<rml><head ><style>
			body { width: 512dp; height: 256dp; background-color: #8899aa; }
			div { position: absolute; }
			</style></head><body>
			<div id="lcd" style="left: 30.25dp; top: 40.25dp; width: 220.5dp; height: 116.5dp;"/>
			<div id="rule" class="elektronPixelRule" style="left: 300.25dp; top: 50.25dp; width: 51.25dp; height: 1dp; background-color: #345678;"/>
			<div id="unmarked" style="left: 300.25dp; top: 60.25dp; width: 51.25dp; height: 1dp; background-color: #345678;"/>
			<div id="teardown" class="elektronPixelRule" style="left: 300.25dp; top: 70.25dp; width: 51.25dp; height: 1dp; background-color: #345678;"/>
			<div id="sentinel" style="left: 400dp; top: 200dp; width: 20dp; height: 20dp; background-color: #ff0000;"/>
			</body></rml>)";
	public:
		const char* getResourceByFilename(const std::string& _name, uint32_t& _size) override
		{
			_size = static_cast<uint32_t>(rml.size());
			return _name == "test.rml" ? rml.data() : nullptr;
		}
		std::vector<std::string> getAllFilenames() override { return {}; }
	};

	void samePixels(const juce::Image& _a, const juce::Image& _b)
	{
		require(_a.getBounds() == _b.getBounds(), "toggle changed output dimensions");
		for (int y = 0; y < _a.getHeight(); ++y)
			for (int x = 0; x < _a.getWidth(); ++x)
				require(_a.getPixelAt(x, y) == _b.getPixelAt(x, y), "toggle changed a pending frame or failed to restore baseline");
	}

	void testRendering()
	{
		Resources resources;
		juceRmlUi::RmlInterfaces interfaces(resources);
		juceRmlUi::RmlComponentConfig config;
		config.forceSoftwareRenderer = juceRmlUi::SoftwareRendererMode::ForceOn;
		juceRmlUi::LookAndFeel lookAndFeel;
		juceRmlUi::RmlComponent component(interfaces, resources, "test.rml", 1.f, {}, {}, config);
		component.setLookAndFeel(&lookAndFeel);
		auto* doc = component.getDocument();
		auto* area = doc->GetElementById("lcd");
		auto* canvas = juceRmlUi::ElemCanvas::create(area);
		auto experiment = std::make_unique<mdJucePlugin::PixelPerfectPanel>();
		juce::Image lcd(juce::Image::ARGB, 128, 64, false);
		for (int y = 0; y < 64; ++y)
			for (int x = 0; x < 128; ++x)
				lcd.setPixelAt(x, y, (x + y) % 2 ? juce::Colours::white : juce::Colours::black);
		canvas->setRepaintGraphicsCallback([&](juce::Image& _target, juce::Graphics& _g)
		{
			_g.fillAll(juce::Colours::green);
			_g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
			if (!experiment || !experiment->paintLcd(lcd, _g))
				_g.drawImageWithin(lcd, 0, 0, _target.getWidth(), _target.getHeight(), juce::RectanglePlacement::centred);
		});
		auto paint = [&](float _dpi)
		{
			juce::Image result(juce::Image::ARGB, static_cast<int>(std::ceil(component.getWidth() * _dpi)),
				static_cast<int>(std::ceil(component.getHeight() * _dpi)), true);
			// Exercise both the legacy direct image-copy shortcut and its guarded
			// fallback when the backing image has a different pixel size.
			lookAndFeel.getCurrentImage() = result;
			juce::Graphics g(result);
			g.addTransform(juce::AffineTransform::scale(_dpi));
			component.paint(g);
			return result;
		};
		auto settle = [&](float _dpi)
		{
			juce::Image result;
			for (int frame = 0; frame < 6; ++frame)
			{
				juceRmlUi::RenderingTestAccess::update(component);
				result = paint(_dpi);
			}
			return result;
		};
		for (float dpi : {1.f, 1.25f, 2.f})
		{
			const auto baseline = settle(dpi);
			experiment->apply(component, canvas, true);
			samePixels(baseline, paint(dpi)); // Paint already queued geometry before any layout update.
			const auto crisp = settle(dpi);
			require(doc->GetElementById("unmarked")->GetNumChildren() == 0, "unmarked hairline was altered");
			require(doc->GetElementById("rule")->GetNumChildren() == 1, "explicit rule was not attached exactly once");
			const auto size = canvas->getPaintSize();
			const auto pos = canvas->GetAbsoluteOffset(Rml::BoxArea::Border).Round();
			const int k = std::min(size.x / 128, size.y / 64);
			require(k > 0, "test framebuffer should fit");
			const int ox = static_cast<int>(pos.x) + (size.x - 128 * k) / 2;
			const int oy = static_cast<int>(pos.y) + (size.y - 64 * k) / 2;
			const auto black = crisp.getPixelAt(ox, oy), white = crisp.getPixelAt(ox + k, oy);
			require(black.getBrightness() < .02f && white.getBrightness() > .98f, "LCD palette lost");
			for (int y = 0; y < 64 * k; ++y)
				for (int x = 0; x < 128 * k; ++x)
					require(crisp.getPixelAt(ox + x, oy + y) == ((x / k + y / k) % 2 ? white : black), "unequal or filtered LCD pixels");
			experiment->apply(component, canvas, false);
			samePixels(crisp, paint(dpi));
			samePixels(baseline, settle(dpi));
		}

		// Display changes can produce another paint before RML lays out a new frame.
		experiment->apply(component, canvas, true);
		settle(1.f);
		const auto firstRetinaPaint = paint(2.f);
		require(firstRetinaPaint.getPixelAt(820, 420) == juce::Colours::red, "pending frame moved the panel");
		samePixels(firstRetinaPaint, paint(2.f));
		settle(2.f);
		require(component.toRmlPosition(410, 210) == juce::Point<int>(820, 420), "pointer mapping did not follow backing density");

		// Simulate a renderer requiring power-of-two textures and a sub-native LCD.
		juceRmlUi::RenderingTestAccess::requirePaddedTextures(component);
		area->SetProperty("width", "80.5dp");
		area->SetProperty("height", "40.5dp");
		for (int y = 0; y < 64; ++y)
			for (int x = 0; x < 128; ++x)
				lcd.setPixelAt(x, y, y >= 56 ? juce::Colours::blue : x >= 120 ? juce::Colours::red : juce::Colours::white);
		canvas->repaint();
		const auto small = settle(1.f);
		const auto pos = canvas->GetAbsoluteOffset(Rml::BoxArea::Border).Round();
		const auto right = small.getPixelAt(static_cast<int>(pos.x) + 79, static_cast<int>(pos.y) + 10);
		const auto bottom = small.getPixelAt(static_cast<int>(pos.x) + 10, static_cast<int>(pos.y) + 39);
		require(right.getRed() > 250 && right.getGreen() < 5 && right.getBlue() < 5, "padded fallback cropped right edge");
		require(bottom.getBlue() > 250 && bottom.getRed() < 5 && bottom.getGreen() < 5, "padded fallback cropped bottom edge");

		// Removing a marked node must not leave a dangling pointer in the controller.
		auto* rule = doc->GetElementById("rule");
		rule->GetParentNode()->RemoveChild(rule).reset();
		experiment->apply(component, canvas, false);
		experiment->apply(component, canvas, true);
		settle(1.f);
		// A detached element can remain alive in a caller-owned ElementPtr.
		auto* remaining = doc->GetElementById("teardown");
		auto detachedHelper = remaining->RemoveChild(remaining->GetChild(0));
		experiment->apply(component, canvas, false);
		detachedHelper.reset();
		// Re-enable with an intact authored background so teardown also exercises
		// restoring and removing an attached helper.
		remaining->SetProperty("background-color", "#345678");
		experiment->apply(component, canvas, true);
		settle(1.f);
		auto detachedCanvas = area->RemoveChild(canvas);
		experiment.reset();
		settle(1.f);
		Rml::ElementList helpers;
		doc->GetElementsByTagName(helpers, "pixelpanelrule");
		require(helpers.empty(), "removing experiment left helper elements behind");
		require(doc->GetElementById("teardown")->GetProperty("background-color")->Get<Rml::Colourb>(doc->GetCoreInstance())
			== Rml::Colourb(0x34, 0x56, 0x78), "removing experiment did not restore the authored rule");
	}
}

int main()
{
	try
	{
		juce::ScopedJuceInitialiser_GUI gui;
		testRendering();
		std::cout << "Panel rendering: toggles, density transitions, integer LCD, padded fallback, and removal passed\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
}
