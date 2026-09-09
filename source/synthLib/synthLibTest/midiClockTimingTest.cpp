#include "synthLib/device.h"
#include "synthLib/plugin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
	using namespace synthLib;
	void require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}

	class ClockProbe final : public Device
	{
	public:
		explicit ClockProbe(float rate) : Device({}), m_rate(rate) {}
		float getSamplerate() const override { return m_rate; }
		bool isValid() const override { return true; }
		bool getState(std::vector<uint8_t>&, StateType) override { return false; }
		bool setState(const std::vector<uint8_t>&, StateType) override { return false; }
		uint32_t getChannelCountIn() override { return 0; }
		uint32_t getChannelCountOut() override { return 1; }
		bool setDspClockPercent(uint32_t) override { return false; }
		uint32_t getDspClockPercent() const override { return 100; }
		uint64_t getDspClockHz() const override { return 1; }
		void process(const TAudioInputs&, const TAudioOutputs& out, size_t size,
			const std::vector<SMidiEvent>& midi, std::vector<SMidiEvent>&) override
		{
			std::fill_n(out[0], size, 0.0f);
			for(const auto& event : midi)
			{
				require(event.offset < size, "clock/transport outside callback");
				events.push_back({frame + event.offset, event.a, event.b, event.c});
			}
			frame += size;
		}
		struct Event { uint64_t sample; uint8_t status, a, b; };
		std::vector<Event> events;
		uint64_t frame = 0;
	private:
		void readMidiOut(std::vector<SMidiEvent>&) override {}
		bool sendMidi(const SMidiEvent&, std::vector<SMidiEvent>&) override { return true; }
		void processAudio(const TAudioInputs&, const TAudioOutputs&, size_t) override {}
		float m_rate;
	};

	struct Fixture
	{
		explicit Fixture(float rate) : device(rate), plugin(&device, [](Device*) {})
		{
			plugin.setHostSamplerate(rate, rate);
			plugin.setBlockSize(512);
			plugin.reserveMidiEventCapacity();
			outputs[0] = buffer.data();
		}
		void block(uint32_t count, double bpm, double ppq, bool playing = true, bool ppqKnown = true)
		{
			plugin.process({}, outputs, count, bpm, ppq, playing, ppqKnown);
		}
		ClockProbe device;
		Plugin plugin;
		std::array<float, 512> buffer{};
		TAudioOutputs outputs{};
	};

	void startStop()
	{
		Fixture f(48000);
		f.block(0, 120, 0);
		require(f.device.events.empty(), "zero-length callback consumed transport start");
		f.block(1, 120, 0);
		require(f.device.events.size() == 2 && f.device.events[0].status == M_START
			&& f.device.events[1].status == M_TIMINGCLOCK && f.device.events[1].sample == 0,
			"first beat did not receive START then CLOCK at sample zero");
		f.device.events.clear();
		f.block(128, 0, 0, false);
		require(f.device.events.size() == 1 && f.device.events[0].status == M_STOP,
			"missing BPM prevented STOP or clocks continued while stopped");
		f.device.events.clear();
		f.block(128, 120, 0, false);
		require(f.device.events.empty(), "stopped transport repeated STOP or emitted clocks");
	}

	void constantTempo(float rate, bool variable, double origin, uint32_t seconds)
	{
		Fixture f(rate);
		constexpr double bpm = 123.456;
		constexpr std::array<uint32_t, 6> sizes{0, 1, 127, 256, 63, 512};
		const auto total = uint64_t(rate) * seconds;
		for(size_t block = 0; f.device.frame < total; ++block)
		{
			const auto count = static_cast<uint32_t>(std::min<uint64_t>(
				variable ? sizes[block % sizes.size()] : 512, total - f.device.frame));
			f.block(count, bpm, origin + f.device.frame * bpm / (60.0 * rate));
		}
		std::vector<uint64_t> actual, expected;
		for(auto event : f.device.events)
			if(event.status == M_TIMINGCLOCK) actual.push_back(event.sample);
		const auto firstTick = static_cast<int64_t>(std::ceil(static_cast<long double>(origin) * 24));
		for(auto tick = firstTick;; ++tick)
		{
			const auto exact = (static_cast<long double>(tick) / 24 - origin) * rate * 60 / bpm;
			const auto sample = static_cast<uint64_t>(std::ceil(std::max(0.0L, exact - 1e-8L)));
			if(sample >= total) break;
			expected.push_back(sample);
		}
		require(actual.size() == expected.size(), "clock count drifted or a boundary tick was lost");
		for(size_t i = 0; i < actual.size(); ++i)
			require(std::abs(static_cast<double>(actual[i]) - expected[i]) <= 1,
				"clock drift exceeded one host sample");
		std::cout << "clock rate=" << rate << " variable=" << variable << " origin=" << origin
			<< " seconds=" << seconds << " ticks=" << actual.size() << " passed\n";
	}

	void seekAndTempo()
	{
		Fixture f(48000);
		f.block(500, 120, 0);
		f.device.events.clear();
		// Change tempo half way between ticks. The next tick is 1000/3
		// samples away at 180 BPM, hence offset 334.
		f.block(512, 180, 1.0 / 48);
		require(f.device.events.size() == 1 && f.device.events[0].status == M_TIMINGCLOCK
			&& f.device.events[0].sample == 834, "tempo change reset or misplaced clock phase");
		f.device.events.clear();
		f.block(128, 120, 8.0);
		require(f.device.events.size() == 4 && f.device.events[0].status == M_STOP
			&& f.device.events[1].status == M_SONGPOSITION && f.device.events[1].a == 32
			&& f.device.events[1].b == 0 && f.device.events[2].status == M_CONTINUE
			&& f.device.events[3].status == M_TIMINGCLOCK, "seek did not locate before restarting clocks");
		f.device.events.clear();
		f.block(128, 120, 0.0);
		require(f.device.events.size() == 3 && f.device.events[0].status == M_STOP
			&& f.device.events[1].status == M_START && f.device.events[2].status == M_TIMINGCLOCK,
			"loop to zero did not restart on the downbeat");
	}

	void missingPositionAndFractionalStart()
	{
		Fixture f(48000);
		f.block(512, 120, 1234, true, false);
		f.block(512, 0, -1234, true, false);
		require(f.device.events.size() == 3 && f.device.events[0].status == M_START
			&& f.device.events[1].sample == 0 && f.device.events[2].sample == 1000,
			"missing host position/tempo interrupted the free-running clock");
		f.device.events.clear();
		f.block(512, std::numeric_limits<double>::quiet_NaN(),
			std::numeric_limits<double>::quiet_NaN());
		require(f.device.events.empty(), "invalid host position generated a spurious transport edge");

		Fixture fractional(48000);
		fractional.block(512, 120, .01);
		require(fractional.device.events.size() == 2
			&& fractional.device.events[0].status == M_SONGPOSITION
			&& fractional.device.events[1].status == M_CONTINUE,
			"fractional start emitted an early clock");
		fractional.block(512, 120, .01 + 512.0 / 24000);
		require(fractional.device.events.size() == 3 && fractional.device.events[2].sample == 760,
			"fractional start did not reach the next 24-PPQN boundary");
	}
}

int main()
{
	bool failed = false;
	auto run = [&](auto fn) { try { fn(); } catch(const std::exception& e) { std::cerr << e.what() << '\n'; failed = true; } };
	run(startStop);
	run(seekAndTempo);
	run(missingPositionAndFractionalStart);
	for(float rate : {44100.0f, 48000.0f, 96000.0f})
		for(bool variable : {false, true})
			run([&] { constantTempo(rate, variable, 0, 5); });
	run([] { constantTempo(96000, true, 1000000.125, 240); });
	return failed ? 1 : 0;
}
