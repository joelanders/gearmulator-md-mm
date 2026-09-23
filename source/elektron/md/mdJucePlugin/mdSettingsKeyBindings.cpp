#include "mdSettingsKeyBindings.h"

#include "mdEditor.h"
#include "mdKeyBindings.h"

#include "mdLib/mdpanel.h"

#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"

#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/ElementDocument.h"

namespace mdJucePlugin
{
	namespace
	{
		bool isReservedKey(const Rml::Input::KeyIdentifier _key)
		{
			switch(_key)
			{
			case Rml::Input::KI_ESCAPE:
			case Rml::Input::KI_LSHIFT:
			case Rml::Input::KI_RSHIFT:
			case Rml::Input::KI_LCONTROL:
			case Rml::Input::KI_RCONTROL:
			case Rml::Input::KI_LMETA:
			case Rml::Input::KI_RMETA:
			case Rml::Input::KI_LMENU:
			case Rml::Input::KI_RMENU:
			case Rml::Input::KI_UNKNOWN:
				return true;
			default:
				return false;
			}
		}
	}

	SettingsKeyBindings::SettingsKeyBindings(Editor& _editor, Rml::Element* _root)
		: m_editor(_editor)
	{
		m_listContainer = juceRmlUi::helper::findChild(_root, "keybindList", false);

		if(m_listContainer)
		{
			m_rowTemplate = juceRmlUi::helper::findChild(m_listContainer, "keybindRowTemplate", false);
			buildRows();
		}

		m_resetStatusElem = juceRmlUi::helper::findChild(_root, "resetStatus", false);

		if(auto* const resetBtn = juceRmlUi::helper::findChild(_root, "btResetKeybinds", false))
		{
			juceRmlUi::EventListener::AddClick(resetBtn, [this]
			{
				resetToDefaults();
			});
		}

		if(auto* const document = _root->GetOwnerDocument())
		{
			m_keydownListener.add(document, Rml::EventId::Keydown,
				[this](Rml::Event& _event)
				{
					if(onKeyDown(_event))
						_event.StopPropagation();
				}, true);
		}
	}

	bool SettingsKeyBindings::onKeyDown(Rml::Event& _event)
	{
		if(m_capturingIndex < 0)
			return false;

		const auto key = juceRmlUi::helper::getKeyIdentifier(_event);

		if(key == Rml::Input::KI_ESCAPE)
		{
			exitCaptureMode();
			return true;
		}

		if(isReservedKey(key))
			return true;

		const int juceKey = juceKeyForRmlKey(key);
		if(juceKey == 0)
			return true;

		applyCapture(key, juceKey);
		return true;
	}

	void SettingsKeyBindings::buildRows()
	{
		if(!m_listContainer || !m_rowTemplate)
			return;

		for(auto* row : m_rows)
		{
			if(row && row->GetParentNode())
				row->GetParentNode()->RemoveChild(row);
		}
		m_rows.clear();

		const auto& mappings = m_editor.getKeyboardMappings();
		m_rows.resize(mappings.size(), nullptr);

		const auto model = m_editor.getModel();
		for(size_t i = 0; i < mappings.size(); ++i)
		{
			const auto& m = mappings[i];

			const bool primaryValid = md::panelPacket(model, m.control).has_value();
			const bool altValid = m.altControl && md::panelPacket(model, *m.altControl).has_value();
			if(!primaryValid && !altValid)
			{
				m_rows[i] = nullptr;
				continue;
			}

			auto* row = m_listContainer->AppendChild(m_rowTemplate->Clone());
			row->RemoveProperty("display");
			m_rows[i] = row;

			if(auto* labelElem = juceRmlUi::helper::findChild(row, "keybindLabel", false))
				labelElem->SetInnerRML(md::panelControlName(m.control));

			if(auto* keyElem = juceRmlUi::helper::findChild(row, "keybindKey", false))
				keyElem->SetInnerRML(keyDisplayName(m));

			if(auto* btn = juceRmlUi::helper::findChild(row, "keybindBtn", false))
			{
				juceRmlUi::EventListener::AddClick(btn, [this, i]
				{
					enterCaptureMode(i);
				});
			}
		}
	}

	void SettingsKeyBindings::updateRowDisplay(const size_t _index)
	{
		if(_index >= m_rows.size() || !m_rows[_index])
			return;

		auto* const row = m_rows[_index];
		if(auto* const keyElem = juceRmlUi::helper::findChild(row, "keybindKey", false))
		{
			const auto& mappings = m_editor.getKeyboardMappings();
			keyElem->SetInnerRML(keyDisplayName(mappings[_index]));
		}
	}

	void SettingsKeyBindings::enterCaptureMode(const size_t _index)
	{
		if(m_capturingIndex >= 0)
			exitCaptureMode();

		m_capturingIndex = static_cast<int>(_index);

		if(auto* const row = m_rows[_index])
		{
			row->SetClass("keybind-row--capturing", true);
			if(auto* const keyElem = juceRmlUi::helper::findChild(row, "keybindKey", false))
				keyElem->SetInnerRML("Press a key...");
		}
	}

	void SettingsKeyBindings::exitCaptureMode()
	{
		if(m_capturingIndex < 0)
			return;

		const auto index = static_cast<size_t>(m_capturingIndex);
		if(auto* const row = m_rows[index])
			row->SetClass("keybind-row--capturing", false);

		updateRowDisplay(index);
		m_capturingIndex = -1;
	}

	void SettingsKeyBindings::applyCapture(
		const Rml::Input::KeyIdentifier _rmlKey, const int _juceKey)
	{
		const auto index = static_cast<size_t>(m_capturingIndex);
		const auto& mappings = m_editor.getKeyboardMappings();

		for(size_t i = 0; i < mappings.size(); ++i)
		{
			if(i == index)
				continue;
			if(mappings[i].key == _rmlKey)
			{
				KeyboardMapping cleared = mappings[i];
				cleared.key = Rml::Input::KI_UNKNOWN;
				cleared.juceKeyCode = 0;
				m_editor.setKeyboardMapping(i, cleared);
				updateRowDisplay(i);
				break;
			}
		}

		KeyboardMapping updated = mappings[index];
		updated.key = _rmlKey;
		updated.juceKeyCode = _juceKey;
		m_editor.setKeyboardMapping(index, updated);

		exitCaptureMode();
	}

	void SettingsKeyBindings::resetToDefaults()
	{
		m_editor.resetKeyboardMappings();
		buildRows();
		if(m_resetStatusElem)
			m_resetStatusElem->SetInnerRML("Defaults restored.");
	}
}
