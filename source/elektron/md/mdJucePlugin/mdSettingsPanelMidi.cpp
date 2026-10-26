#include "mdSettingsPanelMidi.h"

#include "mdEditor.h"
#include "mdPluginProcessor.h"

#include "jucePluginEditorLib/pluginProcessor.h"
#include "jucePluginEditorLib/settingsPlugin.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlElemComboBox.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"

#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/ID.h"

namespace mdJucePlugin
{
	namespace
	{
		using Target = panelMidi::Controller::LearnTarget;

		std::string encoderLabel(const md::PanelEncoder _encoder)
		{
			switch(_encoder)
			{
			case md::PanelEncoder::Level: return "Level";
			case md::PanelEncoder::SoundSelection: return "Sound selection";
			default: return std::string("Data entry ") + static_cast<char>('A' + static_cast<int>(_encoder));
			}
		}

		std::string controlLabel(const md::PanelControl _control)
		{
			using C = md::PanelControl;
			if(_control >= C::Trigger1 && _control <= C::Trigger16)
				return "Trig " + std::to_string(static_cast<int>(_control) - static_cast<int>(C::Trigger1) + 1);
			if(_control >= C::Track1 && _control <= C::Track6)
				return "Track " + std::to_string(static_cast<int>(_control) - static_cast<int>(C::Track1) + 1);

			switch(_control)
			{
			case C::Tempo: return "Tempo";
			case C::SynthesisEffectsRouting: return "Synth / FX / Routing";
			case C::Function: return "Function";
			case C::Kit: return "Kit";
			case C::Enter: return "Enter";
			case C::Exit: return "Exit";
			case C::Up: return "Up";
			case C::Down: return "Down";
			case C::Left: return "Left";
			case C::Right: return "Right";
			case C::BankGroup: return "Bank group";
			case C::BankA: return "Bank A";
			case C::BankB: return "Bank B";
			case C::BankC: return "Bank C";
			case C::BankD: return "Bank D";
			case C::Record: return "Record";
			case C::Play: return "Play";
			case C::Stop: return "Stop";
			case C::DataPageForward: return "Data page next";
			case C::DataPageBackward: return "Data page previous";
			case C::Scale: return "Scale";
			case C::PatternSong: return "Pattern / Song";
			case C::TrigSelect: return "Trig select";
			case C::SongEnable: return "Song enable";
			case C::ClassicExtended: return "Classic / Extended";
			default: return md::panelControlName(_control);
			}
		}

		Target encoderTarget(const md::PanelEncoder _encoder)
		{
			Target t;
			t.kind = Target::Kind::Encoder;
			t.encoder = _encoder;
			return t;
		}

		Target pushTarget(const md::PanelEncoder _encoder)
		{
			Target t;
			t.kind = Target::Kind::EncoderPush;
			t.encoder = _encoder;
			return t;
		}

		Target buttonTarget(const md::PanelControl _control)
		{
			Target t;
			t.kind = Target::Kind::Button;
			t.control = _control;
			return t;
		}

		bool sameTarget(const Target& _a, const Target& _b)
		{
			if(_a.kind != _b.kind)
				return false;
			return _a.kind == Target::Kind::Button ? _a.control == _b.control : _a.encoder == _b.encoder;
		}

		std::string labelOf(const Target& _target)
		{
			switch(_target.kind)
			{
			case Target::Kind::Encoder: return encoderLabel(_target.encoder);
			case Target::Kind::EncoderPush: return encoderLabel(_target.encoder) + " push";
			default: return controlLabel(_target.control);
			}
		}

		void beginLearnFor(panelMidi::Controller& _controller, const Target& _target)
		{
			switch(_target.kind)
			{
			case Target::Kind::Encoder: _controller.beginLearn(_target.encoder); break;
			case Target::Kind::EncoderPush: _controller.beginLearnPush(_target.encoder); break;
			default: _controller.beginLearn(_target.control); break;
			}
		}

		void clearBindingFor(panelMidi::Controller& _controller, const Target& _target)
		{
			switch(_target.kind)
			{
			case Target::Kind::Encoder: _controller.clear(_target.encoder); break;
			case Target::Kind::EncoderPush: _controller.clearPush(_target.encoder); break;
			default: _controller.clear(_target.control); break;
			}
		}

		const panelMidi::Source& sourceOf(const panelMidi::Table& _table, const Target& _target)
		{
			switch(_target.kind)
			{
			case Target::Kind::Encoder: return _table.encoders[static_cast<size_t>(_target.encoder)].source;
			case Target::Kind::EncoderPush: return _table.pushes[static_cast<size_t>(_target.encoder)];
			default: return _table.buttons[static_cast<size_t>(_target.control)];
			}
		}
	}

	SettingsPanelMidi::SettingsPanelMidi(Editor& _editor)
		: SettingsPlugin(_editor.getProcessor())
		, m_editor(_editor)
		, m_lifetime(_editor.getLifetimeToken())
	{
	}

