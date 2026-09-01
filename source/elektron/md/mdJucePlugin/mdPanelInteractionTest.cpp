#include "mdEditor.h"
#include "mdPluginProcessor.h"

#include "jucePluginEditorLib/pluginEditorState.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlElemKnob.h"

#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/ElementDocument.h"

#include "juce_events/juce_events.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace mdJucePlugin
{
	// Keep production interaction methods private while allowing this test to
	// inspect the state reached through the live RmlUi event listeners.
	struct EditorIdentityTestAccess
	{
		static uint8_t rowMask(const Editor& _editor, const uint8_t _row)
		{
			return _editor.m_panelRows.mask(_row);
		}

		static size_t heldCount(const Editor& _editor)
		{
			return _editor.m_shiftPanelLatch.size();
		}

		static size_t activeCount(const Editor& _editor)
		{
			return _editor.m_activePanelButtons.size();
		}

		static size_t gestureCount(const Editor& _editor)
		{
			return _editor.m_panelGesturePackets.size();
		}

		static void presentLed(Editor& _editor, const uint8_t _bank,
			const uint8_t _bit)
		{
			md::FrontPanel blank;
			_editor.m_frontPanelSnapshot = blank;
			_editor.m_frontPanelSnapshotValid = true;
			_editor.m_ledPresentation.reset(blank);
			_editor.m_ledPresentation.apply({1, 0, _bank,
				static_cast<uint8_t>(0xffu & ~(1u << _bit))}, 0.0);
			(void)_editor.m_ledPresentation.advance(1.0);
			_editor.m_ledsChanged = true;
			_editor.updateLeds();
		}

		static void loseFocus(Editor& _editor)
		{
			_editor.globalFocusChanged(nullptr);
		}
	};
}

