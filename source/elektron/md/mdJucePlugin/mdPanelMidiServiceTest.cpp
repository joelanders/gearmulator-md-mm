// Panel MIDI on the plugin's MIDI input: which messages are held back from the
// machine, how learning takes them, where actions go with and without an
// editor, and the project state round trip.

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

	void testNothingIsTakenByDefault()
	{
		Service service(g_md, {});
		// CC 16+ on the base channel are the machine's own track parameters.
		require(!service.intercept(0xb0, 16, 64), "an unbound controller was taken from the machine");
		require(!service.intercept(0x90, 36, 100), "an unbound note was taken from the machine");
		require(!service.intercept(0xe0, 0, 64), "pitch bend was taken");
	}

	void testLearnTakesTheMessageAndBindsIt()
	{
		Service service(g_md, {});
		std::vector<Action> actions;
		service.setActionHandler([&](const Action& _action) { actions.push_back(_action); });

		service.getController().beginLearn(md::PanelControl::Play);
		require(service.intercept(0xb0, 30, 127), "the learn message reached the machine");
		service.service();
		require(service.getController().getTable().buttons[static_cast<size_t>(md::PanelControl::Play)]
			== Source{ Source::Kind::Controller, 30 }, "the message was not learned");
		require(actions.empty(), "learning pressed the button");

		require(!service.intercept(0x90, 36, 100), "notes are still taken after learning ended");
		require(service.intercept(0xb0, 30, 127) && service.intercept(0xb0, 30, 0),
			"the learned controller still reaches the machine");
		require(!service.intercept(0xb0, 31, 127), "an unlearned controller was taken");
		service.service();
		require(actions.size() == 2 && actions[0].kind == Action::Kind::ButtonDown
			&& actions[1].kind == Action::Kind::ButtonUp && actions[0].control == md::PanelControl::Play,
			"the learned controller did not press and release Play");
	}

	void testNotesOffAreTakenWithTheirNote()
	{
		Service service(g_md, {});
		service.getController().beginLearn(md::PanelControl::Trigger1);
		service.intercept(0x90, 48, 100);
		service.service();
		require(service.intercept(0x90, 48, 0) && service.intercept(0x80, 48, 0),
			"a learned note's release reached the machine");
	}

	void testNoEditorDropsActionsButKeepsFiltering()
	{
		Service service(g_md, {});
		std::vector<Action> actions;
		service.setActionHandler([&](const Action& _action) { actions.push_back(_action); });
		service.getController().beginLearn(md::PanelControl::Stop);
		service.intercept(0xb0, 40, 127);
		service.service();

		service.setActionHandler({});	// editor closed
		require(service.intercept(0xb0, 40, 127), "a learned message reached the machine with the editor closed");
		service.service();
		require(actions.empty(), "an action was delivered with no editor");

		service.setActionHandler([&](const Action& _action) { actions.push_back(_action); });	// reopened
		service.intercept(0xb0, 40, 0);
		service.service();
		require(actions.size() == 1, "actions did not resume after the editor reattached");
	}

	void testChannelFilter()
	{
		Service service(g_md, {});
		service.getController().setChannel(16);
		service.getController().beginLearn(md::PanelControl::Play);
		require(!service.intercept(0xb0, 30, 127), "learning took a message from another channel");
		require(service.intercept(0xbf, 30, 127), "learning ignored its own channel");
		service.service();
		require(!service.intercept(0xb0, 30, 127), "the binding took channel 1");
		require(service.intercept(0xbf, 30, 127), "the binding missed channel 16");
	}

	void testProjectState()
	{
		std::string state;
		{
			Service service(g_md, {});
			service.getController().setChannel(5);
			service.getController().beginLearn(md::PanelControl::Play);
			service.intercept(0x94, 50, 100);
			service.service();
			state = service.getProjectState();
		}
		Service restored(g_md, {});
		require(!restored.intercept(0x94, 50, 100), "a fresh instance already had the binding");
		restored.setProjectState(state);
		restored.service();
		require(restored.getController().getTable().channel == 5, "channel not restored");
		require(restored.intercept(0x94, 50, 100), "the restored binding does not filter");

		// A project saved with nothing bound clears what an instance started with.
		restored.setProjectState(toText(Table()));
		restored.service();
		require(!restored.intercept(0x94, 50, 100), "loading an empty project kept the old binding");
	}
}

int main()
{
	const juce::ScopedJuceInitialiser_GUI juceInit;	// for the service's timer
	try
	{
		testNothingIsTakenByDefault();
		testLearnTakesTheMessageAndBindsIt();
		testNotesOffAreTakenWithTheirNote();
		testNoEditorDropsActionsButKeepsFiltering();
		testChannelFilter();
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
