#include "mdEditor.h"

#include "mdController.h"
#include "mdPanelAffordances.h"
#include "mdPluginProcessor.h"
#include "mdSettingsPanelFeel.h"

#include "jucePluginEditorLib/pluginProcessor.h"
#include "jucePluginEditorLib/fileChooserFlow.h"

#include "juceUiLib/messageBox.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdmidiprotocol.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"

#include "synthLib/plugin.h"

#include "baseLib/filesystem.h"

#include "juceRmlUi/rmlElemCanvas.h"
#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlElemComboBox.h"
#include "juceRmlUi/rmlElemKnob.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/juceRmlComponent.h"

#include "RmlUi/Core/Element.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace mdJucePlugin
{
	namespace
	{
		// The two machines use the same framebuffer geometry but different physical
		// displays, both positive: lit red/orange backlight with dark pixels on the
		// Machinedrum, pale green-grey with dark pixels on the Monomachine. "Off" is
		// therefore the bright backlight and "on" the dark set pixel.
		constexpr uint32_t g_mdLcdOff = 0xffe0472b;
		constexpr uint32_t g_mdLcdOn  = 0xff38100a;
		constexpr uint32_t g_mmLcdOff = 0xffb9c8b2;
		constexpr uint32_t g_mmLcdOn  = 0xff1a2b1e;

		// A skin button bound to a logical control. The packet is selected using the
		// actual device model when the editor is created.
		struct PanelButton
		{
			const char* id;
			md::PanelControl control;
		};

		bool lcdChanged(const md::FrontPanel& _a, const md::FrontPanel& _b)
		{
			for(uint32_t half = 0; half < 2; ++half)
			{
				for(uint32_t page = 0; page < 8; ++page)
				{
					for(uint32_t column = 0; column < 64; ++column)
					{
						if(_a.getLcdVram(half, page, column) != _b.getLcdVram(half, page, column))
							return true;
					}
				}
			}
			return false;
		}

		constexpr bool isPatternBank(const md::PanelControl _control)
		{
			return _control == md::PanelControl::BankA || _control == md::PanelControl::BankB
				|| _control == md::PanelControl::BankC || _control == md::PanelControl::BankD;
		}

		constexpr bool isTrigger(const md::PanelControl _control)
		{
			return _control >= md::PanelControl::Trigger1 && _control <= md::PanelControl::Trigger16;
		}

		// Arbitrary endless-knob value range; only per-move deltas are used.
		constexpr float g_encoderRange = 100.0f;
		constexpr int g_encoderBurstCap = 8;	// max ±1 events emitted per Change

		constexpr PanelButton g_panelButtons[] =
		{
			{ "trigKey0", md::PanelControl::Trigger1 }, { "trigKey1", md::PanelControl::Trigger2 },
			{ "trigKey2", md::PanelControl::Trigger3 }, { "trigKey3", md::PanelControl::Trigger4 },
			{ "trigKey4", md::PanelControl::Trigger5 }, { "trigKey5", md::PanelControl::Trigger6 },
			{ "trigKey6", md::PanelControl::Trigger7 }, { "trigKey7", md::PanelControl::Trigger8 },
			{ "trigKey8", md::PanelControl::Trigger9 }, { "trigKey9", md::PanelControl::Trigger10 },
			{ "trigKey10", md::PanelControl::Trigger11 }, { "trigKey11", md::PanelControl::Trigger12 },
			{ "trigKey12", md::PanelControl::Trigger13 }, { "trigKey13", md::PanelControl::Trigger14 },
			{ "trigKey14", md::PanelControl::Trigger15 }, { "trigKey15", md::PanelControl::Trigger16 },
			{ "btTempo", md::PanelControl::Tempo },
			{ "btRec", md::PanelControl::Record },
			{ "btPlay", md::PanelControl::Play },
			{ "btStop", md::PanelControl::Stop },
			{ "btSynth", md::PanelControl::SynthesisEffectsRouting },
			{ "btPattern", md::PanelControl::PatternSong },
			{ "btKit", md::PanelControl::Kit },
			{ "btScale", md::PanelControl::Scale },
			{ "btExit", md::PanelControl::Exit },
			{ "btLeft", md::PanelControl::Left },
			{ "btDown", md::PanelControl::Down },
			{ "btRight", md::PanelControl::Right },
			{ "btClassic", md::PanelControl::ClassicExtended },
			{ "btFunction", md::PanelControl::Function },
			{ "btBankGrp", md::PanelControl::BankGroup },
			{ "btEnter", md::PanelControl::Enter },
			{ "btUp", md::PanelControl::Up },
			{ "btTrigSelect", md::PanelControl::TrigSelect },
			{ "btSongEnable", md::PanelControl::SongEnable },
			{ "btDataNext", md::PanelControl::DataPageForward },
			{ "btDataPrev", md::PanelControl::DataPageBackward },
			{ "btBankA", md::PanelControl::BankA },
			{ "btBankB", md::PanelControl::BankB },
			{ "btBankC", md::PanelControl::BankC },
			{ "btBankD", md::PanelControl::BankD },
			{ "btTrack1", md::PanelControl::Track1 },
			{ "btTrack2", md::PanelControl::Track2 },
			{ "btTrack3", md::PanelControl::Track3 },
			{ "btTrack4", md::PanelControl::Track4 },
			{ "btTrack5", md::PanelControl::Track5 },
			{ "btTrack6", md::PanelControl::Track6 },
		};
	}

	Editor::Editor(jucePluginEditorLib::Processor& _processor, const jucePluginEditorLib::Skin& _skin)
		: jucePluginEditorLib::Editor(_processor, _skin)
		, m_controller(dynamic_cast<Controller&>(_processor.getController()))
		, m_model(dynamic_cast<const AudioPluginAudioProcessor&>(_processor).getModel())
	{
	}

	Editor::~Editor()
	{
		m_panelSteps.clear();
		endPanelGesture();
		releasePatternBankLatch();
		releaseAllPanelInputs();
		stopTimer();
	}

	md::Hardware* Editor::getHardware() const
	{
		auto* device = dynamic_cast<md::Device*>(getProcessor().getPlugin().getDevice());
		return device ? &device->getHardware() : nullptr;
	}

	bool Editor::refreshFrontPanelSnapshot()
	{
		auto* device = dynamic_cast<md::Device*>(getProcessor().getPlugin().getDevice());
		if(!device)
			return false;

		auto snapshot = device->getFrontPanelSnapshot();
		m_lcdChanged = !m_frontPanelSnapshotValid || lcdChanged(m_frontPanelSnapshot, snapshot);
		m_frontPanelSnapshot = std::move(snapshot);
		return true;
	}

	md::MachineModel Editor::getModel() const
	{
		return m_model;
	}

	void Editor::create()
	{
		jucePluginEditorLib::Editor::create();

		if(auto* romSelector = findChild<juceRmlUi::ElemComboBox>("RomSelector", false))
		{
			const auto rom = md::RomLoader::findROM(getModel());

			if(rom.isValid())
				romSelector->addOption(baseLib::filesystem::getFilenameWithoutPath(rom.getFilename()));
			else
				romSelector->addOption("<No ROM found>");

			romSelector->setValue(0);
			romSelector->SetProperty(Rml::PropertyId::PointerEvents, Rml::Style::PointerEvents::None);
		}

		createLcd();
		createButtons();
		createEncoders();
		createMasterVolume();
		applyPanelSpeeds();
		createLeds();
		createPanelAffordances();
	}

	void Editor::createLcd()
	{
		auto* lcdArea = findChild("lcdArea", false);

		if(!lcdArea)
			return;

		m_lcdCanvas = juceRmlUi::ElemCanvas::create(lcdArea);
		m_lcdCanvas->setClearEveryFrame(true);
		m_lcdCanvas->setRepaintGraphicsCallback([this](const juce::Image& _image, juce::Graphics& _g)
		{
			paintLcd(_image, _g);
		});
		m_lcdCanvas->repaint();

		// ~30 Hz refresh of the live framebuffer (the firmware runs on the MCU thread).
		startTimerHz(30);
	}

	void Editor::createButtons()
	{
		releasePatternBankLatch();
		m_panelRows.reset();
		const auto model = getModel();

		for (const auto& pb : g_panelButtons)
		{
			auto* b = findChild<juceRmlUi::ElemButton>(pb.id, false);
			if(!b)
				continue;

			const auto packet = md::panelPacket(model, pb.control);
			if(!packet)
			{
				b->SetProperty(Rml::PropertyId::PointerEvents, Rml::Style::PointerEvents::None);
				continue;
			}

			// On the hardware, A/E through D/H are held while a trig key chooses the
			// pattern number. A mouse cannot hold one momentary button while clicking
			// another, so an MM bank click latches until the next trig.
			if(model == md::MachineModel::Monomachine && isPatternBank(pb.control))
			{
				juceRmlUi::EventListener::Add(b, Rml::EventId::Click, [this, b, packet](Rml::Event&)
				{
					togglePatternBankLatch(b, *packet);
				});
				continue;
			}

			juceRmlUi::EventListener::Add(b, Rml::EventId::Mousedown,
				[this, b, packet, model, control = pb.control](Rml::Event&)
			{
				if(model == md::MachineModel::Monomachine && !isTrigger(control))
					releasePatternBankLatch();
				juceRmlUi::ElemButton::setChecked(b, true);
				if(auto* hw = getHardware())
				{
					const auto combined = m_panelRows.press(*packet);
					hw->sendPanelEvent(combined.row, combined.mask);
				}
			});

			// Mouseout releases too, otherwise dragging off a button leaves it held.
			const auto release = [this, b, packet, model, control = pb.control](Rml::Event&)
			{
				if(!b->isChecked())
					return;
				juceRmlUi::ElemButton::setChecked(b, false);
				if(auto* hw = getHardware())
				{
					const auto combined = m_panelRows.release(*packet);
					hw->sendPanelEvent(combined.row, combined.mask);
				}
				if(model == md::MachineModel::Monomachine && isTrigger(control))
					releasePatternBankLatch();
			};
			juceRmlUi::EventListener::Add(b, Rml::EventId::Mouseup, release);
			juceRmlUi::EventListener::Add(b, Rml::EventId::Mouseout, release);
		}
	}

	void Editor::createPanelAffordances()
	{
		const auto bindChordList = [this](const auto& _shortcuts)
		{
			for(const auto& shortcut : _shortcuts)
				bindPanelChord(shortcut.id, shortcut.control);
		};

		const auto bindPage = [this](const char* const _id, const auto _select)
		{
			auto* element = findChild(_id, false);
			if(!element)
				return;
			element->SetClass(panelAffordances::g_affordanceClass, true);
			juceRmlUi::EventListener::Add(element, Rml::EventId::Click, [_select](Rml::Event&)
			{
				_select();
			});
		};

		if(getModel() == md::MachineModel::Machinedrum)
		{
			for(int track = 0; track < 16; ++track)
			{
				const auto id = std::to_string(track);
				bindPage((panelAffordances::g_drumLedPrefix + id).c_str(),
					[this, track] { selectMachinedrumTrack(track); });
				bindPage((panelAffordances::g_trackLabelPrefix + id).c_str(),
					[this, track] { selectMachinedrumTrack(track); });
			}

			bindChordList(panelAffordances::g_machinedrumShortcuts);

			for(size_t page = 0; page < panelAffordances::g_machinedrumDataPages.size(); ++page)
				bindPage(panelAffordances::g_machinedrumDataPages[page],
					[this, page] { selectMachinedrumDataPage(static_cast<int>(page)); });
			return;
		}

		for(int track = 0; track < 6; ++track)
		{
			const auto control = static_cast<md::PanelControl>(
				static_cast<int>(md::PanelControl::Track1) + track);
			const auto id = std::to_string(track);
			bindPanelTarget((panelAffordances::g_drumLedPrefix + id).c_str(), control);
			bindPanelTarget((panelAffordances::g_trackLabelPrefix + id).c_str(), control);
			bindPanelChord((panelAffordances::g_trackMutePrefix + id).c_str(), control);
		}

		bindChordList(panelAffordances::g_monomachineShortcuts);

		for(size_t page = 0; page < panelAffordances::g_monomachineDataPages.size(); ++page)
			bindPage(panelAffordances::g_monomachineDataPages[page],
				[this, page] { selectMonomachineDataPage(static_cast<int>(page)); });

		for(size_t mode = 0; mode < panelAffordances::g_monomachineTrigModes.size(); ++mode)
			bindPage(panelAffordances::g_monomachineTrigModes[mode],
				[this, mode] { selectMonomachineTrigMode(static_cast<int>(mode)); });
	}

	void Editor::bindPanelTarget(const char* const _id, const md::PanelControl _control)
	{
		auto* element = findChild(_id, false);
		if(!element)
			return;

		element->SetClass(panelAffordances::g_affordanceClass, true);
		juceRmlUi::EventListener::Add(element, Rml::EventId::Mousedown,
			[this, element, _control](Rml::Event&)
		{
			beginPanelGesture(element, { _control });
		});

		const auto release = [this](Rml::Event&) { endPanelGesture(); };
		juceRmlUi::EventListener::Add(element, Rml::EventId::Mouseup, release);
		juceRmlUi::EventListener::Add(element, Rml::EventId::Mouseout, release);
	}

	void Editor::bindPanelChord(const char* const _id, const md::PanelControl _control)
	{
		auto* element = findChild(_id, false);
		if(!element)
			return;

		element->SetClass(panelAffordances::g_affordanceClass, true);
		juceRmlUi::EventListener::Add(element, Rml::EventId::Mousedown,
			[this, element, _control](Rml::Event&)
		{
			beginPanelGesture(element, { md::PanelControl::Function, _control });
		});

		const auto release = [this](Rml::Event&) { endPanelGesture(); };
		juceRmlUi::EventListener::Add(element, Rml::EventId::Mouseup, release);
		juceRmlUi::EventListener::Add(element, Rml::EventId::Mouseout, release);
	}

	void Editor::beginPanelGesture(Rml::Element* const _element,
		const std::initializer_list<md::PanelControl> _controls)
	{
		endPanelGesture();
		releasePatternBankLatch();

		m_panelGestureElement = _element;
		m_panelGestureElement->SetClass("active", true);

		for(const auto control : _controls)
		{
			const auto packet = md::panelPacket(getModel(), control);
			if(!packet)
				continue;

			m_panelGesturePackets.push_back(*packet);
			if(auto* hw = getHardware())
			{
				const auto combined = m_panelRows.press(*packet);
				hw->sendPanelEvent(combined.row, combined.mask);
			}
		}
	}

	void Editor::endPanelGesture()
	{
		if(m_panelGestureElement)
			m_panelGestureElement->SetClass("active", false);

		// Release in reverse order so a chord lets go of the target before FUNCTION.
		for(auto it = m_panelGesturePackets.rbegin(); it != m_panelGesturePackets.rend(); ++it)
		{
			const auto combined = m_panelRows.release(*it);
			if(auto* hw = getHardware())
				hw->sendPanelEvent(combined.row, combined.mask);
		}

		m_panelGesturePackets.clear();
		m_panelGestureElement = nullptr;
	}

	void Editor::releaseAllPanelInputs()
	{
		if(auto* hw = getHardware())
		{
			for(uint8_t row = 0x20; row <= 0x25; ++row)
				if(m_panelRows.mask(row) != 0)
					hw->sendPanelEvent(row, 0);
		}
		m_panelRows.reset();
	}

	void Editor::queuePanelPulse(const md::PanelControl _control, const int _count)
	{
		if(_count <= 0)
			return;

		const auto packet = md::panelPacket(getModel(), _control);
		if(!packet)
			return;

		releasePatternBankLatch();

		for(int i = 0; i < _count; ++i)
		{
			m_panelSteps.push_back({ *packet, true });
			m_panelSteps.push_back({ *packet, false });
		}
	}

	// One step per timer tick, so the firmware sees distinct press and release edges.
	void Editor::servicePanelQueue()
	{
		if(m_panelSteps.empty())
		{
			servicePanelNavigation();
			return;
		}

		const auto step = m_panelSteps.front();
		m_panelSteps.pop_front();

		const auto combined = step.press ? m_panelRows.press(step.packet) : m_panelRows.release(step.packet);
		if(auto* hw = getHardware())
			hw->sendPanelEvent(combined.row, combined.mask);

		// Give the firmware a complete timer interval to update its LED readback
		// before deciding whether the pending direct-selection target needs another
		// pulse. This also makes a rapid replacement request use observed state.
		if(m_panelSteps.empty() && !step.press)
			m_panelSettleTicks = 1;
	}

	void Editor::servicePanelNavigation()
	{
		if(!m_panelSteps.empty())
			return;

		if(m_panelSettleTicks > 0)
		{
			--m_panelSettleTicks;
			return;
		}

		if(!m_frontPanelSnapshotValid && !refreshFrontPanelSnapshot())
			return;
		m_frontPanelSnapshotValid = true;
		const auto& frontPanel = m_frontPanelSnapshot;

		if(getModel() == md::MachineModel::Machinedrum)
		{
			const auto target = m_machinedrumDataPageTarget.target();
			if(!target)
				return;

			constexpr md::FrontPanel::StatusLed pages[] =
			{
				md::FrontPanel::StatusLed::Synthesis,
				md::FrontPanel::StatusLed::Effects,
				md::FrontPanel::StatusLed::Routing,
			};
			std::array<bool, panelAffordances::g_machinedrumDataPages.size()> active{};
			for(size_t page = 0; page < active.size(); ++page)
				active[page] = frontPanel.getStatusLed(pages[page]);

			const auto current = panelAffordances::singleActiveIndex(active);
			if(!current || m_machinedrumDataPageTarget.completeIfAt(*current))
				return;

			const auto plan = panelAffordances::machinedrumDataPagePlan(*current, *target);
			if(plan && m_machinedrumDataPageTarget.beginAttempt())
				queuePanelPulse(plan->control);
			return;
		}

		const auto dataTarget = m_monomachineDataPageTarget.target();
		if(dataTarget)
		{
			constexpr uint8_t banks[] = { 0x25, 0x25, 0x25, 0x25, 0x26, 0x26, 0x26 };
			constexpr uint8_t bits[] = { 4, 5, 6, 7, 0, 1, 2 };
			std::array<bool, panelAffordances::g_monomachineDataPages.size()> active{};
			for(size_t page = 0; page < active.size(); ++page)
			{
				const auto raw = frontPanel.getLedBankRaw(banks[page]);
				active[page] = (raw & static_cast<uint8_t>(1u << bits[page])) == 0;
			}

			const auto current = panelAffordances::singleActiveIndex(active);
			if(current && !m_monomachineDataPageTarget.completeIfAt(*current))
			{
				const auto plan = panelAffordances::monomachineDataPagePlan(*current, *dataTarget);
				if(plan && m_monomachineDataPageTarget.beginAttempt())
				{
					queuePanelPulse(plan->control);
					return;
				}
			}
		}

		const auto modeTarget = m_monomachineTrigModeTarget.target();
		if(!modeTarget)
			return;

		const auto raw = frontPanel.getLedBankRaw(0x27);
		const bool amp = (raw & (1u << 1)) == 0;
		const bool filter = (raw & (1u << 2)) == 0;
		const bool lfo = (raw & (1u << 3)) == 0;
		const std::array<bool, panelAffordances::g_monomachineTrigModes.size()> active
		{{
			amp && !filter && !lfo,
			!amp && filter && !lfo,
			!amp && !filter && lfo,
			amp && filter && lfo,
		}};

		const auto current = panelAffordances::singleActiveIndex(active);
		if(!current || m_monomachineTrigModeTarget.completeIfAt(*current))
			return;

		const auto plan = panelAffordances::monomachineTrigModePlan(*current, *modeTarget);
		if(plan && m_monomachineTrigModeTarget.beginAttempt())
			queuePanelPulse(plan->control);
	}

	void Editor::selectMachinedrumTrack(const int _track)
	{
		const auto body = md::midiProtocol::selectTrack(_track);
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Editor);
		event.sysex.reserve(body.size() + 2);
		event.sysex.push_back(0xf0);
		event.sysex.insert(event.sysex.end(), body.begin(), body.end());
		event.sysex.push_back(0xf7);
		getProcessor().addMidiEvent(event);
	}

	void Editor::selectMachinedrumDataPage(const int _page)
	{
		if(m_machinedrumDataPageTarget.request(_page))
			servicePanelNavigation();
	}

	void Editor::selectMonomachineDataPage(const int _page)
	{
		if(m_monomachineDataPageTarget.request(_page))
			servicePanelNavigation();
	}

	void Editor::selectMonomachineTrigMode(const int _mode)
	{
		if(m_monomachineTrigModeTarget.request(_mode))
			servicePanelNavigation();
	}

	void Editor::togglePatternBankLatch(juceRmlUi::ElemButton* const _button, const md::PanelPacket& _packet)
	{
		const auto wasLatched = m_patternBankButton == _button;
		releasePatternBankLatch();
		if(wasLatched)
			return;

		m_patternBankButton = _button;
		m_patternBankPacket = _packet;
		juceRmlUi::ElemButton::setChecked(_button, true);

		if(auto* hw = getHardware())
		{
			const auto combined = m_panelRows.press(_packet);
			hw->sendPanelEvent(combined.row, combined.mask);
		}
	}

	void Editor::releasePatternBankLatch()
	{
		if(!m_patternBankPacket)
			return;

		if(m_patternBankButton)
			juceRmlUi::ElemButton::setChecked(m_patternBankButton, false);

		const auto combined = m_panelRows.release(*m_patternBankPacket);
		if(auto* hw = getHardware())
			hw->sendPanelEvent(combined.row, combined.mask);

		m_patternBankButton = nullptr;
		m_patternBankPacket.reset();
	}

	void Editor::createEncoders()
	{
		static const char* const ids[8] = { "encA","encB","encC","encD","encE","encF","encG","encH" };

		for(uint32_t i=0; i<8; ++i)
		{
			auto* k = findChild<juceRmlUi::ElemKnob>(ids[i], false);
			m_encoders[i] = k;
			configureEncoder(k, static_cast<md::PanelEncoder>(i), m_encLast[i], m_encAccum[i]);
		}

		m_levelEncoder = findChild<juceRmlUi::ElemKnob>("encLevel", false);
		configureEncoder(m_levelEncoder, md::PanelEncoder::Level, m_levelLast, m_levelAccum);

		m_soundEncoder = findChild<juceRmlUi::ElemKnob>("encSound", false);
		configureEncoder(m_soundEncoder, md::PanelEncoder::SoundSelection,
			m_soundLast, m_soundAccum);
	}

	std::unique_ptr<jucePluginEditorLib::SettingsDeviceSpecific> Editor::createDeviceSpecificSettings(
		const std::string& _templateName, Rml::Element* _root)
	{
		if (_templateName == "tus_settings_gui_Machinedrum" || _templateName == "tus_settings_gui_Monomachine")
			return std::make_unique<SettingsPanelFeel>(*this, _root);
		return jucePluginEditorLib::Editor::createDeviceSpecificSettings(_templateName, _root);
	}

	void Editor::applyPanelSpeeds()
	{
		auto& config = getProcessor().getConfig();
		const auto wheelPercent = config.getIntValue("panelWheelSpeedPercent", 100);
		const auto encoderPercent = config.getIntValue("panelEncoderSpeedPercent", 100);

		// The knob "speed" property is the mouse distance for a full sweep, so a
		// higher user-facing percentage means a smaller property value. Base values
		// mirror the skins' RCSS defaults.
		const auto isMonomachine = getModel() == md::MachineModel::Monomachine;

		// juceRmlUi::Element::getProperty() reads the attribute before the RCSS
		// property, so setting the attribute both overrides the skin default and
		// raises the change notification that refreshes the knob's cached speed.
		const auto apply = [](juceRmlUi::ElemKnob* const _knob, const float _baseSpeed, const int _percent)
		{
			if (!_knob || _percent <= 0)
				return;
			_knob->SetAttribute("speed", _baseSpeed * 100.0f / static_cast<float>(_percent));
		};

		const float encoderBase = isMonomachine ? 150.0f : 120.0f;
		for (auto* knob : m_encoders)
			apply(knob, encoderBase, encoderPercent);
		apply(m_levelEncoder, isMonomachine ? 150.0f : 100.0f, encoderPercent);
		apply(m_soundEncoder, 170.0f, wheelPercent);
	}

	void Editor::loadInstalledFactoryStorage()
	{
		if(m_model != md::MachineModel::Monomachine)
			return;

		auto* const processor = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		if(!processor)
			return;

		const auto isExactStorage = [](const juce::File& _file)
		{
			return _file.existsAsFile()
				&& _file.getSize() == static_cast<juce::int64>(md::g_patchRamStateSize);
		};

		const auto configuredPath = getProcessor().getConfig().getValue(
			"mmFactoryStoragePath");
		const auto configured = configuredPath.isNotEmpty()
			? juce::File(configuredPath) : juce::File{};
		if(isExactStorage(configured))
		{
			confirmStorageImage(configured, StorageImageBookmark::Factory);
			return;
		}

		const auto conventional = processor->getInstalledFactoryStorageImage();
		if(isExactStorage(conventional))
		{
			confirmStorageImage(conventional, StorageImageBookmark::Factory);
			return;
		}

		// Factory content is user-supplied and is never embedded in the product.
		// The first successful selection becomes a convenient remembered slot.
		chooseStorageImage(StorageImageBookmark::Factory);
	}

	void Editor::chooseStorageImage()
	{
		chooseStorageImage(StorageImageBookmark::Other);
	}

	void Editor::chooseStorageImage(const StorageImageBookmark _bookmark)
	{
		if(m_model != md::MachineModel::Monomachine)
			return;
		if(!jucePluginEditorLib::fileChooserFlow::tryBegin(m_storageImageFlow,
			StorageImageFlow::None, StorageImageFlow::Choosing))
		{
			showStorageOperationResult(false,
				"Finish the open storage image dialog first.");
			return;
		}

		auto& config = getProcessor().getConfig();
		const auto factorySelection = _bookmark == StorageImageBookmark::Factory;
		const auto lastPath = config.getValue(factorySelection
			? "mmFactoryStoragePath" : "mmStorageImageLastPath");
		const auto lastDirectory = factorySelection ? juce::String{}
			: config.getValue("mmStorageImageLastDirectory");
		juce::File initial;
		if(lastPath.isNotEmpty())
		{
			const juce::File remembered(lastPath);
			initial = remembered.existsAsFile()
				? remembered : remembered.getParentDirectory();
		}
		if(!initial.existsAsFile() && lastDirectory.isNotEmpty())
			initial = juce::File(lastDirectory);
		if(initial == juce::File())
		{
			if(auto* const processor = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
				initial = processor->getInstalledFactoryStorageImage().getParentDirectory();
		}
		if(!initial.exists())
			initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

		m_storageFileChooser = std::make_unique<juce::FileChooser>(
			factorySelection
				? "Choose an exact 1 MiB factory storage image"
				: "Choose an exact 1 MiB storage image",
			initial, "*.bin", true);
		const auto safeRoot =
			juce::Component::SafePointer<juceRmlUi::RmlComponent>(getRmlComponent());
		const std::function<void(const juce::FileChooser&)> completion =
			jucePluginEditorLib::fileChooserFlow::makeGuardedCompletion(
				safeRoot, &m_storageImageFlow, StorageImageFlow::Choosing,
				StorageImageFlow::None,
				[this, _bookmark](const juce::FileChooser& _chooser)
				{
					const auto file = _chooser.getResult();
					if(!file.existsAsFile())
						return;
					confirmStorageImage(file, _bookmark);
				});
		m_storageFileChooser->launchAsync(
			juce::FileBrowserComponent::openMode
				| juce::FileBrowserComponent::canSelectFiles,
			completion);
	}

	void Editor::restorePreviousStorage()
	{
		if(m_model != md::MachineModel::Monomachine)
			return;
		auto* const processor = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		if(!processor)
			return;
		const auto recovery = processor->getStorageRecoveryImage();
		if(!recovery.existsAsFile()
			|| recovery.getSize() != static_cast<juce::int64>(md::g_patchRamStateSize))
		{
			showStorageOperationResult(false,
				"No complete 1 MiB recovery image is available yet.\n\nExpected at:\n"
				+ recovery.getFullPathName());
			return;
		}
		confirmStorageImage(recovery, StorageImageBookmark::None);
	}

	bool Editor::hasStorageRecoveryImage() const
	{
		auto* const processor = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		if(!processor || m_model != md::MachineModel::Monomachine)
			return false;
		const auto recovery = processor->getStorageRecoveryImage();
		return recovery.existsAsFile()
			&& recovery.getSize() == static_cast<juce::int64>(md::g_patchRamStateSize);
	}

	void Editor::confirmStorageImage(const juce::File& _file,
		const StorageImageBookmark _bookmark)
	{
		if(!jucePluginEditorLib::fileChooserFlow::tryBegin(m_storageImageFlow,
			StorageImageFlow::None, StorageImageFlow::AwaitingConfirmation))
		{
			showStorageOperationResult(false,
				"Finish the open storage image dialog first.");
			return;
		}

		if(!_file.existsAsFile()
			|| _file.getSize() != static_cast<juce::int64>(md::g_patchRamStateSize))
		{
			m_storageImageFlow = StorageImageFlow::None;
			showStorageOperationResult(false,
				"Storage was not changed. The selected image must be exactly 1 MiB.");
			return;
		}
		auto* const processor = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		if(!processor)
		{
			m_storageImageFlow = StorageImageFlow::None;
			return;
		}
		const auto recoveryPath = processor->getStorageRecoveryImage().getFullPathName();

		const auto safeRoot =
			juce::Component::SafePointer<juceRmlUi::RmlComponent>(getRmlComponent());
		const genericUI::MessageBox::Callback completion =
			jucePluginEditorLib::fileChooserFlow::makeGuardedCompletion(
				safeRoot, &m_storageImageFlow,
				StorageImageFlow::AwaitingConfirmation, StorageImageFlow::None,
				[this, file = _file, _bookmark](
					const genericUI::MessageBox::Result _answer)
				{
					if(_answer != genericUI::MessageBox::Result::Yes)
						return;

					auto* const processor =
						dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
					if(!processor)
						return;
					juce::String result;
					const bool loaded = processor->loadStorageImage(file, result);
					if(loaded && _bookmark != StorageImageBookmark::None)
					{
						auto& config = getProcessor().getConfig();
						if(_bookmark == StorageImageBookmark::Factory)
							config.setValue("mmFactoryStoragePath",
								file.getFullPathName());
						else
						{
							config.setValue("mmStorageImageLastPath",
								file.getFullPathName());
							config.setValue("mmStorageImageLastDirectory",
								file.getParentDirectory().getFullPathName());
						}
						config.saveIfNeeded();
					}
					showStorageOperationResult(loaded, result);
				});

		const auto message = "Load '" + _file.getFileName()
			+ "'?\n\nThis replaces every kit, pattern, song, and global in "
				"machine storage, then reboots the machine.\n\n"
				"A recovery copy of the current 1 MiB storage will be written first. "
				"If that backup cannot be saved, nothing will be changed.\n\nRecovery file:\n"
			+ recoveryPath;
		genericUI::MessageBox::showYesNo(genericUI::MessageBox::Icon::Warning,
			"Replace machine storage?", message.toStdString(), completion);
	}

	void Editor::showStorageOperationResult(const bool _success,
		const juce::String& _message)
	{
		genericUI::MessageBox::showOk(_success
				? genericUI::MessageBox::Icon::Info
				: genericUI::MessageBox::Icon::Warning,
			_success ? "Machine storage loaded" : "Machine storage unchanged",
			_message.toStdString(), getRmlComponent());
	}

	void Editor::createMasterVolume()
	{
		m_masterVolume = findChild<juceRmlUi::ElemKnob>("encMaster", false);
		if(!m_masterVolume)
			return;

		m_masterVolume->setMinValue(0.0f);
		m_masterVolume->setMaxValue(1.0f);
		m_masterVolume->setEndless(false);
		m_masterVolume->setValue(
			std::clamp(getProcessor().getOutputGain(), 0.0f, 1.0f), false);

		juceRmlUi::EventListener::Add(m_masterVolume, Rml::EventId::Change,
			[this](Rml::Event&)
			{
				getProcessor().setOutputGain(std::clamp(
					juceRmlUi::ElemValue::getValue(m_masterVolume), 0.0f, 1.0f));
			});
	}

	void Editor::configureEncoder(juceRmlUi::ElemKnob* const _knob,
		const md::PanelEncoder _encoder, float& _last, float& _accum)
	{
		if(!_knob)
			return;

		_knob->setMinValue(0.0f);
		_knob->setMaxValue(g_encoderRange);
		_knob->setEndless(true);
		_knob->setValue(g_encoderRange * 0.5f, false);	// no spurious Change at init
		_last = g_encoderRange * 0.5f;
		_accum = 0.0f;

		juceRmlUi::EventListener::Add(_knob, Rml::EventId::Change,
			[this, _knob, _encoder, &_last, &_accum](Rml::Event&)
			{
				onEncoderChanged(_knob, _encoder, _last, _accum);
			});
	}

	void Editor::onEncoderChanged(juceRmlUi::ElemKnob* const _knob,
		const md::PanelEncoder _encoder, float& _last, float& _accum)
	{
		if(!_knob)
			return;

		const float v = juceRmlUi::ElemValue::getValue(_knob);

		float delta = v - _last;

		// unwrap across the endless range boundary
		if(delta > g_encoderRange * 0.5f)
			delta -= g_encoderRange;
		else if(delta < -g_encoderRange * 0.5f)
			delta += g_encoderRange;

		_last = v;

		// accumulate fractional movement into whole detents
		_accum += delta;
		const int steps = static_cast<int>(_accum);

		if(steps == 0)
			return;

		_accum -= static_cast<float>(steps);

		auto* hw = getHardware();

		if(!hw)
			return;

		// Emit one ±1 panel event per detent (0x01 = +1, 0xff = -1), matching the
		// documented DATA ENTRY encoder packets; robust if only ±1 is honored.
		const auto cmd = md::panelEncoderCommand(getModel(), _encoder);
		if(!cmd)
			return;
		const uint8_t arg = steps > 0 ? 0x01 : 0xff;
		const int n = std::min(std::abs(steps), g_encoderBurstCap);

		for(int s=0; s<n; ++s)
			hw->sendPanelEvent(*cmd, arg);
	}

	void Editor::createLeds()
	{
		for(uint32_t i=0; i<16; ++i)
		{
			m_stepLeds[i] = findChild("stepLed" + std::to_string(i), false);
			m_drumLeds[i] = findChild("drumLed" + std::to_string(i), false);
		}

		if(getModel() == md::MachineModel::Monomachine)
		{
			// Monomachine uses a separate active-low LED-bank layout.
			const struct { const char* id; uint8_t bank; uint8_t bit; } leds[] =
			{
				{ "mmPageLed0", 0x25, 4 }, { "mmPageLed1", 0x25, 5 },
				{ "mmPageLed2", 0x25, 6 }, { "mmPageLed3", 0x25, 7 },
				{ "mmPageLed4", 0x26, 0 }, { "mmPageLed5", 0x26, 1 },
				{ "mmPageLed6", 0x26, 2 },
				{ "mmBankGroupAD", 0x26, 3 }, { "mmBankGroupEH", 0x26, 4 },
				{ "stPattern", 0x26, 5 }, { "stSong", 0x26, 6 },
				{ "mmTempoLed", 0x26, 7 },
				{ "mmRecordLed", 0x27, 0 },
				{ "mmTrigAmp", 0x27, 1 }, { "mmTrigFilter", 0x27, 2 },
				{ "mmTrigLfo", 0x27, 3 },
				{ "mmTrackPage0", 0x27, 4 }, { "mmTrackPage1", 0x27, 5 },
				{ "mmTrackPage2", 0x27, 6 }, { "mmTrackPage3", 0x27, 7 },
			};
			static_assert(std::size(leds) == 20);
			for(size_t i = 0; i < std::size(leds); ++i)
				m_mmPanelLeds[i] = { findChild(leds[i].id, false), leds[i].bank, leds[i].bit };
			return;
		}

		const std::pair<const char*, md::FrontPanel::StatusLed> status[] =
		{
			{ "stPattern", md::FrontPanel::StatusLed::Pattern   },
			{ "stSong",    md::FrontPanel::StatusLed::Song      },
			{ "stSynth",   md::FrontPanel::StatusLed::Synthesis },
			{ "stFx",      md::FrontPanel::StatusLed::Effects   },
			{ "stRoute",   md::FrontPanel::StatusLed::Routing   },
			{ "ledTempo",  md::FrontPanel::StatusLed::Tempo     },
		};
		static_assert(std::size(status) == std::tuple_size_v<decltype(m_statusLeds)>);

		for(size_t i=0; i<m_statusLeds.size(); ++i)
			m_statusLeds[i] = { findChild(status[i].first, false), static_cast<uint8_t>(status[i].second) };

		const std::pair<const char*, md::FrontPanel::ModeLed> mode[] =
		{
			{ "ledClassic",  md::FrontPanel::ModeLed::Classic     },
			{ "ledExtended", md::FrontPanel::ModeLed::Extended    },
			{ "ledBankAD",   md::FrontPanel::ModeLed::BankGroupAD },
			{ "ledBankEH",   md::FrontPanel::ModeLed::BankGroupEH },
			{ "ledRecord",   md::FrontPanel::ModeLed::Record      },
		};
		static_assert(std::size(mode) == std::tuple_size_v<decltype(m_mdModeLeds)>);

		for(size_t i=0; i<m_mdModeLeds.size(); ++i)
			m_mdModeLeds[i] = { findChild(mode[i].first, false), static_cast<uint8_t>(mode[i].second) };
	}

	void Editor::updateLeds()
	{
		if(!m_frontPanelSnapshotValid)
			return;

		const auto& fp = m_frontPanelSnapshot;
		const auto isMonomachine = getModel() == md::MachineModel::Monomachine;

		for(uint32_t i=0; i<16; ++i)
		{
			if(m_stepLeds[i])
				m_stepLeds[i]->SetClass("lit", isMonomachine
					? fp.getMonomachineStepLed(i) : fp.getStepLed(i));
		}

		if(isMonomachine)
		{
			const struct { uint8_t greenBank, greenBit, redBank, redBit; } tracks[] =
			{
				{ 0x25, 0, 0x25, 1 }, { 0x25, 2, 0x25, 3 },
				{ 0x24, 0, 0x24, 1 }, { 0x24, 2, 0x24, 3 },
				{ 0x24, 4, 0x24, 5 }, { 0x24, 6, 0x24, 7 },
			};
			const auto lit = [&fp](const uint8_t bank, const uint8_t bit)
			{
				return (fp.getLedBankRaw(bank) & static_cast<uint8_t>(1u << bit)) == 0;
			};
			for(size_t i = 0; i < std::size(tracks); ++i)
			{
				if(!m_drumLeds[i])
					continue;
				const bool green = lit(tracks[i].greenBank, tracks[i].greenBit);
				const bool red = lit(tracks[i].redBank, tracks[i].redBit);
				m_drumLeds[i]->SetClass("green", green && !red);
				m_drumLeds[i]->SetClass("red", red && !green);
				m_drumLeds[i]->SetClass("yellow", green && red);
			}
			for(const auto& led : m_mmPanelLeds)
			{
				if(led.elem)
					led.elem->SetClass("lit", lit(led.bank, led.bit));
			}
			return;
		}

		for(uint32_t i=0; i<16; ++i)
		{
			if(m_drumLeds[i])
				m_drumLeds[i]->SetClass("lit", fp.getDrumLed(i));
		}

		for(const auto& s : m_statusLeds)
		{
			if(s.elem)
				s.elem->SetClass("lit", fp.getStatusLed(static_cast<md::FrontPanel::StatusLed>(s.bit)));
		}

		for(const auto& m : m_mdModeLeds)
		{
			if(m.elem)
				m.elem->SetClass("lit", fp.getModeLed(static_cast<md::FrontPanel::ModeLed>(m.bit)));
		}
	}

	void Editor::paintLcd(const juce::Image& _target, juce::Graphics& _g) const
	{
		const auto isMonomachine = getModel() == md::MachineModel::Monomachine;
		const auto lcdOff = isMonomachine ? g_mmLcdOff : g_mdLcdOff;
		const auto lcdOn = isMonomachine ? g_mmLcdOn : g_mdLcdOn;

		if(!m_frontPanelSnapshotValid)
		{
			_g.fillAll(juce::Colour(lcdOff));
			return;
		}

		const auto& fp = m_frontPanelSnapshot;

		juce::Image lcd(juce::Image::ARGB, md::FrontPanel::g_lcdWidth, md::FrontPanel::g_lcdHeight, false);

		{
			const juce::Image::BitmapData bd(lcd, juce::Image::BitmapData::writeOnly);

			for(uint32_t y=0; y<md::FrontPanel::g_lcdHeight; ++y)
			{
				for(uint32_t x=0; x<md::FrontPanel::g_lcdWidth; ++x)
					bd.setPixelColour(x, y, juce::Colour(fp.getLcdPixel(x, y) ? lcdOn : lcdOff));
			}
		}

		_g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);	// crisp pixels, no blur
		_g.drawImage(lcd,
			0, 0, _target.getWidth(), _target.getHeight(),
			0, 0, static_cast<int>(md::FrontPanel::g_lcdWidth), static_cast<int>(md::FrontPanel::g_lcdHeight));
	}

	void Editor::timerCallback()
	{
		m_frontPanelSnapshotValid = refreshFrontPanelSnapshot();
		servicePanelQueue();

		if(m_lcdCanvas && m_lcdChanged)
			m_lcdCanvas->repaint();

		updateLeds();
	}

	std::pair<std::string, std::string> Editor::getDemoRestrictionText() const
	{
		return {};
	}
}
