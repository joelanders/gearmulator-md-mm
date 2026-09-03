#include "mmMobilePanel.h"

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

		bool ledIsLit(const md::FrontPanel& _frontPanel, const uint8_t _bank,
			const uint8_t _bit)
		{
			return (_frontPanel.getLedBankRaw(_bank)
				& static_cast<uint8_t>(1u << _bit)) == 0;
		}

		class PanelButton final : public juce::Component
		{
		public:
			PanelButton(juce::String _primary, juce::String _secondary,
				const md::PanelControl _control,
				std::function<void(md::PanelControl, bool)> _callback,
				const Arrow _arrow = Arrow::None,
				const bool _latchable = false)
				: m_primary(std::move(_primary))
				, m_secondary(std::move(_secondary))
				, m_control(_control)
				, m_callback(std::move(_callback))
				, m_arrow(_arrow)
				, m_latchable(_latchable)
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
					const auto hasLabel = m_primary.isNotEmpty();
					const auto side = std::min(bounds.getWidth(), bounds.getHeight())
						* (hasLabel ? 0.12f : 0.20f);
					const auto centre = hasLabel
						? juce::Point<float>(bounds.getRight() - 10.0f, bounds.getCentreY())
						: bounds.getCentre().toFloat();
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
					if(hasLabel)
					{
						_g.setFont(juce::Font(std::clamp(bounds.getHeight() * 0.29f, 10.0f, 14.0f)));
						_g.drawFittedText(m_primary, bounds.withTrimmedRight(16).reduced(2),
							juce::Justification::centred, 1);
					}
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

			void mouseDown(const juce::MouseEvent& _event) override
			{
				if(m_latchable && _event.getNumberOfClicks() > 1
					&& (_event.getNumberOfClicks() % 2) == 0)
				{
					m_latched = !m_latched;
					setPressed(m_latched);
					return;
				}

				if(!m_pressed)
					setPressed(true);
			}

			void mouseUp(const juce::MouseEvent&) override
			{
				if(m_pressed && !m_latched)
					setPressed(false);
			}

		private:
			void setPressed(const bool _pressed)
			{
				if(m_pressed == _pressed)
					return;
				m_pressed = _pressed;
				if(m_callback)
					m_callback(m_control, _pressed);
				repaint();
			}

			juce::String m_primary;
			juce::String m_secondary;
			md::PanelControl m_control;
			std::function<void(md::PanelControl, bool)> m_callback;
			Arrow m_arrow = Arrow::None;
			bool m_latchable = false;
			bool m_latched = false;
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
				m_lastEventTimeMs = _event.eventTime.toMilliseconds();
				m_dragRemainder = 0.0f;
			}

			void mouseDrag(const juce::MouseEvent& _event) override
			{
				const auto delta = (m_lastPosition.y - _event.position.y)
					+ (_event.position.x - m_lastPosition.x);
				m_lastPosition = _event.position;

				const auto eventTimeMs = _event.eventTime.toMilliseconds();
				const auto elapsedMs = std::max<juce::int64>(1, eventTimeMs - m_lastEventTimeMs);
				m_lastEventTimeMs = eventTimeMs;
				const auto speed = std::abs(delta) * 1000.0f / static_cast<float>(elapsedMs);
				const auto acceleration = 1.0f
					+ std::clamp((speed - 220.0f) / 320.0f, 0.0f, 2.5f);
				m_dragRemainder += delta * acceleration / 6.0f;
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
			juce::int64 m_lastEventTimeMs = 0;
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
				if(!m_paint)
					return;

				const auto bounds = getLocalBounds();
				const auto integerScale = std::min(bounds.getWidth() / 128, bounds.getHeight() / 64);
				if(integerScale > 0)
				{
					const auto target = juce::Rectangle<int>(128 * integerScale, 64 * integerScale)
						.withCentre(bounds.getCentre());
					m_paint(_g, target);
					return;
				}

				m_paint(_g, bounds.withSizeKeepingCentre(bounds.getWidth(), bounds.getWidth() / 2));
			}

		private:
			std::function<void(juce::Graphics&, juce::Rectangle<int>)> m_paint;
		};
	}

	class MmMobilePanel::DisplaySection final : public juce::Component
	{
	public:
		explicit DisplaySection(std::function<void(juce::Graphics&, juce::Rectangle<int>)> _paint)
			: m_lcd(std::move(_paint))
		{
			addAndMakeVisible(m_lcd);
			configureLabel(m_page, juce::Justification::centredLeft);
			configureLabel(m_patternPage, juce::Justification::centred);
			configureLabel(m_trigMode, juce::Justification::centred);
			configureLabel(m_bankGroup, juce::Justification::centredRight);
			m_page.setText("Page: Synthesis", juce::dontSendNotification);
			m_patternPage.setText("Pattern Page: 1", juce::dontSendNotification);
			m_trigMode.setText("Trig Sel: Amp", juce::dontSendNotification);
			m_bankGroup.setText("Bank Group: A-D", juce::dontSendNotification);
		}

		void refresh(const md::FrontPanel& _frontPanel, const bool _lcdChanged)
		{
			constexpr const char* pageNames[] =
			{
				"Synthesis", "Amplification", "Filter", "Effects", "LFO 1", "LFO 2", "LFO 3"
			};
			constexpr uint8_t pageBanks[] = { 0x25, 0x25, 0x25, 0x25, 0x26, 0x26, 0x26 };
			constexpr uint8_t pageBits[] = { 4, 5, 6, 7, 0, 1, 2 };
			for(size_t i = 0; i < std::size(pageNames); ++i)
			{
				if(ledIsLit(_frontPanel, pageBanks[i], pageBits[i]))
				{
					m_page.setText("Page: " + juce::String(pageNames[i]), juce::dontSendNotification);
					break;
				}
			}

			for(uint8_t page = 0; page < 4; ++page)
			{
				if(ledIsLit(_frontPanel, 0x27, static_cast<uint8_t>(4 + page)))
				{
					m_patternPage.setText("Pattern Page: " + juce::String(page + 1),
						juce::dontSendNotification);
					break;
				}
			}

			const auto amp = ledIsLit(_frontPanel, 0x27, 1);
			const auto filter = ledIsLit(_frontPanel, 0x27, 2);
			const auto lfo = ledIsLit(_frontPanel, 0x27, 3);
			juce::String trigMode;
			if(amp && filter && lfo)
				trigMode = "All";
			else if(filter)
				trigMode = "Filter";
			else if(lfo)
				trigMode = "LFO";
			else
				trigMode = "Amp";
			m_trigMode.setText("Trig Sel: " + trigMode, juce::dontSendNotification);

			if(ledIsLit(_frontPanel, 0x26, 4))
				m_bankGroup.setText("Bank Group: E-H", juce::dontSendNotification);
			else if(ledIsLit(_frontPanel, 0x26, 3))
				m_bankGroup.setText("Bank Group: A-D", juce::dontSendNotification);

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
			labels.items.add(juce::FlexItem(m_page).withFlex(1.12f));
			labels.items.add(juce::FlexItem(m_patternPage).withFlex(1.0f));
			labels.items.add(juce::FlexItem(m_trigMode).withFlex(0.9f));
			labels.items.add(juce::FlexItem(m_bankGroup).withFlex(1.08f));
			column.items.add(juce::FlexItem(labels).withHeight(getHeight() * 28.0f / 173.0f));
			column.performLayout(getLocalBounds());
		}

	private:
		void configureLabel(juce::Label& _label, const juce::Justification _justification)
		{
			addAndMakeVisible(_label);
			_label.setColour(juce::Label::textColourId, g_metaText);
			_label.setFont(juce::Font(12.0f));
			_label.setJustificationType(_justification);
			_label.setMinimumHorizontalScale(0.72f);
			_label.setInterceptsMouseClicks(false, false);
		}

		LcdView m_lcd;
		juce::Label m_page;
		juce::Label m_patternPage;
		juce::Label m_trigMode;
		juce::Label m_bankGroup;
	};

	class MmMobilePanel::ButtonRow final : public juce::Component
	{
	public:
		PanelButton* addButton(const juce::String& _primary, const juce::String& _secondary,
			const md::PanelControl _control,
			const std::function<void(md::PanelControl, bool)>& _callback,
			const Arrow _arrow = Arrow::None,
			const bool _latchable = false,
			const float _flex = 1.0f)
		{
			auto button = std::make_unique<PanelButton>(_primary, _secondary, _control,
				_callback, _arrow, _latchable);
			auto* const result = button.get();
			addAndMakeVisible(*button);
			m_buttons.push_back({ std::move(button), _flex });
			return result;
		}

		PanelButton* addActionButton(const juce::String& _primary,
			const std::function<void()>& _action)
		{
			return addButton(_primary, {}, md::PanelControl::DataPageForward,
				[_action](md::PanelControl, const bool _pressed)
				{
					if(_pressed && _action)
						_action();
				});
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
				row.items.add(juce::FlexItem(*m_buttons[i].component).withFlex(m_buttons[i].flex)
					.withMargin({ 0.0f, right, 0.0f, left }));
			}
			row.performLayout(getLocalBounds());
		}

	private:
		struct Item
		{
			std::unique_ptr<PanelButton> component;
			float flex = 1.0f;
		};
		std::vector<Item> m_buttons;
	};

	class MmMobilePanel::KnobGrid final : public juce::Component
	{
	public:
		explicit KnobGrid(const std::function<void(md::PanelEncoder, int)>& _callback)
		{
			const std::array<std::pair<const char*, md::PanelEncoder>, 9> controls
			{{
				{ "Level", md::PanelEncoder::Level },
				{ "A", md::PanelEncoder::DataEntryA },
				{ "B", md::PanelEncoder::DataEntryB },
				{ "C", md::PanelEncoder::DataEntryC },
				{ "D", md::PanelEncoder::DataEntryD },
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
			addAndMakeVisible(m_spacer);
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

			for(size_t i = 0; i < 5; ++i)
				grid.items.add(juce::GridItem(*m_knobs[i]).withSize(diameter, diameter));
			grid.items.add(juce::GridItem(m_spacer).withSize(diameter, diameter));
			for(size_t i = 5; i < m_knobs.size(); ++i)
				grid.items.add(juce::GridItem(*m_knobs[i]).withSize(diameter, diameter));
			grid.performLayout(getLocalBounds());
		}

	private:
		juce::Component m_spacer;
		std::vector<std::unique_ptr<EncoderControl>> m_knobs;
	};

	class MmMobilePanel::StepGrid final : public juce::Component
	{
	public:
		explicit StepGrid(const std::function<void(md::PanelControl, bool)>& _callback)
		{
			for(size_t i = 0; i < m_steps.size(); ++i)
			{
				auto step = std::make_unique<PanelButton>(juce::String{}, juce::String{},
					static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Trigger1)
						+ static_cast<int>(i)), _callback, Arrow::None, true);
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

	MmMobilePanel::MmMobilePanel(Callbacks _callbacks)
		: m_callbacks(std::move(_callbacks))
		, m_displaySection(std::make_unique<DisplaySection>(m_callbacks.paintLcd))
		, m_trackButtons(std::make_unique<ButtonRow>())
		, m_editPageButtons(std::make_unique<ButtonRow>())
		, m_setupButtons(std::make_unique<ButtonRow>())
		, m_navigationButtons(std::make_unique<ButtonRow>())
		, m_bankButtons(std::make_unique<ButtonRow>())
		, m_transportButtons(std::make_unique<ButtonRow>())
		, m_knobGrid(std::make_unique<KnobGrid>(m_callbacks.turnEncoder))
		, m_stepGrid(std::make_unique<StepGrid>(m_callbacks.setControlPressed))
	{
		setOpaque(true);
		for(auto* component : { static_cast<juce::Component*>(m_displaySection.get()),
			static_cast<juce::Component*>(m_trackButtons.get()),
			static_cast<juce::Component*>(m_editPageButtons.get()),
			static_cast<juce::Component*>(m_setupButtons.get()),
			static_cast<juce::Component*>(m_navigationButtons.get()),
			static_cast<juce::Component*>(m_bankButtons.get()),
			static_cast<juce::Component*>(m_transportButtons.get()),
			static_cast<juce::Component*>(m_knobGrid.get()),
			static_cast<juce::Component*>(m_stepGrid.get()) })
			addAndMakeVisible(*component);

		for(size_t i = 0; i < m_trackButtonIndicators.size(); ++i)
		{
			m_trackButtonIndicators[i] = m_trackButtons->addButton(juce::String(i + 1), {},
				static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Track1)
					+ static_cast<int>(i)), m_callbacks.setControlPressed);
		}

		constexpr const char* editLabels[] = { "Syn", "Amp", "Filter", "FX", "LFO 1", "LFO 2", "LFO 3" };
		for(size_t i = 0; i < m_editPageIndicators.size(); ++i)
		{
			m_editPageIndicators[i] = m_editPageButtons->addActionButton(editLabels[i],
				[this, i]
				{
					if(m_callbacks.selectDataPage)
						m_callbacks.selectDataPage(static_cast<int>(i));
				});
		}

		m_setupButtons->addButton("Global", {}, md::PanelControl::Kit, m_callbacks.setControlPressed);
		m_patternSongIndicator = m_setupButtons->addButton("Poly", "Pat / Song",
			md::PanelControl::SongEnable, m_callbacks.setControlPressed);
		m_setupButtons->addButton("Pat Page", "Scale", md::PanelControl::Scale,
			m_callbacks.setControlPressed);
		m_setupButtons->addButton("Page", {}, md::PanelControl::DataPageBackward,
			m_callbacks.setControlPressed, Arrow::Up);
		m_setupButtons->addButton("Page", {}, md::PanelControl::DataPageForward,
			m_callbacks.setControlPressed, Arrow::Down);

		m_navigationButtons->addButton("Yes", {}, md::PanelControl::Enter, m_callbacks.setControlPressed);
		m_navigationButtons->addButton("No", {}, md::PanelControl::Exit, m_callbacks.setControlPressed);
		m_navigationButtons->addButton({}, {}, md::PanelControl::Left, m_callbacks.setControlPressed, Arrow::Left);
		m_navigationButtons->addButton({}, {}, md::PanelControl::Up, m_callbacks.setControlPressed, Arrow::Up);
		m_navigationButtons->addButton({}, {}, md::PanelControl::Down, m_callbacks.setControlPressed, Arrow::Down);
		m_navigationButtons->addButton({}, {}, md::PanelControl::Right, m_callbacks.setControlPressed, Arrow::Right);

		m_bankButtons->addButton("Bank", "Mute Mode", md::PanelControl::BankGroup,
			m_callbacks.setControlPressed);
		m_bankButtons->addButton("A / E", "Arp", md::PanelControl::BankA, m_callbacks.setControlPressed);
		m_bankButtons->addButton("B / F", "Trans", md::PanelControl::BankB, m_callbacks.setControlPressed);
		m_bankButtons->addButton("C / G", "Swing", md::PanelControl::BankC, m_callbacks.setControlPressed);
		m_bankButtons->addButton("D / H", "Slide", md::PanelControl::BankD, m_callbacks.setControlPressed);

		m_transportButtons->addButton("Function", {}, md::PanelControl::Function,
			m_callbacks.setControlPressed, Arrow::None, true);
		m_transportButtons->addButton("Trig Select", "Midi Seq", md::PanelControl::TrigSelect,
			m_callbacks.setControlPressed);
		m_recordIndicator = m_transportButtons->addButton("Rec", "Copy", md::PanelControl::Record,
			m_callbacks.setControlPressed);
		m_transportButtons->addButton("Play", "Clear", md::PanelControl::Play,
			m_callbacks.setControlPressed);
		m_transportButtons->addButton("Stop", "Paste", md::PanelControl::Stop,
			m_callbacks.setControlPressed);
	}

	MmMobilePanel::~MmMobilePanel() = default;

	void MmMobilePanel::refresh(const md::FrontPanel& _frontPanel, const bool _lcdChanged)
	{
		m_displaySection->refresh(_frontPanel, _lcdChanged);
		m_stepGrid->refresh(_frontPanel);

		constexpr uint8_t trackBanks[] = { 0x25, 0x25, 0x24, 0x24, 0x24, 0x24 };
		constexpr uint8_t trackGreenBits[] = { 0, 2, 0, 2, 4, 6 };
		constexpr uint8_t trackRedBits[] = { 1, 3, 1, 3, 5, 7 };
		for(size_t i = 0; i < m_trackButtonIndicators.size(); ++i)
		{
			const auto indicated = ledIsLit(_frontPanel, trackBanks[i], trackGreenBits[i])
				|| ledIsLit(_frontPanel, trackBanks[i], trackRedBits[i]);
			static_cast<PanelButton*>(m_trackButtonIndicators[i])->setIndicated(indicated);
		}

		constexpr uint8_t pageBanks[] = { 0x25, 0x25, 0x25, 0x25, 0x26, 0x26, 0x26 };
		constexpr uint8_t pageBits[] = { 4, 5, 6, 7, 0, 1, 2 };
		for(size_t i = 0; i < m_editPageIndicators.size(); ++i)
			static_cast<PanelButton*>(m_editPageIndicators[i])->setIndicated(
				ledIsLit(_frontPanel, pageBanks[i], pageBits[i]));

		static_cast<PanelButton*>(m_patternSongIndicator)->setIndicated(
			ledIsLit(_frontPanel, 0x26, 5) || ledIsLit(_frontPanel, 0x26, 6));
		static_cast<PanelButton*>(m_recordIndicator)->setIndicated(
			ledIsLit(_frontPanel, 0x27, 0));
	}

	void MmMobilePanel::paint(juce::Graphics& _graphics)
	{
		_graphics.fillAll(g_background);
	}

	void MmMobilePanel::resized()
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
		column.items.add(juce::FlexItem(*m_displaySection).withHeight(173.0f * scale));

		const auto addSection = [&column, scale](juce::Component& _component, const float _height)
		{
			column.items.add(juce::FlexItem(1.0f, 18.0f * scale));
			column.items.add(juce::FlexItem(_component).withHeight(_height * scale));
		};
		addSection(*m_trackButtons, 41.0f);
		addSection(*m_editPageButtons, 41.0f);
		addSection(*m_setupButtons, 41.0f);
		addSection(*m_navigationButtons, 41.0f);
		addSection(*m_bankButtons, 41.0f);
		addSection(*m_transportButtons, 41.0f);
		addSection(*m_knobGrid, 117.0f);
		addSection(*m_stepGrid, 92.0f);
		column.items.add(juce::FlexItem().withFlex(1.0f));
		column.performLayout(panel);
	}

	void MmMobilePanel::parentSizeChanged()
	{
		if(const auto* const parent = getParentComponent())
			setBounds(parent->getLocalBounds());
	}
}
