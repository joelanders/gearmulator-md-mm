#include "mdPanelMidiMap.h"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace mdJucePlugin::panelMidi;

	constexpr auto g_md = md::MachineModel::Machinedrum;
	constexpr auto g_mm = md::MachineModel::Monomachine;

	void require(const bool _condition, const std::string& _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	Map makeMap(const md::MachineModel _model, const EncoderMode _mode, const uint8_t _channel = 0)
	{
		auto table = makeDefaultTable(_model);
		table.channel = _channel;
		for(auto& encoder : table.encoders)
			encoder.mode = _mode;
		Map map;
		map.setTable(table);
		return map;
	}

	void testAbsoluteEncoder()
	{
		auto map = makeMap(g_md, EncoderMode::Absolute);

		// The first value only anchors the controller position.
		require(!map.translate(0xb0, 16, 100), "first absolute value must not move the encoder");

		auto a = map.translate(0xb0, 16, 103);
		require(a && a->kind == Action::Kind::EncoderSteps && a->steps == 3
			&& a->encoder == md::PanelEncoder::DataEntryA, "absolute +3");

		a = map.translate(0xb0, 16, 98);
		require(a && a->steps == -5, "absolute -5");

		require(!map.translate(0xb0, 16, 98), "unchanged absolute value is silent");

		// Each controller number tracks its own history.
		require(!map.translate(0xb0, 17, 10), "encoder B anchors independently");
		a = map.translate(0xb0, 17, 12);
		require(a && a->steps == 2 && a->encoder == md::PanelEncoder::DataEntryB, "encoder B +2");

		// Level and sound selection follow encoder H.
		require(!map.translate(0xb0, 24, 0), "level anchors");
		a = map.translate(0xb0, 24, 1);
		require(a && a->encoder == md::PanelEncoder::Level, "CC 24 is Level");
		require(!map.translate(0xb0, 25, 0), "sound anchors");
		a = map.translate(0xb0, 25, 1);
		require(a && a->encoder == md::PanelEncoder::SoundSelection, "CC 25 is Sound selection on the MD");

		// A reset (controller or mode change) re-anchors instead of jumping.
		map.reset();
		require(!map.translate(0xb0, 16, 5), "re-anchors after reset");

		// Replacing the table also re-anchors.
		require(map.translate(0xb0, 16, 9).has_value(), "moves again after anchoring");
		map.setTable(map.getTable());
		require(!map.translate(0xb0, 16, 20), "re-anchors after setTable");
	}

	void testRelativeEncoders()
	{
		auto offset = makeMap(g_md, EncoderMode::RelativeOffset);
		require(!offset.translate(0xb0, 20, 64), "offset 64 is still");
		auto a = offset.translate(0xb0, 20, 66);
		require(a && a->steps == 2 && a->encoder == md::PanelEncoder::DataEntryE, "offset +2");
		a = offset.translate(0xb0, 20, 63);
		require(a && a->steps == -1, "offset -1");

		auto twos = makeMap(g_md, EncoderMode::RelativeTwosComplement);
		a = twos.translate(0xb0, 16, 3);
		require(a && a->steps == 3, "two's complement +3");
		a = twos.translate(0xb0, 16, 127);
		require(a && a->steps == -1, "two's complement -1");
		a = twos.translate(0xb0, 16, 125);
		require(a && a->steps == -3, "two's complement -3");
		require(!twos.translate(0xb0, 16, 0), "two's complement 0 is still");
		require(!twos.translate(0xb0, 16, 64), "two's complement 64 is treated as still");
	}

	void testModesAreIndependentPerEncoder()
	{
		auto table = makeDefaultTable(g_md);
		table.encoders[0].mode = EncoderMode::RelativeOffset;
		table.encoders[1].mode = EncoderMode::Absolute;
		Map map;
		map.setTable(table);

		auto a = map.translate(0xb0, 16, 65);
		require(a && a->steps == 1, "encoder A is relative");
		require(!map.translate(0xb0, 17, 65), "encoder B is absolute and anchors on 65");
	}

	void testTriggerKeys()
	{
		auto map = makeMap(g_md, EncoderMode::Absolute);

		// By default the trig keys are controllers, like the other buttons.
		auto a = map.translate(0xb0, 65, 127);
		require(a && a->kind == Action::Kind::ButtonDown && a->control == md::PanelControl::Trigger1,
			"CC 65 down is trig 1");
		a = map.translate(0xb0, 80, 64);
		require(a && a->kind == Action::Kind::ButtonDown && a->control == md::PanelControl::Trigger16,
			"CC 80 down is trig 16, threshold is 64");
		a = map.translate(0xb0, 65, 0);
		require(a && a->kind == Action::Kind::ButtonUp && a->control == md::PanelControl::Trigger1,
			"CC 65 value 0 releases");
		a = map.translate(0xb0, 69, 63);
		require(a && a->kind == Action::Kind::ButtonUp && a->control == md::PanelControl::Trigger5,
			"63 releases");
		require(!map.translate(0xb0, 81, 127), "CC 81 is unbound");
		require(!map.translate(0x90, 36, 100), "notes are unbound by default");

		// A trig key can still be bound to a note.
		auto table = makeDefaultTable(g_md);
		table.buttons[static_cast<size_t>(md::PanelControl::Trigger1)] = { Source::Kind::Note, 36 };
		table.buttons[static_cast<size_t>(md::PanelControl::Trigger5)] = { Source::Kind::Note, 40 };
		map.setTable(table);
		a = map.translate(0x90, 36, 100);
		require(a && a->kind == Action::Kind::ButtonDown && a->control == md::PanelControl::Trigger1,
			"note 36 down is trig 1 once bound");
		a = map.translate(0x90, 36, 0);
		require(a && a->kind == Action::Kind::ButtonUp, "note-on velocity 0 is release");
		a = map.translate(0x80, 40, 64);
		require(a && a->kind == Action::Kind::ButtonUp && a->control == md::PanelControl::Trigger5, "note-off is release");
		require(!map.translate(0xb0, 65, 127), "its old CC is free");
	}

	void testButtonControllers()
	{
		auto mdMap = makeMap(g_md, EncoderMode::Absolute);
		auto mmMap = makeMap(g_mm, EncoderMode::Absolute);

		// CC 26 is Track 1 in the factory order, which only the MM has.
		auto a = mmMap.translate(0xb0, 26, 127);
		require(a && a->kind == Action::Kind::ButtonDown && a->control == md::PanelControl::Track1,
			"MM: CC 26 down is track 1");
		a = mmMap.translate(0xb0, 26, 0);
		require(a && a->kind == Action::Kind::ButtonUp && a->control == md::PanelControl::Track1,
			"MM: CC 26 up is track 1");
		require(!mdMap.translate(0xb0, 26, 127), "MD has no track buttons, so CC 26 is unbound");

		a = mmMap.translate(0xb0, 32, 64);
		require(a && a->control == md::PanelControl::Function && a->kind == Action::Kind::ButtonDown,
			"threshold is 64");
		a = mmMap.translate(0xb0, 32, 63);
		require(a && a->kind == Action::Kind::ButtonUp, "63 releases");
	}

	void testDefaultTables()
	{
		for(const auto model : { g_md, g_mm })
		{
			const auto table = makeDefaultTable(model);
			std::set<std::pair<int, int>> used;

			for(size_t i = 0; i < g_encoderCount; ++i)
			{
				const auto encoder = static_cast<md::PanelEncoder>(i);
				const auto& source = table.encoders[i].source;
				require(source.valid() == isAvailable(model, encoder),
					std::string("encoder binding matches availability: ") + md::panelEncoderName(encoder));
				if(source.valid())
					require(used.insert({ static_cast<int>(source.kind), source.number }).second,
						std::string("duplicate source for ") + md::panelEncoderName(encoder));
			}

			for(size_t i = 0; i < g_pushCount; ++i)
			{
				const auto encoder = static_cast<md::PanelEncoder>(i);
				const auto& source = table.pushes[i];
				require(source.valid() == isPushAvailable(model, encoder),
					std::string("push binding matches availability: ") + md::panelEncoderName(encoder));
				if(source.valid())
					require(used.insert({ static_cast<int>(source.kind), source.number }).second,
						std::string("duplicate source for push of ") + md::panelEncoderName(encoder));
			}

			for(size_t i = 0; i < g_controlCount; ++i)
			{
				const auto control = static_cast<md::PanelControl>(i);
				const auto& source = table.buttons[i];
				require(source.valid() == isAvailable(model, control),
					std::string("button binding matches availability: ") + md::panelControlName(control));
				if(source.valid())
					require(used.insert({ static_cast<int>(source.kind), source.number }).second,
						std::string("duplicate source for ") + md::panelControlName(control));
			}
		}

		// The two panels really differ.
		require(!isAvailable(g_mm, md::PanelEncoder::SoundSelection), "MM has no sound selection encoder");
		require(isAvailable(g_md, md::PanelEncoder::SoundSelection), "MD has a sound selection encoder");
		require(isAvailable(g_mm, md::PanelControl::Track1) && !isAvailable(g_md, md::PanelControl::Track1),
			"track buttons are MM only");
		require(isAvailable(g_md, md::PanelControl::SynthesisEffectsRouting)
			&& !isAvailable(g_mm, md::PanelControl::SynthesisEffectsRouting), "synth/FX/routing is MD only");
	}

	void testEncoderPush()
	{
		for(const auto model : { g_md, g_mm })
		{
			auto map = makeMap(model, EncoderMode::Absolute);

			// Only the data entry encoders can be pressed.
			for(size_t i = 0; i < g_encoderCount; ++i)
				require(isPushAvailable(model, static_cast<md::PanelEncoder>(i)) == (i < g_pushCount),
					"only encoders A-H have a push switch");

			auto a = map.translate(0xb0, 57, 127);
			require(a && a->kind == Action::Kind::EncoderPushDown && a->encoder == md::PanelEncoder::DataEntryA,
				"CC 57 presses encoder A");
			a = map.translate(0xb0, 57, 0);
			require(a && a->kind == Action::Kind::EncoderPushUp && a->encoder == md::PanelEncoder::DataEntryA,
				"CC 57 value 0 lets go");
			a = map.translate(0xb0, 64, 64);
			require(a && a->kind == Action::Kind::EncoderPushDown && a->encoder == md::PanelEncoder::DataEntryH,
				"CC 64 presses encoder H, threshold is 64");
			a = map.translate(0xb0, 64, 63);
			require(a && a->kind == Action::Kind::EncoderPushUp, "63 lets go");
			require(!map.translate(0xb0, 90, 127), "CC 90 is unbound");
		}

		// A push can also come from a note, like the trig keys do.
		auto table = makeDefaultTable(g_md);
		table.pushes[2] = { Source::Kind::Note, 70 };
		Map map;
		map.setTable(table);
		auto a = map.translate(0x90, 70, 100);
		require(a && a->kind == Action::Kind::EncoderPushDown && a->encoder == md::PanelEncoder::DataEntryC,
			"note 70 presses encoder C");
		a = map.translate(0x80, 70, 0);
		require(a && a->kind == Action::Kind::EncoderPushUp, "note-off lets go");
		require(!map.translate(0xb0, 59, 127), "the old CC of encoder C's push is free now");
	}

	void testHeldStepWhileTurning()
	{
		// The p-lock gesture: a trig key is held while an encoder turns. The map
		// must report the hold, then the turn, then the release, in that order.
		auto map = makeMap(g_md, EncoderMode::RelativeOffset);
		std::vector<Action> seen;
		for(const auto& message : { RawMessage{ 0xb0, 65, 127 }, RawMessage{ 0xb0, 16, 66 },
			RawMessage{ 0xb0, 16, 62 }, RawMessage{ 0xb0, 65, 0 } })
			if(const auto action = map.translate(message))
				seen.push_back(*action);

		require(seen.size() == 4, "all four messages produce an action");
		require(seen[0].kind == Action::Kind::ButtonDown && seen[0].control == md::PanelControl::Trigger1, "hold");
		require(seen[1].kind == Action::Kind::EncoderSteps && seen[1].steps == 2, "turn right while held");
		require(seen[2].kind == Action::Kind::EncoderSteps && seen[2].steps == -2, "turn left while held");
		require(seen[3].kind == Action::Kind::ButtonUp && seen[3].control == md::PanelControl::Trigger1, "release");
	}

	void testChannelFilter()
	{
		auto omni = makeMap(g_md, EncoderMode::RelativeOffset, 0);
		require(omni.translate(0xb0, 16, 66).has_value() && omni.translate(0xbf, 16, 66).has_value(),
			"omni accepts every channel");

		auto ch3 = makeMap(g_md, EncoderMode::RelativeOffset, 3);
		require(!ch3.translate(0xb0, 16, 66), "channel 3 rejects channel 1");
		require(ch3.translate(0xb2, 16, 66).has_value(), "channel 3 accepts channel 3");
		require(ch3.translate(0xb2, 65, 127).has_value(), "channel filter applies to buttons");
		require(!ch3.translate(0xb3, 65, 127), "channel filter rejects other button channels");
	}

	void testIgnoredMessages()
	{
		auto map = makeMap(g_md, EncoderMode::RelativeOffset);
		require(!map.translate(0xc0, 5, 0), "program change is not a panel message");
		require(!map.translate(0xe0, 0, 64), "pitch bend is not a panel message");
		require(!map.translate(0xf8, 0, 0), "realtime clock is ignored");
		require(!map.translate(0x10, 16, 66), "data byte as status is ignored");
		require(!map.translate(0xb0, 15, 66), "CC below the encoder range is unbound");
		require(!map.translate(0xb0, 127, 127), "unbound CC is ignored");
	}

	void testCustomBindings()
	{
		auto table = makeDefaultTable(g_md);
		// Play on note 60, and a knob on CC 100 for encoder C instead of CC 18.
		table.buttons[static_cast<size_t>(md::PanelControl::Play)] = { Source::Kind::Note, 60 };
		table.encoders[2].source = { Source::Kind::Controller, 100 };
		Map map;
		map.setTable(table);

		auto a = map.translate(0x90, 60, 100);
		require(a && a->control == md::PanelControl::Play && a->kind == Action::Kind::ButtonDown, "note 60 plays");
		require(!map.translate(0xb0, 18, 100), "old CC 18 no longer drives encoder C");
		require(!map.translate(0xb0, 100, 10), "CC 100 anchors");
		a = map.translate(0xb0, 100, 14);
		require(a && a->encoder == md::PanelEncoder::DataEntryC && a->steps == 4, "CC 100 drives encoder C");
	}

	void testTextRoundTrip()
	{
		for(const auto model : { g_md, g_mm })
		{
			auto table = makeDefaultTable(model);
			table.channel = 5;
			table.encoders[3].mode = EncoderMode::RelativeTwosComplement;
			table.encoders[4].source = {};	// unbound encoder stays unbound
			table.buttons[static_cast<size_t>(md::PanelControl::Play)] = { Source::Kind::Note, 99 };

			Table loaded;
			require(fromText(toText(table), model, loaded), "text parses");
			require(loaded == table, "text round trip keeps every binding");
		}

		// A table for the other machine loses the controls this one does not have.
		Table mmTable = makeDefaultTable(g_mm);
		Table asMd;
		require(fromText(toText(mmTable), g_md, asMd), "MM text parses as MD");
		require(!asMd.buttons[static_cast<size_t>(md::PanelControl::Track1)].valid(), "track binding dropped on MD");
		require(asMd.buttons[static_cast<size_t>(md::PanelControl::Play)].valid(), "shared binding kept on MD");
	}

	void testTextIsForgiving()
	{
		Table table;
		require(!fromText("", g_md, table), "empty text is not a table");
		require(!fromText("hello 1\n", g_md, table), "wrong header is rejected");
		require(!fromText("gearmulator-panel-midi 3\n", g_md, table), "unknown version is rejected");
		require(!fromText("gearmulator-panel-midi 0\n", g_md, table), "version 0 is rejected");

		const std::string text =
			"gearmulator-panel-midi 1\r\n"
			"channel 4\r\n"
			"encoder DataEntryA cc 40 relative-offset\r\n"
			"encoder DataEntryB note 40 absolute\n"		// encoders take controllers only
			"encoder Nonsense cc 41 absolute\n"		// unknown control
			"button Play cc 200\n"					// out of range
			"button Stop note 61\n"
			"garbage\n";
		require(fromText(text, g_md, table), "tolerant parse");
		require(table.channel == 4, "channel read");
		require(table.encoders[0].source == Source{ Source::Kind::Controller, 40 }
			&& table.encoders[0].mode == EncoderMode::RelativeOffset, "encoder A read");
		require(!table.encoders[1].source.valid(), "note for an encoder is skipped");
		require(!table.buttons[static_cast<size_t>(md::PanelControl::Play)].valid(), "out-of-range number skipped");
		require(table.buttons[static_cast<size_t>(md::PanelControl::Stop)] == Source{ Source::Kind::Note, 61 },
			"valid line after bad ones still read");
	}

	void testVersionOneGetsDefaultPushes()
	{
		// A file written before encoder pushes existed must not leave them unbound.
		Table table;
		require(fromText("gearmulator-panel-midi 1\nchannel 2\nencoder DataEntryA cc 1 absolute\n", g_md, table), "v1 parses");
		require(table.pushes == makeDefaultTable(g_md).pushes, "v1 loads the default pushes");
		require(table.encoders[0].source == Source{ Source::Kind::Controller, 1 }, "v1 encoder read");

		// A version 2 file is authoritative: no push lines means no pushes.
		require(fromText("gearmulator-panel-midi 2\nchannel 2\n", g_md, table), "v2 parses");
		for(const auto& push : table.pushes)
			require(!push.valid(), "v2 without push lines has no pushes");

		// Pushes survive a round trip, including one moved to a note.
		auto custom = makeDefaultTable(g_mm);
		custom.pushes[1] = { Source::Kind::Note, 91 };
		custom.pushes[5] = {};
		Table loaded;
		require(fromText(toText(custom), g_mm, loaded) && loaded == custom, "pushes round trip");
	}

	void testBindableMessages()
	{
		require(isBindableMessage(0x90) && isBindableMessage(0x9f), "note on");
		require(isBindableMessage(0x80) && isBindableMessage(0x8f), "note off");
		require(isBindableMessage(0xb0) && isBindableMessage(0xbf), "controller");
		require(!isBindableMessage(0xa0) && !isBindableMessage(0xaf), "polyphonic pressure (what pads send while held)");
		require(!isBindableMessage(0xd0), "channel pressure");
		require(!isBindableMessage(0xe0), "pitch bend");
		require(!isBindableMessage(0xc0), "program change");
		require(!isBindableMessage(0xf8) && !isBindableMessage(0xfe) && !isBindableMessage(0xf0), "system messages");
		require(!isBindableMessage(0x40), "a data byte is not a status");
	}

	bool contains(const std::string& _text, const char* _part)
	{
		return _text.find(_part) != std::string::npos;
	}

	void testChannelOverlapWarning()
	{
		// Machinedrum with base channel 1 (zero-based 0) listens on 1-4.
		for(uint8_t channel = 1; channel <= 4; ++channel)
		{
			const auto w = channelOverlapWarning(g_md, channel, 0);
			require(w.has_value(), "channels 1-4 overlap the MD");
			require(contains(*w, "Machinedrum") && contains(*w, "(1-4)") && contains(*w, "for example 16"), "names the machine, its channels and a free one");
		}
		for(uint8_t channel = 5; channel <= 16; ++channel)
			require(!channelOverlapWarning(g_md, channel, 0), "channels 5-16 are free on the MD");

		// Omni always includes them.
		const auto omni = channelOverlapWarning(g_md, 0, 0);
		require(omni && contains(*omni, "Omni") && contains(*omni, "1-4"), "omni overlaps");

		// The Monomachine listens on six channels.
		for(uint8_t channel = 1; channel <= 6; ++channel)
			require(channelOverlapWarning(g_mm, channel, 0).has_value(), "channels 1-6 overlap the MM");
		require(!channelOverlapWarning(g_mm, 7, 0), "channel 7 is free on the MM");
		require(contains(*channelOverlapWarning(g_mm, 3, 0), "Monomachine"), "names the MM");

		// A higher base channel moves the block, and 16 stops being the free one.
		require(!channelOverlapWarning(g_md, 12, 12), "MD base 13 uses 13-16, so 12 is free");
		const auto high = channelOverlapWarning(g_md, 16, 12);
		require(high && contains(*high, "(13-16)") && contains(*high, "for example 12"), "16 overlaps and 12 is suggested");
		require(!channelOverlapWarning(g_mm, 10, 10), "MM base 11 uses 11-16, so 10 is free");
		require(channelOverlapWarning(g_mm, 16, 10).has_value(), "MM base 11 reaches 16");

		// The block cannot run past channel 16.
		const auto top = channelOverlapWarning(g_md, 16, 15);
		require(top && contains(*top, "listens on (16)") && contains(*top, "for example 15"), "a block at the top is clamped to channel 16");

		// Base channel not known yet: no guess.
		require(!channelOverlapWarning(g_md, 0, 0x7f), "unknown base gives no warning");
		require(!channelOverlapWarning(g_md, 1, 0xff), "unknown base gives no warning");
		require(!channelOverlapWarning(g_md, 17, 0), "a channel out of range gives no warning");
	}

	void testDescribe()
	{
		require(describe(Source{}) == "-", "unbound source");
		require(describe(Source{ Source::Kind::Controller, 16 }) == "CC 16", "CC source");
		require(describe(Source{ Source::Kind::Note, 36 }) == "Note 36", "note source");
		require(describe(RawMessage{ 0xb0, 17, 65 }) == "CC 17 = 65 (ch 1)", "CC message");
		require(describe(RawMessage{ 0x91, 36, 100 }) == "Note On 36 vel 100 (ch 2)", "note on message");
		require(describe(RawMessage{ 0x80, 36, 0 }) == "Note Off 36 (ch 1)", "note off message");
	}
}

int main()
{
	try
	{
		testAbsoluteEncoder();
		testRelativeEncoders();
		testModesAreIndependentPerEncoder();
		testTriggerKeys();
		testButtonControllers();
		testDefaultTables();
		testEncoderPush();
		testHeldStepWhileTurning();
		testChannelFilter();
		testIgnoredMessages();
		testCustomBindings();
		testTextRoundTrip();
		testTextIsForgiving();
		testVersionOneGetsDefaultPushes();
		testBindableMessages();
		testChannelOverlapWarning();
		testDescribe();
	}
	catch(const std::exception& _e)
	{
		std::cerr << "mdPanelMidiMapTest failed: " << _e.what() << '\n';
		return 1;
	}
	std::cout << "mdPanelMidiMapTest passed\n";
	return 0;
}