	std::string SettingsPanelMidi::getTemplateName() const
	{
		return "tus_settings_panelmidi_" + m_editor.getSettingsTemplateSuffix();
	}

	void SettingsPanelMidi::createUi(Rml::Element* _root)
	{
		const auto model = m_editor.getPanelMidi().getModel();

		// One row per control this machine actually has.
		std::vector<Target> encoders;
		for(size_t i = 0; i < panelMidi::g_encoderCount; ++i)
		{
			const auto encoder = static_cast<md::PanelEncoder>(i);
			if(panelMidi::isAvailable(model, encoder))
				encoders.push_back(encoderTarget(encoder));
		}

		std::vector<Target> pushes;
		for(size_t i = 0; i < panelMidi::g_pushCount; ++i)
		{
			const auto encoder = static_cast<md::PanelEncoder>(i);
			if(panelMidi::isPushAvailable(model, encoder))
				pushes.push_back(pushTarget(encoder));
		}

		std::vector<Target> trigs;
		std::vector<Target> buttons;
		for(size_t i = 0; i < panelMidi::g_controlCount; ++i)
		{
			const auto control = static_cast<md::PanelControl>(i);
			if(!panelMidi::isAvailable(model, control))
				continue;
			const bool trig = control >= md::PanelControl::Trigger1 && control <= md::PanelControl::Trigger16;
			(trig ? trigs : buttons).push_back(buttonTarget(control));
		}

		createRows(_root, "encoderRow", encoders);
		createRows(_root, "pushRow", pushes);
		createRows(_root, "trigRow", trigs);
		createRows(_root, "buttonRow", buttons);

		createSection(_root, "btLearnAllEncoders", encoders);
		createSection(_root, "btLearnAllPushes", pushes);
		createSection(_root, "btLearnAllTrigs", trigs);
		createSection(_root, "btLearnAllButtons", buttons);

		m_portName = juceRmlUi::helper::findChild(_root, "panelMidiPortName", false);
		m_monitor = juceRmlUi::helper::findChild(_root, "panelMidiMonitor", false);
		m_channelWarning = juceRmlUi::helper::findChild(_root, "panelMidiChannelWarning", false);

		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btPanelMidiPort",
			m_editor.getProcessor().getConfig(), panelMidi::g_configKeyVirtualPort, [this](const bool _enabled)
			{
				if(m_lifetime.expired())
					return;
				m_editor.setPanelMidiPortEnabled(_enabled);
				m_shownPortName = "?";	// redraw the port line on the next tick
			}, true);

		m_channel = juceRmlUi::helper::findChildT<juceRmlUi::ElemComboBox>(_root, "panelMidiChannel", false);
		if(m_channel)
		{
			m_channel->addOption("Omni");
			for(int channel = 1; channel <= 16; ++channel)
				m_channel->addOption(std::to_string(channel));
			m_channel->onValueChanged.addListener([this](const float _value)
			{
				if(auto* const c = controller())
					c->setChannel(static_cast<uint8_t>(_value));
			});
		}

		if(auto* const reset = juceRmlUi::helper::findChild(_root, "btPanelMidiReset", false))
		{
			juceRmlUi::EventListener::Add(reset, Rml::EventId::Click, [this](Rml::Event& _event)
			{
				_event.StopPropagation();
				if(auto* const c = controller())
					c->resetToDefault();
			});
		}

