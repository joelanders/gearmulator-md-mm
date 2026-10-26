#include "mdKeyBindings.h"

#include "juce_gui_basics/juce_gui_basics.h"

namespace mdJucePlugin
{
	const std::array<KeyboardMapping, g_keyboardMappingCount>& defaultKeyboardMappings()
	{
		static const std::array<KeyboardMapping, g_keyboardMappingCount> s_defaults =
		{{
			{ Rml::Input::KI_LEFT,   juce::KeyPress::leftKey,       md::PanelControl::Left        },
			{ Rml::Input::KI_RIGHT,  juce::KeyPress::rightKey,      md::PanelControl::Right       },
			{ Rml::Input::KI_UP,     juce::KeyPress::upKey,         md::PanelControl::Up          },
			{ Rml::Input::KI_DOWN,   juce::KeyPress::downKey,       md::PanelControl::Down        },
			{ Rml::Input::KI_RETURN, juce::KeyPress::returnKey,     md::PanelControl::Enter       },
			{ Rml::Input::KI_BACK,   juce::KeyPress::backspaceKey,  md::PanelControl::Exit        },
			{ Rml::Input::KI_1,      '1',                           md::PanelControl::Trigger1    },
			{ Rml::Input::KI_2,      '2',                           md::PanelControl::Trigger2    },
			{ Rml::Input::KI_3,      '3',                           md::PanelControl::Trigger3    },
			{ Rml::Input::KI_4,      '4',                           md::PanelControl::Trigger4    },
			{ Rml::Input::KI_5,      '5',                           md::PanelControl::Trigger5    },
			{ Rml::Input::KI_6,      '6',                           md::PanelControl::Trigger6    },
			{ Rml::Input::KI_7,      '7',                           md::PanelControl::Trigger7    },
			{ Rml::Input::KI_8,      '8',                           md::PanelControl::Trigger8    },
			{ Rml::Input::KI_Q,      'Q',                           md::PanelControl::Trigger9    },
			{ Rml::Input::KI_W,      'W',                           md::PanelControl::Trigger10   },
			{ Rml::Input::KI_E,      'E',                           md::PanelControl::Trigger11   },
			{ Rml::Input::KI_R,      'R',                           md::PanelControl::Trigger12   },
			{ Rml::Input::KI_T,      'T',                           md::PanelControl::Trigger13   },
			{ Rml::Input::KI_Y,      'Y',                           md::PanelControl::Trigger14   },
			{ Rml::Input::KI_U,      'U',                           md::PanelControl::Trigger15   },
			{ Rml::Input::KI_I,      'I',                           md::PanelControl::Trigger16   },
			{ Rml::Input::KI_TAB,    juce::KeyPress::tabKey,        md::PanelControl::Record      },
			{ Rml::Input::KI_O,      'O',                           md::PanelControl::Stop        },
			{ Rml::Input::KI_P,      'P',                           md::PanelControl::Play        },
			{ Rml::Input::KI_K,      'K',                           md::PanelControl::Kit         },
			{ Rml::Input::KI_F,      'F',                           md::PanelControl::PatternSong, md::PanelControl::SongEnable },
			{ Rml::Input::KI_A,      'A',                           md::PanelControl::BankA       },
			{ Rml::Input::KI_S,      'S',                           md::PanelControl::BankB       },
			{ Rml::Input::KI_D,      'D',                           md::PanelControl::BankC       },
			{ Rml::Input::KI_H,      'H',                           md::PanelControl::BankD       },
			{ Rml::Input::KI_G,      'G',                           md::PanelControl::BankGroup   },
			{ Rml::Input::KI_L,      'L',                           md::PanelControl::Scale       },
			{ Rml::Input::KI_J,      'J',                           md::PanelControl::ClassicExtended, md::PanelControl::TrigSelect },
			{ Rml::Input::KI_N,      'N',                           md::PanelControl::SynthesisEffectsRouting, md::PanelControl::DataPageForward },
			{ Rml::Input::KI_M,      'M',                           md::PanelControl::DataPageBackward, md::PanelControl::SynthesisEffectsRouting },
			{ Rml::Input::KI_B,      'B',                           md::PanelControl::Tempo       },
			{ Rml::Input::KI_Z,      'Z',                           md::PanelControl::Track1      },
			{ Rml::Input::KI_X,      'X',                           md::PanelControl::Track2      },
			{ Rml::Input::KI_C,      'C',                           md::PanelControl::Track3      },
			{ Rml::Input::KI_V,      'V',                           md::PanelControl::Track4      },
			{ Rml::Input::KI_9,      '9',                           md::PanelControl::Track5      },
			{ Rml::Input::KI_0,      '0',                           md::PanelControl::Track6      },
		}};
		return s_defaults;
	}

