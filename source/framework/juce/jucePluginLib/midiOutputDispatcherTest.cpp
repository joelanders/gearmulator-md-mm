#include "midiports.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
	using namespace std::chrono_literals;

	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	struct SinkState
	{
		std::mutex mutex;
		std::condition_variable condition;
		bool running = false;
		bool stopped = false;
		bool block = false;
		bool released = false;
		size_t entered = 0;
		std::vector<std::array<uint8_t, 3>> messages;
	};

	class ControlledSink final : public pluginLib::MidiOutputSink
	{
	public:
		ControlledSink(std::shared_ptr<SinkState> _state, juce::String _identifier)
			: state(std::move(_state)), identifier(std::move(_identifier)) {}

		juce::String getIdentifier() const override { return identifier; }
		void start() override
		{
			const std::lock_guard lock(state->mutex);
			state->running = true;
		}
		void stop() override
		{
			const std::lock_guard lock(state->mutex);
			state->running = false;
			state->stopped = true;
		}
		bool isRunning() const override
		{
			const std::lock_guard lock(state->mutex);
			return state->running;
		}
		void sendMessageNow(const juce::MidiMessage& _message) override
		{
			std::unique_lock lock(state->mutex);
			++state->entered;
			state->condition.notify_all();
			state->condition.wait(lock, [this]
			{
				return !state->block || state->released;
			});
			std::array<uint8_t, 3> bytes{};
			const auto count = std::min(3, _message.getRawDataSize());
			std::copy_n(_message.getRawData(), count, bytes.begin());
			state->messages.push_back(bytes);
			state->condition.notify_all();
		}

	private:
		std::shared_ptr<SinkState> state;
		juce::String identifier;
	};

	std::unique_ptr<ControlledSink> sink(const std::shared_ptr<SinkState>& _state,
		const char* const _identifier)
	{
		return std::make_unique<ControlledSink>(_state, _identifier);
	}

	bool waitFor(const std::shared_ptr<SinkState>& _state,
		const std::function<bool(const SinkState&)>& _predicate)
	{
		std::unique_lock lock(_state->mutex);
		return _state->condition.wait_for(lock, 3s,
			[&] { return _predicate(*_state); });
	}

	void release(const std::shared_ptr<SinkState>& _state)
	{
		{
			const std::lock_guard lock(_state->mutex);
			_state->released = true;
		}
		_state->condition.notify_all();
	}

	juce::MidiMessage message(const int _ordinal)
	{
		return juce::MidiMessage::controllerEvent(
			1 + (_ordinal / 128) % 16, _ordinal % 128, (_ordinal * 37) % 128);
	}

	void testSaturationAndBlockingSend()
	{
		pluginLib::MidiOutputDispatcher dispatcher;
		auto state = std::make_shared<SinkState>();
		state->block = true;
		require(dispatcher.setOutput(sink(state, "saturated")),
			"controlled output did not open");
		require(dispatcher.trySend(message(0)), "first realtime send failed");
		require(waitFor(state, [](const SinkState& value) { return value.entered == 1; }),
			"sender did not enter controlled sink");

		for(size_t index = 0; index < pluginLib::MidiOutputDispatcher::Capacity; ++index)
			require(dispatcher.trySend(message(static_cast<int>(index + 1))),
				"realtime send failed before fixed queue reached capacity");
		require(!dispatcher.trySend(message(1000)),
			"realtime send did not report fixed-capacity saturation");

		auto blocked = std::async(std::launch::async, [&dispatcher]
		{
			dispatcher.send(message(1001));
		});
		const auto status = blocked.wait_for(30ms);
		release(state);
		require(status == std::future_status::timeout,
			"non-realtime send did not wait behind a full queue");
		require(blocked.wait_for(3s) == std::future_status::ready,
			"non-realtime send did not resume after capacity became available");
		require(waitFor(state, [](const SinkState& value)
		{
			return value.messages.size()
				== pluginLib::MidiOutputDispatcher::Capacity + 2;
		}), "queued MIDI messages did not drain completely");
		dispatcher.close();
		require(dispatcher.trySend(message(2)),
			"an absent physical output should remain a successful no-op");
	}

	void testReplacementAndShutdown()
	{
		pluginLib::MidiOutputDispatcher dispatcher;
		auto oldState = std::make_shared<SinkState>();
		oldState->block = true;
		dispatcher.setOutput(sink(oldState, "old"));
		dispatcher.send(message(10));
		require(waitFor(oldState,
			[](const SinkState& value) { return value.entered == 1; }),
			"old sink did not begin its send");

		auto newState = std::make_shared<SinkState>();
		auto replacement = std::async(std::launch::async,
			[&dispatcher, output = sink(newState, "new")]() mutable
			{
				return dispatcher.setOutput(std::move(output));
			});
		const auto replacementStatus = replacement.wait_for(30ms);
		release(oldState);
		require(replacementStatus == std::future_status::timeout,
			"output replacement destroyed a sink with an active send");
		require(replacement.wait_for(3s) == std::future_status::ready
			&& replacement.get(), "output replacement did not complete");
		require(dispatcher.getOutputId() == "new", "replacement identifier is stale");
		dispatcher.send(message(11));
		require(waitFor(newState,
			[](const SinkState& value) { return value.messages.size() == 1; }),
			"replacement sink did not receive subsequent output");
		{
			const std::lock_guard lock(oldState->mutex);
			require(oldState->stopped && oldState->messages.size() == 1,
				"old sink lifecycle or delivery boundary was incorrect");
		}

		newState->block = true;
		newState->released = false;
		dispatcher.send(message(12));
		require(waitFor(newState,
			[](const SinkState& value) { return value.entered == 2; }),
			"shutdown probe did not enter replacement sink");
		auto shutdown = std::async(std::launch::async,
			[&dispatcher] { dispatcher.close(); });
		const auto shutdownStatus = shutdown.wait_for(30ms);
		release(newState);
		require(shutdownStatus == std::future_status::timeout,
			"shutdown destroyed a sink with an active send");
		require(shutdown.wait_for(3s) == std::future_status::ready,
			"shutdown did not complete after active output returned");
		const std::lock_guard lock(newState->mutex);
		require(newState->stopped && !newState->running,
			"shutdown did not stop the physical output sink");
	}

	void testConcurrentProducers()
	{
		pluginLib::MidiOutputDispatcher dispatcher;
		auto state = std::make_shared<SinkState>();
		state->block = true;
		dispatcher.setOutput(sink(state, "concurrent"));
		require(dispatcher.trySend(message(2000)), "sentinel send failed");
		require(waitFor(state,
			[](const SinkState& value) { return value.entered == 1; }),
			"sentinel did not block the consumer");

		constexpr int ProducerCount = 4;
		constexpr int MessagesPerProducer = 24;
		std::array<std::thread, ProducerCount> producers;
		for(int producer = 0; producer < ProducerCount; ++producer)
		{
			producers[producer] = std::thread([&dispatcher, producer]
			{
				for(int index = 0; index < MessagesPerProducer; ++index)
				{
					const auto ordinal = 3000 + producer * MessagesPerProducer + index;
					while(!dispatcher.trySend(message(ordinal)))
						std::this_thread::yield();
				}
			});
		}
		for(auto& producer : producers)
			producer.join();
		release(state);
		require(waitFor(state, [](const SinkState& value)
		{
			return value.messages.size() == 1 + ProducerCount * MessagesPerProducer;
		}), "concurrent producer messages did not all reach the sink");

		std::vector<std::array<uint8_t, 3>> delivered;
		{
			const std::lock_guard lock(state->mutex);
			delivered = state->messages;
		}
		std::sort(delivered.begin() + 1, delivered.end());
		require(std::adjacent_find(delivered.begin() + 1, delivered.end())
			== delivered.end(), "concurrent producers duplicated an output message");
		dispatcher.close();
	}
}

int main()
{
	try
	{
		testSaturationAndBlockingSend();
		testReplacementAndShutdown();
		testConcurrentProducers();
		std::cout << "midiOutputDispatcherTest: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "midiOutputDispatcherTest: " << error.what() << '\n';
		return 1;
	}
}
