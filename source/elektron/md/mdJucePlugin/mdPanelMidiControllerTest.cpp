#include "mdPanelMidiController.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
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

	struct Fixture
	{
		explicit Fixture(const md::MachineModel _model, const std::string& _file = {})
			: controller(_model, _file, [this](const Action& _action) { actions.push_back(_action); })
		{
		}

		void send(const uint8_t _status, const uint8_t _d1, const uint8_t _d2)
		{
			controller.handleMessage(RawMessage{ _status, _d1, _d2 });
		}

		std::vector<Action> actions;
		Controller controller;
	};

	std::string tempFile(const std::string& _name)
	{
		// std::filesystem needs macOS 10.15, above the project's deployment target.
		for(const auto* const variable : { "TMPDIR", "TEMP", "TMP" })
			if(const auto* const folder = std::getenv(variable))
			{
				std::string path = folder;
				if(!path.empty() && path.back() != '/' && path.back() != '\\')
					path += '/';
				return path + "mdPanelMidiControllerTest_" + _name + ".txt";
			}
		return "mdPanelMidiControllerTest_" + _name + ".txt";
	}

	Source buttonSource(const Controller& _c, const md::PanelControl _control)
	{
		return _c.getTable().buttons[static_cast<size_t>(_control)];
	}

	Source encoderSource(const Controller& _c, const md::PanelEncoder _encoder)
	{
		return _c.getTable().encoders[static_cast<size_t>(_encoder)].source;
	}

	void testDispatch()
	{
		Fixture f(g_md);
		f.send(0xb0, 65, 127);
		require(f.actions.size() == 1 && f.actions[0].kind == Action::Kind::ButtonDown
			&& f.actions[0].control == md::PanelControl::Trigger1, "CC drives trig 1");
		f.send(0xb0, 30, 127);	// unbound on the MD
		require(f.actions.size() == 1, "unbound message does nothing");
		require(f.controller.getMessageSerial() == 2, "serial counts every message");
		require(describe(f.controller.getLastMessage()) == "CC 30 = 127 (ch 1)", "last message is kept");
	}

	void testLearnButtonFromNote()
	{
		Fixture f(g_md);
		f.controller.beginLearn(md::PanelControl::Play);
		require(f.controller.getLearnTarget().is(md::PanelControl::Play), "learning Play");

		f.send(0x80, 70, 0);	// a note-off cannot be learned
		require(f.controller.getLearnTarget().active(), "note-off keeps waiting");
		f.send(0x90, 70, 100);
		require(!f.controller.getLearnTarget().active(), "learning ends");
		require(buttonSource(f.controller, md::PanelControl::Play) == Source{ Source::Kind::Note, 70 }, "Play is note 70");
		require(f.actions.empty(), "learning does not drive the panel");

		f.send(0x90, 70, 100);
		require(f.actions.size() == 1 && f.actions[0].control == md::PanelControl::Play, "the new binding works");
	}

	void testLearnButtonFromController()
	{
		Fixture f(g_md);
		f.controller.beginLearn(md::PanelControl::Stop);
		f.send(0xb0, 90, 0);	// any value: a button controller may start at 0
		require(buttonSource(f.controller, md::PanelControl::Stop) == Source{ Source::Kind::Controller, 90 }, "Stop is CC 90");
	}

	void testLearnEncoder()
	{
		Fixture f(g_md);
		f.controller.beginLearn(md::PanelEncoder::DataEntryB);
		f.send(0x90, 50, 100);	// a note cannot drive an encoder
		require(f.controller.getLearnTarget().is(md::PanelEncoder::DataEntryB), "note ignored while learning an encoder");
		f.send(0xb0, 77, 20);
		require(encoderSource(f.controller, md::PanelEncoder::DataEntryB) == Source{ Source::Kind::Controller, 77 }, "encoder B is CC 77");
		require(f.controller.getTable().encoders[1].mode == EncoderMode::Absolute, "the mode is kept");
	}

	void testLearnMovesAnExistingSource()
	{
		Fixture f(g_md);
		// Trig 2 is CC 66 by default; learning trig 1 onto CC 66 takes it over.
		f.controller.beginLearn(md::PanelControl::Trigger1);
		f.send(0xb0, 66, 127);
		require(buttonSource(f.controller, md::PanelControl::Trigger1) == Source{ Source::Kind::Controller, 66 }, "trig 1 got CC 66");
		require(!buttonSource(f.controller, md::PanelControl::Trigger2).valid(), "trig 2 lost it");

		// A controller used by an encoder moves to a button too.
		f.controller.beginLearn(md::PanelControl::Play);
		f.send(0xb0, 16, 127);
		require(buttonSource(f.controller, md::PanelControl::Play) == Source{ Source::Kind::Controller, 16 }, "Play got CC 16");
		require(!encoderSource(f.controller, md::PanelEncoder::DataEntryA).valid(), "encoder A lost CC 16");
	}

	void testLearnPush()
	{
		Fixture f(g_md);
		f.controller.beginLearnPush(md::PanelEncoder::DataEntryB);
		require(f.controller.getLearnTarget().isPush(md::PanelEncoder::DataEntryB), "learning B's push");
		require(!f.controller.getLearnTarget().is(md::PanelEncoder::DataEntryB), "not B's turning");

		f.send(0x80, 50, 0);	// a note-off cannot be learned
		require(f.controller.getLearnTarget().active(), "note-off keeps waiting");
		f.send(0xb0, 80, 127);
		require(!f.controller.getLearnTarget().active(), "learning ends");
		require(f.controller.getTable().pushes[1] == Source{ Source::Kind::Controller, 80 }, "B's push is CC 80");
		require(f.controller.getTable().encoders[1].source == Source{ Source::Kind::Controller, 17 }, "turning B is untouched");
		require(f.actions.empty(), "learning does not drive the panel");

		f.send(0xb0, 80, 127);
		require(f.actions.size() == 1 && f.actions[0].kind == Action::Kind::EncoderPushDown
			&& f.actions[0].encoder == md::PanelEncoder::DataEntryB, "the new push works");
		f.send(0xb0, 80, 0);
		require(f.actions.size() == 2 && f.actions[1].kind == Action::Kind::EncoderPushUp, "and lets go");

		// A note works too.
		f.controller.beginLearnPush(md::PanelEncoder::DataEntryC);
		f.send(0x90, 70, 100);
		require(f.controller.getTable().pushes[2] == Source{ Source::Kind::Note, 70 }, "C's push is note 70");

		// A source another control had moves to the push.
		f.controller.beginLearnPush(md::PanelEncoder::DataEntryE);
		f.send(0xb0, 65, 127);	// trig 1's CC
		require(f.controller.getTable().pushes[4] == Source{ Source::Kind::Controller, 65 }, "E's push is CC 65");
		require(!buttonSource(f.controller, md::PanelControl::Trigger1).valid(), "trig 1 lost CC 65");

		// A controller used by an encoder's turning moves to the push.
		f.controller.beginLearnPush(md::PanelEncoder::DataEntryD);
		f.send(0xb0, 16, 127);
		require(f.controller.getTable().pushes[3] == Source{ Source::Kind::Controller, 16 }, "D's push is CC 16");
		require(!encoderSource(f.controller, md::PanelEncoder::DataEntryA).valid(), "encoder A lost CC 16");

		f.controller.clearPush(md::PanelEncoder::DataEntryD);
		require(!f.controller.getTable().pushes[3].valid(), "push cleared");

		// Level and Sound selection cannot be pressed.
		f.controller.beginLearnPush(md::PanelEncoder::Level);
		require(!f.controller.getLearnTarget().active(), "Level has no push switch");
		f.controller.beginLearnPush(md::PanelEncoder::SoundSelection);
		require(!f.controller.getLearnTarget().active(), "Sound selection has no push switch");
	}

	void testHeldStepWhileTurning()
	{
		// p-lock gesture end to end: hold a trig, turn an encoder, release.
		Fixture f(g_md);
		f.controller.setEncoderMode(md::PanelEncoder::DataEntryA, EncoderMode::RelativeOffset);
		f.send(0xb0, 65, 127);
		f.send(0xb0, 16, 65);
		f.send(0xb0, 57, 127);	// and press A while at it
		f.send(0xb0, 57, 0);
		f.send(0xb0, 65, 0);
		require(f.actions.size() == 5, "five actions");
		require(f.actions[0].kind == Action::Kind::ButtonDown, "step held first");
		require(f.actions[1].kind == Action::Kind::EncoderSteps && f.actions[1].steps == 1, "turned while held");
		require(f.actions[2].kind == Action::Kind::EncoderPushDown, "pushed while held");
		require(f.actions[3].kind == Action::Kind::EncoderPushUp, "push released");
		require(f.actions[4].kind == Action::Kind::ButtonUp, "step released last");
	}

	using Target = Controller::LearnTarget;

	Target trigTarget(const int _index)
	{
		Target t;
		t.kind = Target::Kind::Button;
		t.control = static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Trigger1) + _index);
		return t;
	}

	Target encoderTarget(const md::PanelEncoder _encoder)
	{
		Target t;
		t.kind = Target::Kind::Encoder;
		t.encoder = _encoder;
		return t;
	}

	std::vector<Target> allTrigs()
	{
		std::vector<Target> targets;
		for(int i = 0; i < 16; ++i)
			targets.push_back(trigTarget(i));
		return targets;
	}

	void testLearnSequenceWithNotes()
	{
		Fixture f(g_md);
		f.controller.beginLearnSequence(allTrigs());
		require(f.controller.isLearningSequence() && f.controller.getSequenceSize() == 16, "a run of 16");
		require(f.controller.getLearnTarget().is(md::PanelControl::Trigger1), "starts with trig 1");

		// 16 pads, each pressed and released like a real pad: note-on, note-off.
		for(int i = 0; i < 16; ++i)
		{
			require(f.controller.getLearnTarget().is(static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Trigger1) + i)),
				"the current control moves along the list");
			require(f.controller.getSequenceIndex() == static_cast<size_t>(i), "progress counts");
			f.send(0x90, static_cast<uint8_t>(81 + i), 100);
			f.send(0x80, static_cast<uint8_t>(81 + i), 0);	// releasing must not advance
		}

		require(!f.controller.isLearningSequence() && !f.controller.getLearnTarget().active(), "the run ends by itself");
		for(int i = 0; i < 16; ++i)
			require(buttonSource(f.controller, static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Trigger1) + i))
				== Source{ Source::Kind::Note, static_cast<uint8_t>(81 + i) }, "each trig has its own pad");
		// Nothing was pressed on the panel while learning. Only the last pad's note-off
		// arrives after the run has ended: a release for a key that is not down, which
		// the editor ignores.
		for(const auto& action : f.actions)
			require(action.kind == Action::Kind::ButtonUp && action.control == md::PanelControl::Trigger16,
				"learning did not press anything on the panel");
		f.actions.clear();

		f.send(0x90, 88, 100);
		require(f.actions.size() == 1 && f.actions[0].control == md::PanelControl::Trigger8, "pad 88 plays trig 8 now");
	}

	void testLearnSequenceIgnoresRepeatsAndReleases()
	{
		Fixture f(g_md);
		f.controller.beginLearnSequence({ trigTarget(0), trigTarget(1), trigTarget(2) });

		// Pads that send a CC: 127 when pressed, 0 when released.
		f.send(0xb0, 40, 127);	// trig 1
		f.send(0xb0, 40, 0);	// its release: must not learn "CC 40" again for trig 2
		require(f.controller.getSequenceIndex() == 1, "the release did not advance the run");
		f.send(0xb0, 40, 127);	// pressing the same pad again is not a new pad
		require(f.controller.getSequenceIndex() == 1, "the same pad twice does not advance");
		f.send(0xb0, 41, 127);	// trig 2
		f.send(0xb0, 41, 0);
		f.send(0xb0, 42, 127);	// trig 3
		require(!f.controller.isLearningSequence(), "finished");
		require(buttonSource(f.controller, md::PanelControl::Trigger1) == Source{ Source::Kind::Controller, 40 }, "trig 1 kept CC 40");
		require(buttonSource(f.controller, md::PanelControl::Trigger2) == Source{ Source::Kind::Controller, 41 }, "trig 2 is CC 41");
		require(buttonSource(f.controller, md::PanelControl::Trigger3) == Source{ Source::Kind::Controller, 42 }, "trig 3 is CC 42");
	}

	void testLearnSequenceOfEncoders()
	{
		Fixture f(g_md);
		std::vector<Target> targets;
		for(size_t i = 0; i < 4; ++i)
			targets.push_back(encoderTarget(static_cast<md::PanelEncoder>(i)));
		f.controller.beginLearnSequence(targets);

		// A knob keeps sending while it is turned; only the first message counts.
		for(int i = 0; i < 6; ++i) f.send(0xb0, 70, static_cast<uint8_t>(10 + i));
		require(f.controller.getSequenceIndex() == 1, "turning one knob advances the run once");
		f.send(0x90, 60, 100);	// a note cannot be an encoder
		require(f.controller.getSequenceIndex() == 1, "notes are ignored while learning encoders");
		for(int i = 0; i < 6; ++i) f.send(0xb0, 71, static_cast<uint8_t>(50 + i));
		for(int i = 0; i < 3; ++i) f.send(0xb0, 72, static_cast<uint8_t>(90 + i));
		for(int i = 0; i < 3; ++i) f.send(0xb0, 73, static_cast<uint8_t>(20 + i));
		require(!f.controller.isLearningSequence(), "four knobs learned");
		for(size_t i = 0; i < 4; ++i)
			require(encoderSource(f.controller, static_cast<md::PanelEncoder>(i))
				== Source{ Source::Kind::Controller, static_cast<uint8_t>(70 + i) }, "knob order kept");
	}

	void testLearnSequenceCanBeStopped()
	{
		Fixture f(g_md);
		f.controller.beginLearnSequence(allTrigs());
		f.send(0x90, 90, 100);
		f.send(0x90, 91, 100);
		f.controller.cancelLearn();
		require(!f.controller.isLearningSequence() && !f.controller.getLearnTarget().active(), "stopped");
		require(buttonSource(f.controller, md::PanelControl::Trigger2) == Source{ Source::Kind::Note, 91 }, "what was learned is kept");
		require(buttonSource(f.controller, md::PanelControl::Trigger3) == Source{ Source::Kind::Controller, 67 },
			"the rest keeps its old binding (CC 67)");

		// A single Learn during a run replaces the run.
		f.controller.beginLearnSequence(allTrigs());
		f.controller.beginLearn(md::PanelControl::Play);
		require(!f.controller.isLearningSequence() && f.controller.getLearnTarget().is(md::PanelControl::Play),
			"a single learn ends the run");
		f.controller.cancelLearn();

		// Reset ends a run too.
		f.controller.beginLearnSequence(allTrigs());
		f.controller.resetToDefault();
		require(!f.controller.isLearningSequence(), "reset ends the run");
	}

	void testLearnSequenceSkipsControlsTheMachineLacks()
	{
		Fixture md(g_md);
		Target track;
		track.kind = Target::Kind::Button;
		track.control = md::PanelControl::Track1;
		md.controller.beginLearnSequence({ track, trigTarget(0) });
		require(md.controller.getSequenceSize() == 1 && md.controller.getLearnTarget().is(md::PanelControl::Trigger1),
			"the MD has no track buttons, so only the trig is left");

		Fixture mm(g_mm);
		mm.controller.beginLearnSequence({ track, trigTarget(0) });
		require(mm.controller.getSequenceSize() == 2 && mm.controller.getLearnTarget().is(md::PanelControl::Track1),
			"the MM keeps both");

		Fixture none(g_md);
		none.controller.beginLearnSequence({ track });
		require(!none.controller.isLearningSequence(), "a list of nothing available starts nothing");
	}

	void testFilteredMessagesAreFlagged()
	{
		Fixture f(g_md);
		f.controller.setChannel(5);
		f.send(0xb0, 20, 66);	// channel 1
		require(f.controller.wasLastMessageFiltered(), "channel 1 is filtered when the filter is 5");
		f.send(0xb4, 20, 66);	// channel 5
		require(!f.controller.wasLastMessageFiltered(), "channel 5 passes");
		f.send(0xf8, 0, 0);		// a clock tick is not a channel message
		require(!f.controller.wasLastMessageFiltered(), "system messages are not called filtered");
		f.controller.setChannel(0);
		f.send(0xb0, 20, 66);
		require(!f.controller.wasLastMessageFiltered(), "omni filters nothing");
	}

	void testPressureIsIgnored()
	{
		Fixture f(g_md);
		f.send(0xb0, 20, 66);
		const auto serial = f.controller.getMessageSerial();
		const auto last = describe(f.controller.getLastMessage());

		// A pad held down keeps sending pressure: it must not appear as the last message.
		f.send(0xa0, 21, 72);
		f.send(0xd0, 72, 0);
		f.send(0xe0, 0, 64);
		f.send(0xc0, 5, 0);
		require(f.controller.getMessageSerial() == serial, "pressure and the like do not count as messages");
		require(describe(f.controller.getLastMessage()) == last, "the last message is unchanged");

		// And they do not disturb Learn.
		f.controller.beginLearn(md::PanelControl::Play);
		f.send(0xa0, 21, 72);
		require(f.controller.getLearnTarget().is(md::PanelControl::Play), "still waiting");
		f.send(0x90, 90, 100);
		require(buttonSource(f.controller, md::PanelControl::Play) == Source{ Source::Kind::Note, 90 }, "the note is learned, not the pressure");
	}

	void testLearnHonoursTheChannel()
	{
		Fixture f(g_md);
		f.controller.setChannel(3);
		f.controller.beginLearn(md::PanelControl::Play);
		f.send(0x90, 70, 100);	// channel 1
		require(f.controller.getLearnTarget().active(), "another channel is not learned");
		f.send(0x92, 70, 100);	// channel 3
		require(!f.controller.getLearnTarget().active(), "the selected channel is learned");
	}

	void testCancelAndClear()
	{
		Fixture f(g_md);
		const auto revision = f.controller.getRevision();
		f.controller.beginLearn(md::PanelControl::Play);
		require(f.controller.getRevision() != revision, "learn changes the revision");
		f.controller.cancelLearn();
		require(!f.controller.getLearnTarget().active(), "cancelled");
		f.send(0xb0, 65, 127);
		require(f.actions.size() == 1, "messages drive the panel again after cancel");

		f.controller.clear(md::PanelControl::Trigger1);
		require(!buttonSource(f.controller, md::PanelControl::Trigger1).valid(), "cleared");
		f.send(0xb0, 65, 127);
		require(f.actions.size() == 1, "a cleared control no longer responds");

		f.controller.clear(md::PanelEncoder::DataEntryA);
		require(!encoderSource(f.controller, md::PanelEncoder::DataEntryA).valid(), "encoder cleared");
	}

	void testOnlyAvailableControlsCanBeLearned()
	{
		Fixture f(g_md);
		f.controller.beginLearn(md::PanelControl::Track1);	// the MD has no track buttons
		require(!f.controller.getLearnTarget().active(), "a control the machine lacks cannot be learned");

		Fixture mmFixture(g_mm);
		mmFixture.controller.beginLearn(md::PanelEncoder::SoundSelection);
		require(!mmFixture.controller.getLearnTarget().active(), "the MM has no sound selection encoder");
		mmFixture.controller.setEncoderMode(md::PanelEncoder::SoundSelection, EncoderMode::RelativeOffset);
		require(mmFixture.controller.getTable().encoders[static_cast<size_t>(md::PanelEncoder::SoundSelection)].mode
			== EncoderMode::Absolute, "its mode cannot be changed either");
	}

	void testModeAndReset()
	{
		Fixture f(g_md);
		f.controller.setEncoderMode(md::PanelEncoder::DataEntryA, EncoderMode::RelativeOffset);
		f.send(0xb0, 16, 66);
		require(f.actions.size() == 1 && f.actions[0].steps == 2, "relative mode applies at once");

		f.controller.resetToDefault();
		require(f.controller.getTable() == makeDefaultTable(g_md), "reset restores the factory map");
	}

	void testPersistence()
	{
		const auto path = tempFile("persist");
		std::remove(path.c_str());

		{
			Fixture f(g_md, path);
			f.controller.setChannel(7);
			f.controller.beginLearn(md::PanelControl::Play);
			f.send(0x96, 88, 100);	// channel 7 is status 0x96
			f.controller.setEncoderMode(md::PanelEncoder::DataEntryC, EncoderMode::RelativeTwosComplement);
			f.controller.beginLearnPush(md::PanelEncoder::DataEntryE);
			f.send(0x96, 91, 100);
		}
		{
			Fixture f(g_md, path);
			require(f.controller.getTable().channel == 7, "channel restored");
			require(buttonSource(f.controller, md::PanelControl::Play) == Source{ Source::Kind::Note, 88 }, "learned binding restored");
			require(f.controller.getTable().encoders[2].mode == EncoderMode::RelativeTwosComplement, "mode restored");
			require(f.controller.getTable().pushes[4] == Source{ Source::Kind::Note, 91 }, "learned push restored");
		}

		// A damaged file falls back to the factory map instead of leaving the panel unbound.
		{
			std::ofstream(path, std::ios::trunc) << "this is not a panel map\n";
			Fixture f(g_md, path);
			require(f.controller.getTable() == makeDefaultTable(g_md), "garbage file falls back to the default map");
		}
		{
			Fixture f(g_md, tempFile("does_not_exist"));
			require(f.controller.getTable() == makeDefaultTable(g_md), "missing file uses the default map");
		}

		std::remove(path.c_str());
	}

	void testModelsKeepSeparateFiles()
	{
		const auto path = tempFile("shared_name");
		std::remove(path.c_str());
		{
			Fixture mmFixture(g_mm, path);
			mmFixture.controller.beginLearn(md::PanelControl::Track1);
			mmFixture.send(0xb0, 99, 127);
		}
		{
			// Same file read as an MD: the MM-only Track binding is dropped.
			Fixture mdFixture(g_md, path);
			require(!buttonSource(mdFixture.controller, md::PanelControl::Track1).valid(), "MD ignores MM-only controls");
		}
		std::remove(path.c_str());
	}
}

int main()
{
	try
	{
		testDispatch();
		testLearnButtonFromNote();
		testLearnButtonFromController();
		testLearnEncoder();
		testLearnMovesAnExistingSource();
		testLearnPush();
		testHeldStepWhileTurning();
		testLearnSequenceWithNotes();
		testLearnSequenceIgnoresRepeatsAndReleases();
		testLearnSequenceOfEncoders();
		testLearnSequenceCanBeStopped();
		testLearnSequenceSkipsControlsTheMachineLacks();
		testFilteredMessagesAreFlagged();
		testPressureIsIgnored();
		testLearnHonoursTheChannel();
		testCancelAndClear();
		testOnlyAvailableControlsCanBeLearned();
		testModeAndReset();
		testPersistence();
		testModelsKeepSeparateFiles();
	}
	catch(const std::exception& _e)
	{
		std::cerr << "mdPanelMidiControllerTest failed: " << _e.what() << '\n';
		return 1;
	}
	std::cout << "mdPanelMidiControllerTest passed\n";
	return 0;
}
