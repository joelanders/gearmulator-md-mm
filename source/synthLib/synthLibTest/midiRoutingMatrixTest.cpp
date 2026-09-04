#include "synthLib/midiRoutingMatrix.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	void verifyDefaultsAndIndependentFlags()
	{
		using Source = synthLib::MidiEventSource;
		using Type = synthLib::MidiRoutingMatrix::EventType;
		synthLib::MidiRoutingMatrix matrix;

		require(matrix.enabled(Source::Host, Source::Device, Type::Note),
			"default host-to-device note route is disabled");
		require(!matrix.enabled(Source::Host, Source::Physical, Type::Note),
			"unexpected default host-to-physical note route");

		matrix.setEnabled(Source::Host, Source::Device, Type::Note, false);
		require(!matrix.enabled(Source::Host, Source::Device, Type::Note),
			"note route was not disabled");
		require(matrix.enabled(Source::Host, Source::Device, Type::Controller),
			"disabling notes also disabled controller routing");
	}

	void verifyCopiesRetainPublishedRoutes()
	{
		using Source = synthLib::MidiEventSource;
		using Type = synthLib::MidiRoutingMatrix::EventType;
		synthLib::MidiRoutingMatrix source;
		source.setEnabled(Source::Editor, Source::Host, Type::PitchBend, false);

		const synthLib::MidiRoutingMatrix copied(source);
		require(copied == source, "copy construction changed MIDI routes");

		synthLib::MidiRoutingMatrix assigned;
		assigned = source;
		require(assigned == source, "copy assignment changed MIDI routes");

		synthLib::MidiRoutingMatrix moved(std::move(source));
		require(moved == copied, "move construction changed MIDI routes");
		synthLib::MidiRoutingMatrix moveAssigned;
		moveAssigned = std::move(moved);
		require(moveAssigned == copied, "move assignment changed MIDI routes");
	}

	void verifyConcurrentFlagUpdatesDoNotLoseBits()
	{
		using Source = synthLib::MidiEventSource;
		using Type = synthLib::MidiRoutingMatrix::EventType;
		synthLib::MidiRoutingMatrix matrix;
		constexpr auto source = Source::Host;
		constexpr auto destination = Source::Physical;

		for(size_t repetition = 0; repetition < 100; ++repetition)
		{
			matrix.setEnabled(source, destination, Type::All, false);
			std::atomic<bool> start{false};
			std::thread noteWriter([&]
			{
				while(!start.load(std::memory_order_acquire))
				{
				}
				matrix.setEnabled(source, destination, Type::Note, true);
			});
			std::thread controllerWriter([&]
			{
				while(!start.load(std::memory_order_acquire))
				{
				}
				matrix.setEnabled(source, destination, Type::Controller, true);
			});
			start.store(true, std::memory_order_release);
			noteWriter.join();
			controllerWriter.join();
			require(matrix.enabled(source, destination, Type::Note)
				&& matrix.enabled(source, destination, Type::Controller),
				"concurrent route updates lost an event-type bit");
		}
	}
}

int main()
{
	try
	{
		verifyDefaultsAndIndependentFlags();
		verifyCopiesRetainPublishedRoutes();
		verifyConcurrentFlagUpdatesDoNotLoseBits();
		std::cout << "midiRoutingMatrixTest: PASS\n";
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::cerr << "midiRoutingMatrixTest: " << _error.what() << '\n';
		return 1;
	}
}