	Rml::Input::KeyIdentifier rmlKeyForJuceKey(const int _juceKey)
	{
		if(_juceKey == juce::KeyPress::leftKey)       return Rml::Input::KI_LEFT;
		if(_juceKey == juce::KeyPress::rightKey)      return Rml::Input::KI_RIGHT;
		if(_juceKey == juce::KeyPress::upKey)         return Rml::Input::KI_UP;
		if(_juceKey == juce::KeyPress::downKey)       return Rml::Input::KI_DOWN;
		if(_juceKey == juce::KeyPress::returnKey)     return Rml::Input::KI_RETURN;
		if(_juceKey == juce::KeyPress::backspaceKey)  return Rml::Input::KI_BACK;
		if(_juceKey == juce::KeyPress::tabKey)        return Rml::Input::KI_TAB;
		if(_juceKey == juce::KeyPress::escapeKey)     return Rml::Input::KI_ESCAPE;
		if(_juceKey == juce::KeyPress::deleteKey)     return Rml::Input::KI_DELETE;
		if(_juceKey == juce::KeyPress::homeKey)       return Rml::Input::KI_HOME;
		if(_juceKey == juce::KeyPress::endKey)        return Rml::Input::KI_END;
		if(_juceKey == juce::KeyPress::pageUpKey)     return Rml::Input::KI_PRIOR;
		if(_juceKey == juce::KeyPress::pageDownKey)   return Rml::Input::KI_NEXT;
		if(_juceKey == juce::KeyPress::spaceKey)      return Rml::Input::KI_SPACE;
		if(_juceKey == juce::KeyPress::F1Key)         return Rml::Input::KI_F1;
		if(_juceKey == juce::KeyPress::F2Key)         return Rml::Input::KI_F2;
		if(_juceKey == juce::KeyPress::F3Key)         return Rml::Input::KI_F3;
		if(_juceKey == juce::KeyPress::F4Key)         return Rml::Input::KI_F4;
		if(_juceKey == juce::KeyPress::F5Key)         return Rml::Input::KI_F5;
		if(_juceKey == juce::KeyPress::F6Key)         return Rml::Input::KI_F6;
		if(_juceKey == juce::KeyPress::F7Key)         return Rml::Input::KI_F7;
		if(_juceKey == juce::KeyPress::F8Key)         return Rml::Input::KI_F8;
		if(_juceKey == juce::KeyPress::F9Key)         return Rml::Input::KI_F9;
		if(_juceKey == juce::KeyPress::F10Key)        return Rml::Input::KI_F10;
		if(_juceKey == juce::KeyPress::F11Key)        return Rml::Input::KI_F11;
		if(_juceKey == juce::KeyPress::F12Key)        return Rml::Input::KI_F12;
		if(_juceKey == ',') return Rml::Input::KI_OEM_COMMA;
		if(_juceKey == '.') return Rml::Input::KI_OEM_PERIOD;
		if(_juceKey == '-') return Rml::Input::KI_OEM_MINUS;
		if(_juceKey == '=') return Rml::Input::KI_OEM_PLUS;
		if(_juceKey == '/') return Rml::Input::KI_OEM_2;
		if(_juceKey == '`') return Rml::Input::KI_OEM_3;

		if(_juceKey >= 'A' && _juceKey <= 'Z')
			return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_A + (_juceKey - 'A'));
		if(_juceKey >= 'a' && _juceKey <= 'z')
			return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_A + (_juceKey - 'a'));
		if(_juceKey >= '0' && _juceKey <= '9')
			return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_0 + (_juceKey - '0'));

		return Rml::Input::KI_UNKNOWN;
	}

	int juceKeyForRmlKey(const Rml::Input::KeyIdentifier _key)
	{
		switch(_key)
		{
		case Rml::Input::KI_LEFT:      return juce::KeyPress::leftKey;
		case Rml::Input::KI_RIGHT:     return juce::KeyPress::rightKey;
		case Rml::Input::KI_UP:        return juce::KeyPress::upKey;
		case Rml::Input::KI_DOWN:      return juce::KeyPress::downKey;
		case Rml::Input::KI_RETURN:    return juce::KeyPress::returnKey;
		case Rml::Input::KI_BACK:      return juce::KeyPress::backspaceKey;
		case Rml::Input::KI_TAB:       return juce::KeyPress::tabKey;
		case Rml::Input::KI_DELETE:    return juce::KeyPress::deleteKey;
		case Rml::Input::KI_HOME:      return juce::KeyPress::homeKey;
		case Rml::Input::KI_END:       return juce::KeyPress::endKey;
		case Rml::Input::KI_PRIOR:     return juce::KeyPress::pageUpKey;
		case Rml::Input::KI_NEXT:      return juce::KeyPress::pageDownKey;
		case Rml::Input::KI_SPACE:     return juce::KeyPress::spaceKey;
		case Rml::Input::KI_F1:        return juce::KeyPress::F1Key;
		case Rml::Input::KI_F2:        return juce::KeyPress::F2Key;
		case Rml::Input::KI_F3:        return juce::KeyPress::F3Key;
		case Rml::Input::KI_F4:        return juce::KeyPress::F4Key;
		case Rml::Input::KI_F5:        return juce::KeyPress::F5Key;
		case Rml::Input::KI_F6:        return juce::KeyPress::F6Key;
		case Rml::Input::KI_F7:        return juce::KeyPress::F7Key;
		case Rml::Input::KI_F8:        return juce::KeyPress::F8Key;
		case Rml::Input::KI_F9:        return juce::KeyPress::F9Key;
		case Rml::Input::KI_F10:       return juce::KeyPress::F10Key;
		case Rml::Input::KI_F11:       return juce::KeyPress::F11Key;
		case Rml::Input::KI_F12:        return juce::KeyPress::F12Key;
		case Rml::Input::KI_OEM_COMMA:  return ',';
		case Rml::Input::KI_OEM_PERIOD: return '.';
		case Rml::Input::KI_OEM_MINUS:  return '-';
		case Rml::Input::KI_OEM_PLUS:   return '=';
		case Rml::Input::KI_OEM_2:      return '/';
		case Rml::Input::KI_OEM_3:      return '`';
		default: break;
		}

		if(_key >= Rml::Input::KI_A && _key <= Rml::Input::KI_Z)
			return 'A' + (_key - Rml::Input::KI_A);

		if(_key >= Rml::Input::KI_0 && _key <= Rml::Input::KI_9)
			return '0' + (_key - Rml::Input::KI_0);

		return 0;
	}

	std::string keyDisplayName(const KeyboardMapping& _mapping)
	{
		const auto juceKey = _mapping.juceKeyCode;

		if(juceKey == 0)
			return "-";

		if(juceKey == juce::KeyPress::leftKey)       return "Left";
		if(juceKey == juce::KeyPress::rightKey)      return "Right";
		if(juceKey == juce::KeyPress::upKey)         return "Up";
		if(juceKey == juce::KeyPress::downKey)       return "Down";
		if(juceKey == juce::KeyPress::returnKey)     return "Enter";
		if(juceKey == juce::KeyPress::backspaceKey)  return "Backspace";
		if(juceKey == juce::KeyPress::tabKey)        return "Tab";
		if(juceKey == juce::KeyPress::escapeKey)     return "Escape";
		if(juceKey == juce::KeyPress::deleteKey)     return "Delete";
		if(juceKey == juce::KeyPress::homeKey)       return "Home";
		if(juceKey == juce::KeyPress::endKey)        return "End";
		if(juceKey == juce::KeyPress::pageUpKey)     return "PageUp";
		if(juceKey == juce::KeyPress::pageDownKey)   return "PageDown";
		if(juceKey == juce::KeyPress::spaceKey)      return "Space";
		if(juceKey == juce::KeyPress::F1Key)         return "F1";
		if(juceKey == juce::KeyPress::F2Key)         return "F2";
		if(juceKey == juce::KeyPress::F3Key)         return "F3";
		if(juceKey == juce::KeyPress::F4Key)         return "F4";
		if(juceKey == juce::KeyPress::F5Key)         return "F5";
		if(juceKey == juce::KeyPress::F6Key)         return "F6";
		if(juceKey == juce::KeyPress::F7Key)         return "F7";
		if(juceKey == juce::KeyPress::F8Key)         return "F8";
		if(juceKey == juce::KeyPress::F9Key)         return "F9";
		if(juceKey == juce::KeyPress::F10Key)        return "F10";
		if(juceKey == juce::KeyPress::F11Key)        return "F11";
		if(juceKey == juce::KeyPress::F12Key)        return "F12";

		if(juceKey >= 32 && juceKey <= 126)
			return std::string(1, static_cast<char>(juceKey));

		return "?";
	}
}