namespace
{
	using Access = mdJucePlugin::EditorIdentityTestAccess;

	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "mdPanelInteractionTest: " << _message << '\n';
		std::exit(1);
	}

	Rml::Dictionary mouseParameters(const bool _shift = false,
		const bool _command = false, const int _x = 0, const int _y = 0,
		const bool _control = false)
	{
		Rml::Dictionary result;
		result["shift_key"] = static_cast<int>(_shift);
		result["meta_key"] = static_cast<int>(_command);
		result["ctrl_key"] = static_cast<int>(_control);
		result["mouse_x"] = _x;
		result["mouse_y"] = _y;
		result["button"] = 0;
		return result;
	}

	Rml::Element* element(mdJucePlugin::Editor& _editor, const char* const _id)
	{
		auto* const result = _editor.findChild(_id, false);
		expect(result != nullptr, "live skin is missing a panel control");
		return result;
	}

	juceRmlUi::ElemButton* button(mdJucePlugin::Editor& _editor,
		const char* const _id)
	{
		auto* const result = dynamic_cast<juceRmlUi::ElemButton*>(element(_editor, _id));
		expect(result != nullptr, "panel control is not a live button");
		return result;
	}

	void dispatch(Rml::Element* const _element, const Rml::EventId _event,
		const Rml::Dictionary& _parameters = {})
	{
		expect(_element != nullptr, "cannot dispatch an event to a missing element");
		// A false return means a listener intentionally stopped propagation, not
		// that dispatch failed. The target listener has still received the event.
		(void)_element->DispatchEvent(_event, _parameters);
	}

	void shiftClick(Rml::Element* const _element)
	{
		const auto shifted = mouseParameters(true);
		dispatch(_element, Rml::EventId::Mousedown, shifted);
		dispatch(_element, Rml::EventId::Mouseup, shifted);
	}

	void releaseShift(mdJucePlugin::Editor& _editor)
	{
		Rml::Dictionary parameters;
		parameters["shift_key"] = 0;
		dispatch(_editor.getDocument(), Rml::EventId::Keyup, parameters);
	}

	void expectAllRowsReleased(const mdJucePlugin::Editor& _editor)
	{
		for(uint8_t row = 0x20; row <= 0x25; ++row)
			expect(Access::rowMask(_editor, row) == 0,
				"cleanup left a firmware panel row held");
		expect(Access::heldCount(_editor) == 0, "cleanup left a Shift latch held");
		expect(Access::activeCount(_editor) == 0, "cleanup left a mouse button active");
	}

	void checkMultipleTrigLatch(mdJucePlugin::Editor& _editor)
	{
		auto* const trig1 = button(_editor, "trigKey0");
		auto* const trig6 = button(_editor, "trigKey5");
		auto* const trig11 = button(_editor, "trigKey10");
		shiftClick(trig1);
		shiftClick(trig6);
		shiftClick(trig11);

		expect(Access::heldCount(_editor) == 3,
			"live Shift-click path did not retain all three trigs");
		expect(Access::rowMask(_editor, 0x20) == 0x21,
			"live Shift-click path combined trig row 0 incorrectly");
		expect(Access::rowMask(_editor, 0x21) == 0x04,
			"live Shift-click path combined trig row 1 incorrectly");
		expect(trig1->isChecked() && trig6->isChecked() && trig11->isChecked(),
			"latched trig did not remain visibly pressed");

		releaseShift(_editor);
		expectAllRowsReleased(_editor);
		expect(!trig1->isChecked() && !trig6->isChecked() && !trig11->isChecked(),
			"Shift key-up did not clear checked trig states");
	}

	void checkRecordPlayChord(mdJucePlugin::Editor& _editor)
	{
		auto* const record = button(_editor, "btRec");
		auto* const play = button(_editor, "btPlay");
		shiftClick(record);
		expect(Access::heldCount(_editor) == 1
			&& Access::rowMask(_editor, 0x22) == 0x02,
			"Shift+REC did not leave only RECORD held");

		const auto shifted = mouseParameters(true);
		dispatch(play, Rml::EventId::Mousedown, shifted);
		expect(Access::activeCount(_editor) == 1
			&& Access::rowMask(_editor, 0x22) == 0x06,
			"Shift+REC+PLAY did not present the combined firmware row");

		// Exercise the document listener used by a real native Shift release.
		releaseShift(_editor);
		expectAllRowsReleased(_editor);
		expect(!record->isChecked() && !play->isChecked(),
			"record/play cleanup left a button checked");

		// A delayed physical mouse-up after forced cleanup must be harmless.
		dispatch(play, Rml::EventId::Mouseup, shifted);
		expectAllRowsReleased(_editor);
	}

	void checkFocusLossCleanup(mdJucePlugin::Editor& _editor)
	{
		auto* const trig = button(_editor, "trigKey15");
		auto* const play = button(_editor, "btPlay");
		shiftClick(trig);
		dispatch(play, Rml::EventId::Mousedown, mouseParameters(true));
		expect(Access::heldCount(_editor) == 1 && Access::activeCount(_editor) == 1,
			"focus-loss fixture did not create held and momentary inputs");
		Access::loseFocus(_editor);
		expectAllRowsReleased(_editor);
		expect(!trig->isChecked() && !play->isChecked(),
			"focus loss left a visible panel button pressed");

		auto* const shortcut = element(_editor, "altMute");
		dispatch(shortcut, Rml::EventId::Mousedown, mouseParameters());
		expect(Access::gestureCount(_editor) == 2 && shortcut->IsClassSet("active"),
			"direct panel shortcut did not begin its FUNCTION chord");
		Access::loseFocus(_editor);
		expectAllRowsReleased(_editor);
		expect(Access::gestureCount(_editor) == 0 && !shortcut->IsClassSet("active"),
			"focus loss left a direct panel shortcut held");
	}

	float dragDelta(juceRmlUi::ElemKnob& _knob, const bool _command)
	{
		_knob.setValue(50.0f, false);
		dispatch(&_knob, Rml::EventId::Mousedown,
			mouseParameters(true, _command));
		// A zero-distance drag selects and anchors the modifier; the second measures it.
		dispatch(&_knob, Rml::EventId::Drag,
			mouseParameters(true, _command));
		dispatch(&_knob, Rml::EventId::Drag,
			mouseParameters(true, _command, 25));
		return std::abs(_knob.getValue() - 50.0f);
	}

	void checkShiftCommandEncoder(mdJucePlugin::Editor& _editor)
	{
		auto* const knob = dynamic_cast<juceRmlUi::ElemKnob*>(element(_editor, "encA"));
		expect(knob != nullptr, "live DATA ENTRY A is not a knob");
		const auto shifted = dragDelta(*knob, false);
		const auto commandFine = dragDelta(*knob, true);
		expect(shifted > 0.0f, "Shift-only encoder drag did not move");
		expect(commandFine > shifted * 0.15f && commandFine < shifted * 0.25f,
			"Shift+Command/Ctrl did not select the 20% fine encoder modifier");
		expectAllRowsReleased(_editor);
	}

	void checkLiveLedMappings(mdJucePlugin::Editor& _editor)
	{
		const struct { const char* id; uint8_t bank; uint8_t bit; } cases[] =
		{
			{ "mdPatternPage0", 0x22, 0 },
			{ "mdPatternPage1", 0x22, 1 },
			{ "mdPatternPage2", 0x22, 2 },
			{ "mdPatternPage3", 0x23, 6 },
			{ "ledTempo", 0x23, 5 },
		};
		for(const auto& ledCase : cases)
		{
			auto* const led = element(_editor, ledCase.id);
			Access::presentLed(_editor, ledCase.bank, ledCase.bit);
			expect(led->IsClassSet("lit"),
				"front-panel LED bank did not light its live skin element");
		}
	}

	void checkMonomachineBankLatch(mdJucePlugin::Editor& _editor)
	{
		auto* const bank = button(_editor, "btBankA");
		dispatch(bank, Rml::EventId::Mousedown, mouseParameters());
		const auto packet = md::panelPacket(md::MachineModel::Monomachine,
			md::PanelControl::BankA);
		expect(packet && Access::rowMask(_editor, packet->row) == packet->mask,
			"MM bank latch did not retain its firmware row state");
		expect(bank->isChecked(), "MM bank latch did not remain visibly pressed");
		Access::loseFocus(_editor);
		expectAllRowsReleased(_editor);
		expect(!bank->isChecked(), "MM bank latch survived focus loss");
	}
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juce;

