// Panel MIDI on the plugin's MIDI input: which messages are held back from the
// machine, how learning takes them, how the panel is operated with no editor
// (minimum hold, separate mouse and MIDI keys, All Notes Off), MIDI Learn
// precedence, and the project state round trip.

#include "mdPanelMidiService.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace mdJucePlugin::panelMidi;

	constexpr auto g_md = md::MachineModel::Machinedrum;

	void require(const bool _condition, const std::string& _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	// A service with no editor and no device: what it would send is recorded.
	struct Rig
	{
		md::PanelRowMixer rows;
		std::vector<md::PanelPacket> sent;
		Service service;

		Rig() : service(g_md, {}, rows, [this](const uint8_t _command, const uint8_t _argument)
		{
			sent.push_back({ _command, _argument });
			return true;
		})
		{
		}

		void learn(const md::PanelControl _control, const uint8_t _status, const uint8_t _data1)
		{
			service.getController().beginLearn(_control);
			(void)service.intercept(_status, _data1, 127);
			service.service(0.0);
			sent.clear();
		}
	};

	md::PanelPacket packetOf(const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(g_md, _control);
		require(packet.has_value(), "the test control has no packet");
		return *packet;
	}

	bool sentRow(const std::vector<md::PanelPacket>& _sent, const uint8_t _row, const uint8_t _mask)
	{
		return !_sent.empty() && _sent.back().row == _row && _sent.back().mask == _mask;
	}

	void testNothingIsTakenByDefault()
	{
		Rig rig;
		// CC 16+ on the base channel are the machine's own track parameters.
		require(!rig.service.intercept(0xb0, 16, 64), "an unbound controller was taken from the machine");
		require(!rig.service.intercept(0x90, 36, 100), "an unbound note was taken from the machine");
		require(!rig.service.intercept(0xe0, 0, 64), "pitch bend was taken");
	}

	void testLearnedButtonDrivesThePanelWithoutAnEditor()
	{
		Rig rig;
		rig.service.getController().beginLearn(md::PanelControl::Play);
		require(rig.service.intercept(0xb0, 30, 127), "the learn message reached the machine");
		rig.service.service(0.0);
		require(rig.service.getController().getTable().buttons[static_cast<size_t>(md::PanelControl::Play)]
			== Source{ Source::Kind::Controller, 30 }, "the message was not learned");
		require(rig.sent.empty(), "learning pressed the button");

		require(!rig.service.intercept(0x90, 36, 100), "notes are still taken after learning ended");
		require(!rig.service.intercept(0xb0, 31, 127), "an unlearned controller was taken");

		const auto play = packetOf(md::PanelControl::Play);
		require(rig.service.intercept(0xb0, 30, 127), "the learned controller reached the machine");
		rig.service.service(100.0);
		require(sentRow(rig.sent, play.row, play.mask), "Play was not pressed");
		require(rig.service.getDriver().isHeld(md::PanelControl::Play), "Play is not shown as held");

		require(rig.service.intercept(0xb0, 30, 0), "the release reached the machine");
		rig.service.service(200.0);
		require(sentRow(rig.sent, play.row, 0), "Play was not released");
	}

	void testNotesOffAreTakenWithTheirNote()
	{
		Rig rig;
		rig.learn(md::PanelControl::Trigger1, 0x90, 48);
		require(rig.service.intercept(0x90, 48, 0) && rig.service.intercept(0x80, 48, 0),
			"a learned note's release reached the machine");
	}

	void testShortTapIsHeldLongEnoughToBeSeen()
	{
		Rig rig;
		rig.learn(md::PanelControl::Trigger1, 0x90, 48);
		const auto trig = packetOf(md::PanelControl::Trigger1);

		rig.service.intercept(0x90, 48, 100);
		rig.service.intercept(0x80, 48, 0);
		rig.service.service(1000.0);
		require(rig.sent.size() == 1 && sentRow(rig.sent, trig.row, trig.mask), "the tap was not pressed first");

		rig.service.service(1000.0 + Driver::g_minEdgeIntervalMs - 1.0);
		require(rig.sent.size() == 1, "the release came before the minimum hold");
		rig.service.service(1000.0 + Driver::g_minEdgeIntervalMs);
		require(rig.sent.size() == 2 && sentRow(rig.sent, trig.row, 0), "the tap was never released");

		// A second tap right after the first waits for its own turn too.
		rig.service.intercept(0x90, 48, 100);
		rig.service.service(1000.0 + Driver::g_minEdgeIntervalMs + 1.0);
		require(rig.sent.size() == 2, "the next press came too soon after the release");
		rig.service.service(1000.0 + 2 * Driver::g_minEdgeIntervalMs);
		require(rig.sent.size() == 3 && sentRow(rig.sent, trig.row, trig.mask), "the second tap was lost");
	}

	void testMouseAndMidiHoldKeysIndependently()
	{
		md::PanelRowMixer rows;
		auto mouse = rows.port(md::PanelInputSource::Mouse);
		auto midi = rows.port(md::PanelInputSource::Midi);
		const md::PanelPacket a{ 0x21, 0x01 };
		const md::PanelPacket b{ 0x21, 0x04 };

		require(midi.press(a).mask == 0x01, "MIDI press");
		require(mouse.press(b).mask == 0x05, "the machine does not see both keys");
		require(mouse.press(a).mask == 0x05, "a shared key");
		require(mouse.releaseRow(0x21).mask == 0x01, "the mouse letting go released the MIDI key");
		require(mouse.mask(0x21) == 0 && midi.mask(0x21) == 0x01, "per-source masks");
		require(midi.release(a).mask == 0, "the key stayed down");
		// Packets outside the scan rows pass through as with PanelRowState.
		require(mouse.press({ 0x10, 0x7f }).row == 0x10 && mouse.press({ 0x10, 0x7f }).mask == 0x7f,
			"a non-row packet was changed");
	}

	void testAllNotesOffReleasesHeldKeys()
	{
		Rig rig;
		rig.learn(md::PanelControl::Play, 0xb0, 30);
		rig.service.intercept(0xb0, 30, 127);
		rig.service.service(0.0);
		require(rig.service.getDriver().isHeld(md::PanelControl::Play), "Play was not held");

		require(!rig.service.intercept(0xb0, 123, 0), "All Notes Off was kept from the machine");
		rig.service.service(1.0);
		require(!rig.service.getDriver().isHeld(md::PanelControl::Play), "All Notes Off left Play held");
		require(sentRow(rig.sent, packetOf(md::PanelControl::Play).row, 0), "the release was not sent");

		rig.service.intercept(0xb0, 30, 127);
		rig.service.service(100.0);
		rig.service.requestReleaseAll();	// as after loading a project
		rig.service.service(101.0);
		require(!rig.service.getDriver().isHeld(md::PanelControl::Play), "a project load left Play held");
	}

	void testMidiLearnMappingsComeFirst()
	{
		Rig rig;
		rig.service.getController().beginLearn(md::PanelControl::Play);
		require(!rig.service.intercept(0xb0, 30, 127, true), "a MIDI Learn mapping was taken by panel learn");
		rig.service.service(0.0);
		require(!rig.service.getController().getTable().buttons[static_cast<size_t>(md::PanelControl::Play)].valid(),
			"panel learn bound a message MIDI Learn has mapped");
		require(rig.service.getController().wasLastMessageReserved(), "the refusal is not shown");

		require(rig.service.intercept(0xb0, 31, 127), "an unmapped message was not learned");
		rig.service.service(1.0);
		require(rig.service.intercept(0xb0, 31, 127), "the binding does not take its message");
		require(!rig.service.intercept(0xb0, 31, 127, true), "a binding took a message MIDI Learn has mapped since");
	}

	void testEncoderSteps()
	{
		Rig rig;
		std::vector<int> turns;
		rig.service.getDriver().setEncoderTurnedHandler([&](const md::PanelEncoder _encoder, const int _steps)
		{
			require(_encoder == md::PanelEncoder::DataEntryA, "wrong encoder reported");
			turns.push_back(_steps);
		});
		auto& controller = rig.service.getController();
		controller.beginLearn(md::PanelEncoder::DataEntryA);
		rig.service.intercept(0xb0, 20, 1);
		rig.service.service(0.0);
		controller.setEncoderMode(md::PanelEncoder::DataEntryA, EncoderMode::RelativeTwosComplement);
		rig.sent.clear();

		const auto command = md::panelEncoderCommand(g_md, md::PanelEncoder::DataEntryA);
		require(command.has_value(), "encoder A has no command");

		rig.service.intercept(0xb0, 20, 3);
		rig.service.service(1.0);
		require(rig.sent.size() == 3, "a turn of 3 did not send 3 detents");
		for(const auto& packet : rig.sent)
			require(packet.row == *command && packet.mask == 0x01, "wrong detent packet");

		rig.sent.clear();
		rig.service.intercept(0xb0, 20, 128 - 20);	// -20
		rig.service.service(2.0);
		require(rig.sent.size() == static_cast<size_t>(Driver::g_encoderBurstCap)
			&& rig.sent.back().mask == 0xff, "a large turn was not capped like a mouse move");
		require(turns.size() == 2 && turns[0] == 3 && turns[1] == -Driver::g_encoderBurstCap,
			"the editor was not told about the turns");
	}

	void testChannelFilter()
	{
		Rig rig;
		rig.service.getController().setChannel(16);
		rig.service.getController().beginLearn(md::PanelControl::Play);
		require(!rig.service.intercept(0xb0, 30, 127), "learning took a message from another channel");
		require(rig.service.intercept(0xbf, 30, 127), "learning ignored its own channel");
		rig.service.service(0.0);
		require(!rig.service.intercept(0xb0, 30, 127), "the binding took channel 1");
		require(rig.service.intercept(0xbf, 30, 127), "the binding missed channel 16");
		require(!rig.service.intercept(0xb0, 123, 0), "All Notes Off on another channel");
	}

	void testStarterMapStaysOffTheMachinesChannels()
	{
		Rig rig;
		const auto channel = suggestPanelChannel(g_md, 0);
		require(channel == 16, "the suggested channel for base channel 1 is not 16");
		require(suggestPanelChannel(g_md, 12) == 12, "channels 13-16 are the machine's");
		require(suggestPanelChannel(g_md, 255) == 16, "unknown base channel");

		rig.service.getController().loadStarterMap(channel);
		rig.service.service(0.0);
		require(!rig.service.intercept(0xb0, g_firstEncoderController, 64),
			"the starter map took the machine's controller on channel 1");
		require(rig.service.intercept(0xbf, g_firstEncoderController, 64), "the starter map is not on channel 16");
	}

	void testProjectState()
	{
		std::string state;
		{
			Rig rig;
			rig.service.getController().setChannel(5);
			rig.service.getController().beginLearn(md::PanelControl::Play);
			rig.service.intercept(0x94, 50, 100);
			rig.service.service(0.0);
			state = rig.service.getProjectState();
		}
		Rig restored;
		require(!restored.service.intercept(0x94, 50, 100), "a fresh instance already had the binding");
		restored.service.setProjectState(state);
		restored.service.service(0.0);
		require(restored.service.getController().getTable().channel == 5, "channel not restored");
		require(restored.service.intercept(0x94, 50, 100), "the restored binding does not filter");

		// A project saved with nothing bound clears what an instance started with.
		restored.service.setProjectState(toText(Table()));
		restored.service.service(1.0);
		require(!restored.service.intercept(0x94, 50, 100), "loading an empty project kept the old binding");
	}
}

int main()
{
	const juce::ScopedJuceInitialiser_GUI juceInit;	// for the service's timer
	try
	{
		testNothingIsTakenByDefault();
		testLearnedButtonDrivesThePanelWithoutAnEditor();
		testNotesOffAreTakenWithTheirNote();
		testShortTapIsHeldLongEnoughToBeSeen();
		testMouseAndMidiHoldKeysIndependently();
		testAllNotesOffReleasesHeldKeys();
		testMidiLearnMappingsComeFirst();
		testEncoderSteps();
		testChannelFilter();
		testStarterMapStaysOffTheMachinesChannels();
		testProjectState();
	}
	catch(const std::exception& _e)
	{
		std::cerr << "FAIL: " << _e.what() << '\n';
		return 1;
	}
	std::cout << "mdPanelMidiServiceTest: PASS\n";
	return 0;
}