		refresh();
		startTimerHz(20);	// redraws when the controller's revision changes
	}

	SettingsPanelMidi::~SettingsPanelMidi()
	{
		stopTimer();
		// Leaving the page must not leave the panel deaf to its controller.
		if(auto* const c = controller())
			c->cancelLearn();
	}

	panelMidi::Controller* SettingsPanelMidi::controller() const
	{
		return m_lifetime.expired() ? nullptr : &m_editor.getPanelMidi();
	}

	bool SettingsPanelMidi::isRunning(const Section& _section, const panelMidi::Controller& _controller) const
	{
		if(!_controller.isLearningSequence())
			return false;
		for(const auto& target : _section.targets)
			if(sameTarget(target, _controller.getLearnTarget()))
				return true;
		return false;
	}

	void SettingsPanelMidi::createSection(Rml::Element* const _root, const char* const _buttonId,
		const std::vector<panelMidi::Controller::LearnTarget>& _targets)
	{
		auto* const button = juceRmlUi::helper::findChild(_root, _buttonId, false);
		if(!button || _targets.empty())
			return;

		const auto index = m_sections.size();
		m_sections.push_back({ button, _targets });

		juceRmlUi::EventListener::Add(button, Rml::EventId::Click, [this, index](Rml::Event& _event)
		{
			_event.StopPropagation();
			auto* const c = controller();
			if(!c)
				return;
			const auto& section = m_sections[index];
			if(isRunning(section, *c))
				c->cancelLearn();
			else
				c->beginLearnSequence(section.targets);
		});
	}

	void SettingsPanelMidi::createRows(Rml::Element* _root, const char* const _rowId,
		const std::vector<panelMidi::Controller::LearnTarget>& _targets)
	{
		auto* const templateRow = juceRmlUi::helper::findChild(_root, _rowId, false);
		if(!templateRow)
			return;

		auto* const body = templateRow->GetParentNode();
		const auto rowTemplate = body->RemoveChild(templateRow);

		for(const auto& target : _targets)
		{
			auto* const row = body->AppendChild(rowTemplate->Clone());

			Row entry;
			entry.target = target;
			entry.source = juceRmlUi::helper::findChild(row, "source", false);
			entry.learn = juceRmlUi::helper::findChild(row, "btLearn", false);
			entry.mode = juceRmlUi::helper::findChildT<juceRmlUi::ElemComboBox>(row, "modeCombo", false);

			if(auto* const name = juceRmlUi::helper::findChild(row, "name", false))
				name->SetInnerRML(labelOf(target));

			if(entry.learn)
			{
				juceRmlUi::EventListener::Add(entry.learn, Rml::EventId::Click, [this, target](Rml::Event& _event)
				{
					_event.StopPropagation();
					auto* const c = controller();
					if(!c)
						return;
					if(sameTarget(c->getLearnTarget(), target))
						c->cancelLearn();
					else
						beginLearnFor(*c, target);
				});
			}

			if(auto* const clear = juceRmlUi::helper::findChild(row, "btClear", false))
			{
				juceRmlUi::EventListener::Add(clear, Rml::EventId::Click, [this, target](Rml::Event& _event)
				{
					_event.StopPropagation();
					auto* const c = controller();
					if(!c)
						return;
					clearBindingFor(*c, target);
				});
			}

			if(entry.mode)
			{
				for(uint8_t i = 0; i < panelMidi::g_encoderModeCount; ++i)
					entry.mode->addOption(panelMidi::describe(static_cast<panelMidi::EncoderMode>(i)));
				entry.mode->onValueChanged.addListener([this, target](const float _value)
				{
					if(auto* const c = controller())
						c->setEncoderMode(target.encoder, static_cast<panelMidi::EncoderMode>(static_cast<int>(_value)));
				});
			}

			m_rows.push_back(entry);
		}
	}

	void SettingsPanelMidi::refresh()
	{
		auto* const c = controller();
		if(!c)
			return;

		m_revision = c->getRevision();
		const auto& table = c->getTable();
		const auto& learning = c->getLearnTarget();

		for(const auto& row : m_rows)
		{
			const bool waiting = sameTarget(learning, row.target);

			if(row.source)
			{
				if(waiting)
					row.source->SetInnerRML("waiting...");
				else
					row.source->SetInnerRML(panelMidi::describe(sourceOf(table, row.target)));
			}

			if(row.learn)
				row.learn->SetInnerRML(waiting ? "Cancel" : "Learn");

			if(row.mode && row.target.kind == Target::Kind::Encoder)
				row.mode->setSelectedIndex(static_cast<size_t>(table.encoders[static_cast<size_t>(row.target.encoder)].mode), false);
		}

		if(m_channel)
			m_channel->setSelectedIndex(table.channel, false);

		for(const auto& section : m_sections)
		{
			if(!section.button)
				continue;
			section.button->SetInnerRML(isRunning(section, *c)
				? "Stop (" + std::to_string(c->getSequenceIndex() + 1) + " of " + std::to_string(c->getSequenceSize()) + ")"
				: std::string("Learn all in order"));
		}
	}

	void SettingsPanelMidi::timerCallback()
	{
		auto* const c = controller();
		if(!c)
			return;

		if(c->getRevision() != m_revision)
			refresh();

		if(m_monitor && c->getMessageSerial() != m_messageSerial)
		{
			m_messageSerial = c->getMessageSerial();
			auto text = "Last received: " + panelMidi::describe(c->getLastMessage());
			if(c->wasLastMessageFiltered())
				text += " - ignored, the channel filter is " + std::to_string(c->getTable().channel);
			m_monitor->SetInnerRML(text);
		}

		if(m_channelWarning)
		{
			// Depends on the channel filter and on the machine's base channel, which the
			// machine reports some time after start, so it is checked on every tick.
			const auto warning = panelMidi::channelOverlapWarning(c->getModel(), c->getTable().channel,
				m_editor.getMachineBaseChannel()).value_or(std::string());
			if(warning != m_shownChannelWarning)
			{
				m_shownChannelWarning = warning;
				m_channelWarning->SetInnerRML(warning);
				m_channelWarning->SetProperty(Rml::PropertyId::Display,
					warning.empty() ? Rml::Style::Display::None : Rml::Style::Display::Block);
			}
		}

		if(m_portName)
		{
			const auto port = m_editor.getPanelMidiPortName();
			if(port != m_shownPortName)
			{
				m_shownPortName = port;
				m_portName->SetInnerRML(port.empty() ? "Port: not created" : "Port: " + port);
			}
		}
	}
}
