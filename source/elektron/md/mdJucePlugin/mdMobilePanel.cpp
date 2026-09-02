#include "mdMobilePanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace mdJucePlugin
{
	namespace
	{
		const juce::Colour g_background = juce::Colour::fromRGB(0, 0, 0);
		const juce::Colour g_dark = juce::Colour::fromRGB(48, 48, 48);
		const juce::Colour g_light = juce::Colour::fromRGB(128, 128, 128);
		const juce::Colour g_text = juce::Colour::fromRGB(255, 255, 255);
		const juce::Colour g_secondaryText = juce::Colour::fromRGBA(255, 255, 255, 153);
		const juce::Colour g_metaText = juce::Colour::fromRGB(128, 128, 128);

		enum class Arrow
		{
			None,
			Left,
			Up,
			Down,
			Right
		};

		class PanelButton final : public juce::Component
		{
		public:
			PanelButton(juce::String _primary, juce::String _secondary,
				const md::PanelControl _control,
				std::function<void(md::PanelControl, bool)> _callback,
				const Arrow _arrow = Arrow::None)
				: m_primary(std::move(_primary))
				, m_secondary(std::move(_secondary))
				, m_control(_control)
				, m_callback(std::move(_callback))
				, m_arrow(_arrow)
			{
				setRepaintsOnMouseActivity(true);
			}

			~PanelButton() override
			{
				if(m_pressed && m_callback)
					m_callback(m_control, false);
			}

			void setIndicated(const bool _indicated)
			{
				if(m_indicated == _indicated)
					return;
				m_indicated = _indicated;
				repaint();
			}

			void paint(juce::Graphics& _g) override
			{
				_g.fillAll((m_pressed || m_indicated) ? g_light : g_dark);

				auto bounds = getLocalBounds();
				if(m_arrow != Arrow::None)
				{
					const auto side = std::min(bounds.getWidth(), bounds.getHeight()) * 0.20f;
					const auto centre = bounds.getCentre().toFloat();
					juce::Path arrow;
					switch(m_arrow)
					{
					case Arrow::Left:
						arrow.addTriangle(centre.x - side, centre.y,
							centre.x + side, centre.y - side, centre.x + side, centre.y + side);
						break;
					case Arrow::Up:
						arrow.addTriangle(centre.x, centre.y - side,
							centre.x - side, centre.y + side, centre.x + side, centre.y + side);
						break;
					case Arrow::Down:
						arrow.addTriangle(centre.x, centre.y + side,
							centre.x - side, centre.y - side, centre.x + side, centre.y - side);
						break;
					case Arrow::Right:
						arrow.addTriangle(centre.x + side, centre.y,
							centre.x - side, centre.y - side, centre.x - side, centre.y + side);
						break;
					case Arrow::None:
						break;
					}
					_g.setColour(g_text);
					_g.fillPath(arrow);
					return;
				}

				const auto fontHeight = std::clamp(bounds.getHeight() * 0.29f, 10.0f, 14.0f);
				_g.setFont(juce::Font(fontHeight));
				if(m_secondary.isEmpty())
				{
					_g.setColour(g_text);
					_g.drawFittedText(m_primary, bounds.reduced(3), juce::Justification::centred, 1);
					return;
				}

				auto upper = bounds.removeFromTop(bounds.getHeight() / 2 + 2);
				_g.setColour(g_text);
				_g.drawFittedText(m_primary, upper.withTrimmedTop(2), juce::Justification::centredBottom, 1);
				_g.setColour(g_secondaryText);
				_g.drawFittedText(m_secondary, bounds.withTrimmedBottom(2), juce::Justification::centredTop, 1);
			}

			void mouseDown(const juce::MouseEvent&) override
			{
				if(m_pressed)
					return;
				m_pressed = true;
				if(m_callback)
					m_callback(m_control, true);
				repaint();
			}

			void mouseUp(const juce::MouseEvent&) override
			{
				if(!m_pressed)
					return;
				m_pressed = false;
				if(m_callback)
					m_callback(m_control, false);
				repaint();
			}

		private:
			juce::String m_primary;
			juce::String m_secondary;
			md::PanelControl m_control;
			std::function<void(md::PanelControl, bool)> m_callback;
			Arrow m_arrow = Arrow::None;
			bool m_pressed = false;
			bool m_indicated = false;
		};

		class EncoderControl final : public juce::Component
		{
		public:
			EncoderControl(juce::String _label, const md::PanelEncoder _encoder,
				std::function<void(md::PanelEncoder, int)> _callback)
				: m_label(std::move(_label)), m_encoder(_encoder), m_callback(std::move(_callback))
			{
			}

			void paint(juce::Graphics& _g) override
			{
				const auto circle = getLocalBounds().toFloat().reduced(0.5f);
				_g.setColour(g_text);
				_g.fillEllipse(circle);
				_g.setColour(juce::Colours::black);
				_g.setFont(juce::Font(std::clamp(getHeight() * 0.24f, 10.0f, 13.0f)));
				_g.drawFittedText(m_label, getLocalBounds().reduced(2), juce::Justification::centred, 1);
			}

			void mouseDown(const juce::MouseEvent& _event) override
			{
				m_lastPosition = _event.position;
				m_dragRemainder = 0.0f;
			}

			void mouseDrag(const juce::MouseEvent& _event) override
			{
				const auto delta = (m_lastPosition.y - _event.position.y)
					+ (_event.position.x - m_lastPosition.x);
				m_lastPosition = _event.position;
				m_dragRemainder += delta / 7.0f;
				const auto steps = static_cast<int>(m_dragRemainder);
				if(steps == 0)
					return;
				m_dragRemainder -= static_cast<float>(steps);
				if(m_callback)
					m_callback(m_encoder, steps);
			}

			void mouseWheelMove(const juce::MouseEvent&,
				const juce::MouseWheelDetails& _wheel) override
			{
				if(m_callback && _wheel.deltaY != 0.0f)
					m_callback(m_encoder, _wheel.deltaY > 0.0f ? 1 : -1);
			}

		private:
			juce::String m_label;
			md::PanelEncoder m_encoder;
			std::function<void(md::PanelEncoder, int)> m_callback;
			juce::Point<float> m_lastPosition;
			float m_dragRemainder = 0.0f;
		};

		class LcdView final : public juce::Component
		{
		public:
			explicit LcdView(std::function<void(juce::Graphics&, juce::Rectangle<int>)> _paint)
				: m_paint(std::move(_paint))
			{
				setOpaque(true);
			}

			void paint(juce::Graphics& _g) override
			{
				_g.fillAll(g_background);
				if(m_paint)
					m_paint(_g, getLocalBounds());
			}

		private:
			std::function<void(juce::Graphics&, juce::Rectangle<int>)> m_paint;
		};
	}

	class MobilePanel::DisplaySection final : public juce::Component
	{
	public:
		explicit DisplaySection(std::function<void(juce::Graphics&, juce::Rectangle<int>)> _paint)
			: m_lcd(std::move(_paint))
		{
			addAndMakeVisible(m_lcd);
			configureLabel(m_page, juce::Justification::centredLeft);
			configureLabel(m_patternPage, juce::Justification::centred);
			configureLabel(m_bankGroup, juce::Justification::centredRight);
			m_page.setText("Page: Synthesis", juce::dontSendNotification);
			m_patternPage.setText("Pattern Page: 1", juce::dontSendNotification);
			m_bankGroup.setText("Bank Group: A-D", juce::dontSendNotification);
		}

		void refresh(const md::FrontPanel& _frontPanel, const bool _lcdChanged)
		{
			juce::String page("Page: ");
			if(_frontPanel.getStatusLed(md::FrontPanel::StatusLed::Effects))
				page += "Effects";
			else if(_frontPanel.getStatusLed(md::FrontPanel::StatusLed::Routing))
				page += "Routing";
			else
				page += "Synthesis";
			m_page.setText(page, juce::dontSendNotification);

			const auto eh = _frontPanel.getModeLed(md::FrontPanel::ModeLed::BankGroupEH);
			m_bankGroup.setText(eh ? "Bank Group: E-H" : "Bank Group: A-D",
				juce::dontSendNotification);
			if(_lcdChanged)
				m_lcd.repaint();
		}

		void resized() override
		{
			juce::FlexBox column;
			column.flexDirection = juce::FlexBox::Direction::column;
			column.items.add(juce::FlexItem(m_lcd).withFlex(1.0f));

			juce::FlexBox labels;
			labels.flexDirection = juce::FlexBox::Direction::row;
			labels.items.add(juce::FlexItem(m_page).withFlex(1.0f));
			labels.items.add(juce::FlexItem(m_patternPage).withFlex(1.0f));
			labels.items.add(juce::FlexItem(m_bankGroup).withFlex(1.0f));
			// 384 x 192 at the 402 x 874 reference size: the 128 x 64 LCD is
			// enlarged by exactly 3x in logical coordinates (9x on a 3x iPhone).
			column.items.add(juce::FlexItem(labels).withHeight(getHeight() * 29.0f / 221.0f));
			column.performLayout(getLocalBounds());
		}

	private:
		void configureLabel(juce::Label& _label, const juce::Justification _justification)
		{
			addAndMakeVisible(_label);
			_label.setColour(juce::Label::textColourId, g_metaText);
			_label.setFont(juce::Font(12.0f));
			_label.setJustificationType(_justification);
			_label.setInterceptsMouseClicks(false, false);
		}

		LcdView m_lcd;
		juce::Label m_page;
		juce::Label m_patternPage;
		juce::Label m_bankGroup;
	};

	class MobilePanel::ButtonRow final : public juce::Component
	{
	public:
		PanelButton* addButton(const juce::String& _primary, const juce::String& _secondary,
			const md::PanelControl _control,
			const std::function<void(md::PanelControl, bool)>& _callback,
			const Arrow _arrow = Arrow::None)
		{
			auto button = std::make_unique<PanelButton>(_primary, _secondary, _control, _callback, _arrow);
			auto* const result = button.get();
			addAndMakeVisible(*button);
			m_buttons.push_back(std::move(button));
			return result;
		}

		void resized() override
		{
			juce::FlexBox row;
			row.flexDirection = juce::FlexBox::Direction::row;
			row.alignItems = juce::FlexBox::AlignItems::stretch;
			const auto gap = std::max(6.0f, getHeight() * 0.22f);
			for(size_t i = 0; i < m_buttons.size(); ++i)
			{
				const auto left = i == 0 ? 0.0f : gap * 0.5f;
				const auto right = i + 1 == m_buttons.size() ? 0.0f : gap * 0.5f;
				row.items.add(juce::FlexItem(*m_buttons[i]).withFlex(1.0f)
					.withMargin({ 0.0f, right, 0.0f, left }));
			}
			row.performLayout(getLocalBounds());
		}

	private:
		std::vector<std::unique_ptr<PanelButton>> m_buttons;
	};

	class MobilePanel::KnobGrid final : public juce::Component
	{
	public:
		explicit KnobGrid(const std::function<void(md::PanelEncoder, int)>& _callback)
		{
			const std::array<std::pair<const char*, md::PanelEncoder>, 10> controls
			{{
				{ "Level", md::PanelEncoder::Level },
				{ "A", md::PanelEncoder::DataEntryA },
				{ "B", md::PanelEncoder::DataEntryB },
				{ "C", md::PanelEncoder::DataEntryC },
				{ "D", md::PanelEncoder::DataEntryD },
				{ "Data", md::PanelEncoder::SoundSelection },
				{ "E", md::PanelEncoder::DataEntryE },
				{ "F", md::PanelEncoder::DataEntryF },
				{ "G", md::PanelEncoder::DataEntryG },
				{ "H", md::PanelEncoder::DataEntryH },
			}};

			for(const auto& control : controls)
			{
				auto knob = std::make_unique<EncoderControl>(control.first, control.second, _callback);
				addAndMakeVisible(*knob);
				m_knobs.push_back(std::move(knob));
			}
		}

		void resized() override
		{
			juce::Grid grid;
			for(int i = 0; i < 5; ++i)
				grid.templateColumns.add(juce::Grid::TrackInfo(juce::Grid::Fr(1)));
			grid.templateRows.add(juce::Grid::TrackInfo(juce::Grid::Fr(1)));
			grid.templateRows.add(juce::Grid::TrackInfo(juce::Grid::Fr(1)));
			grid.columnGap = juce::Grid::Px(std::max(6.0f, getWidth() * 0.025f));
			grid.rowGap = juce::Grid::Px(std::max(10.0f, getHeight() * 0.12f));
			grid.justifyItems = juce::Grid::JustifyItems::center;
			grid.alignItems = juce::Grid::AlignItems::center;
			const auto diameter = std::min(51.0f * getWidth() / 384.0f,
				(static_cast<float>(getHeight()) - static_cast<float>(grid.rowGap.pixels)) * 0.5f);
			for(auto& knob : m_knobs)
				grid.items.add(juce::GridItem(*knob).withSize(diameter, diameter));
			grid.performLayout(getLocalBounds());
		}

	private:
		std::vector<std::unique_ptr<EncoderControl>> m_knobs;
	};

	class MobilePanel::StepGrid final : public juce::Component
	{
	public:
		explicit StepGrid(const std::function<void(md::PanelControl, bool)>& _callback)
		{
			for(size_t i = 0; i < m_steps.size(); ++i)
			{
				auto step = std::make_unique<PanelButton>(juce::String{}, juce::String{},
					static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Trigger1)
						+ static_cast<int>(i)), _callback);
				m_steps[i] = step.get();
				addAndMakeVisible(*step);
				m_ownedSteps.push_back(std::move(step));
			}
		}

		void refresh(const md::FrontPanel& _frontPanel)
		{
			for(size_t i = 0; i < m_steps.size(); ++i)
				m_steps[i]->setIndicated(_frontPanel.getStepLed(static_cast<uint32_t>(i)));
		}

		void resized() override
		{
			juce::Grid grid;
			for(int i = 0; i < 8; ++i)
				grid.templateColumns.add(juce::Grid::TrackInfo(juce::Grid::Fr(1)));
			grid.templateRows.add(juce::Grid::TrackInfo(juce::Grid::Fr(1)));
			grid.templateRows.add(juce::Grid::TrackInfo(juce::Grid::Fr(1)));
			grid.columnGap = juce::Grid::Px(std::max(7.0f, getWidth() * 0.025f));
			grid.rowGap = juce::Grid::Px(std::max(10.0f, getHeight() * 0.17f));
			for(auto& step : m_ownedSteps)
				grid.items.add(juce::GridItem(*step));
			grid.performLayout(getLocalBounds());
		}

	private:
		std::array<PanelButton*, 16> m_steps{};
		std::vector<std::unique_ptr<PanelButton>> m_ownedSteps;
	};

	MobilePanel::MobilePanel(Callbacks _callbacks)
		: m_callbacks(std::move(_callbacks))
		, m_displaySection(std::make_unique<DisplaySection>(m_callbacks.paintLcd))
		, m_topButtons(std::make_unique<ButtonRow>())
		, m_navigationButtons(std::make_unique<ButtonRow>())
		, m_bankButtons(std::make_unique<ButtonRow>())
		, m_transportButtons(std::make_unique<ButtonRow>())
		, m_knobGrid(std::make_unique<KnobGrid>(m_callbacks.turnEncoder))
		, m_stepGrid(std::make_unique<StepGrid>(m_callbacks.setControlPressed))
	{
		setOpaque(true);
		for(auto* component : { static_cast<juce::Component*>(m_displaySection.get()),
			static_cast<juce::Component*>(m_topButtons.get()),
			static_cast<juce::Component*>(m_navigationButtons.get()),
			static_cast<juce::Component*>(m_bankButtons.get()),
			static_cast<juce::Component*>(m_transportButtons.get()),
			static_cast<juce::Component*>(m_knobGrid.get()),
			static_cast<juce::Component*>(m_stepGrid.get()) })
			addAndMakeVisible(*component);

		m_topButtons->addButton("Tempo", {}, md::PanelControl::Tempo, m_callbacks.setControlPressed);
		m_classicButton = m_topButtons->addButton("Classic", {}, md::PanelControl::ClassicExtended,
			m_callbacks.setControlPressed);
		m_topButtons->addButton("Pat Page", "Scale", md::PanelControl::Scale, m_callbacks.setControlPressed);
		m_topButtons->addButton("Pat / Song", "Global", md::PanelControl::PatternSong, m_callbacks.setControlPressed);
		m_topButtons->addButton("Kit", "Song", md::PanelControl::Kit, m_callbacks.setControlPressed);

		m_navigationButtons->addButton("Yes", {}, md::PanelControl::Enter, m_callbacks.setControlPressed);
		m_navigationButtons->addButton("No", {}, md::PanelControl::Exit, m_callbacks.setControlPressed);
		m_navigationButtons->addButton({}, {}, md::PanelControl::Left, m_callbacks.setControlPressed, Arrow::Left);
		m_navigationButtons->addButton({}, {}, md::PanelControl::Up, m_callbacks.setControlPressed, Arrow::Up);
		m_navigationButtons->addButton({}, {}, md::PanelControl::Down, m_callbacks.setControlPressed, Arrow::Down);
		m_navigationButtons->addButton({}, {}, md::PanelControl::Right, m_callbacks.setControlPressed, Arrow::Right);

		m_bankButtons->addButton("Bank Grp", {}, md::PanelControl::BankGroup, m_callbacks.setControlPressed);
		m_bankButtons->addButton("A / E", "Mute", md::PanelControl::BankA, m_callbacks.setControlPressed);
		m_bankButtons->addButton("B / F", "Accent", md::PanelControl::BankB, m_callbacks.setControlPressed);
		m_bankButtons->addButton("C / G", "Swing", md::PanelControl::BankC, m_callbacks.setControlPressed);
		m_bankButtons->addButton("D / H", "Slide", md::PanelControl::BankD, m_callbacks.setControlPressed);

		m_transportButtons->addButton("Function", {}, md::PanelControl::Function, m_callbacks.setControlPressed);
		m_transportButtons->addButton("Page", "LFO", md::PanelControl::SynthesisEffectsRouting,
			m_callbacks.setControlPressed);
		m_recordButton = m_transportButtons->addButton("Rec", "Copy", md::PanelControl::Record,
			m_callbacks.setControlPressed);
		m_transportButtons->addButton("Play", "Clear", md::PanelControl::Play, m_callbacks.setControlPressed);
		m_transportButtons->addButton("Stop", "Paste", md::PanelControl::Stop, m_callbacks.setControlPressed);
	}

	MobilePanel::~MobilePanel() = default;

	void MobilePanel::refresh(const md::FrontPanel& _frontPanel, const bool _lcdChanged)
	{
		m_displaySection->refresh(_frontPanel, _lcdChanged);
		m_stepGrid->refresh(_frontPanel);
		static_cast<PanelButton*>(m_classicButton)->setIndicated(
			_frontPanel.getModeLed(md::FrontPanel::ModeLed::Classic));
		static_cast<PanelButton*>(m_recordButton)->setIndicated(
			_frontPanel.getModeLed(md::FrontPanel::ModeLed::Record));
	}

	void MobilePanel::paint(juce::Graphics& _graphics)
	{
		_graphics.fillAll(g_background);
	}

	void MobilePanel::resized()
	{
		constexpr float designWidth = 402.0f;
		constexpr float designHeight = 874.0f;
		const auto scale = std::min(getWidth() / designWidth, getHeight() / designHeight);
		const auto panelWidth = juce::roundToInt(designWidth * scale);
		const auto panelHeight = juce::roundToInt(designHeight * scale);
		auto panel = juce::Rectangle<int>(panelWidth, panelHeight).withCentre(getLocalBounds().getCentre());
		panel.reduce(juce::roundToInt(9.0f * scale), 0);

		juce::FlexBox column;
		column.flexDirection = juce::FlexBox::Direction::column;
		column.items.add(juce::FlexItem(panel.getWidth(), 55.0f * scale));
		column.items.add(juce::FlexItem(*m_displaySection).withHeight(221.0f * scale));
		column.items.add(juce::FlexItem(panel.getWidth(), 18.0f * scale));
		column.items.add(juce::FlexItem().withFlex(1.0f));

		const auto addSection = [&column, scale](juce::Component& _component, const float _height)
		{
			column.items.add(juce::FlexItem(_component).withHeight(_height * scale));
			column.items.add(juce::FlexItem(1.0f, 18.0f * scale));
		};
		addSection(*m_topButtons, 41.0f);
		addSection(*m_navigationButtons, 41.0f);
		addSection(*m_bankButtons, 41.0f);
		addSection(*m_transportButtons, 41.0f);
		addSection(*m_knobGrid, 117.0f);
		column.items.add(juce::FlexItem(*m_stepGrid).withHeight(92.0f * scale));
		column.items.add(juce::FlexItem(panel.getWidth(), 40.0f * scale));
		column.performLayout(panel);
	}

	void MobilePanel::parentSizeChanged()
	{
		if(const auto* const parent = getParentComponent())
			setBounds(parent->getLocalBounds());
	}
}