#if defined(MD_PANEL_TEST_MONOMACHINE)
	mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig mmConfig;
	mmConfig.isolateDeviceStorage = true;
	mdJucePlugin::AudioPluginAudioProcessor mmProcessor(
		md::MachineModel::Monomachine, std::move(mmConfig), false);
	auto& mmState = mmProcessor.getOrCreateEditorState();
	auto* const mmEditor = dynamic_cast<mdJucePlugin::Editor*>(mmState.getEditor());
	expect(mmEditor != nullptr, "embedded Monomachine skin did not create its editor");
	checkMonomachineBankLatch(*mmEditor);
	mmProcessor.destroyEditorState();
#else
	mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig config;
	config.isolateDeviceStorage = true;
	mdJucePlugin::AudioPluginAudioProcessor processor(md::MachineModel::Machinedrum,
		std::move(config), false);
	auto& state = processor.getOrCreateEditorState();
	auto* const editor = dynamic_cast<mdJucePlugin::Editor*>(state.getEditor());
	expect(editor != nullptr, "embedded Machinedrum skin did not create its editor");

	checkMultipleTrigLatch(*editor);
	checkRecordPlayChord(*editor);
	checkFocusLossCleanup(*editor);
	checkShiftCommandEncoder(*editor);
	checkLiveLedMappings(*editor);

	processor.destroyEditorState();
#endif
	std::cout << "mdPanelInteractionTest: PASS\n";
	return 0;
}
