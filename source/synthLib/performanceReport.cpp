#include "performanceReport.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace
{
	std::string quote(const std::string& value)
	{
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << '"';
		for(const unsigned char c : value)
		{
			if(c == '"' || c == '\\') out << '\\' << c;
			else if(c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
			else out << c;
		}
		out << '"';
		return out.str();
	}
}

namespace synthLib
{
	PerformanceReport::PerformanceReport(RealtimeInstrumentation& _instrumentation)
		: m_instrumentation(_instrumentation) {}

	PerformanceReport::~PerformanceReport() { stop(); }

	void PerformanceReport::start(std::string _filename, Context _context)
	{
		start(std::move(_filename), std::move(_context), Limits{});
	}

	void PerformanceReport::start(std::string _filename, Context _context, Limits _limits)
	{
		stop();
		m_stop.store(false, std::memory_order_release);
		m_status.store(Status::Starting, std::memory_order_release);
		try
		{
			m_thread = std::thread([this, filename = std::move(_filename), context = std::move(_context), _limits]
			{
				run(filename, context, _limits);
			});
		}
		catch(...)
		{
			m_status.store(Status::Error, std::memory_order_release);
		}
	}

	void PerformanceReport::stop()
	{
		m_instrumentation.setEnabled(false);
		m_stop.store(true, std::memory_order_release);
		m_wake.notify_one();
		if(m_thread.joinable()) m_thread.join();
	}

	std::string PerformanceReport::formatContext(const Context& _context)
	{
		std::string out = "{\"type\":\"session\",\"schema\":1,\"duration_unit\":\"ns\"";
		for(const auto& field : _context) out += ',' + quote(field.first) + ':' + quote(field.second);
		return out + "}\n";
	}

	std::string PerformanceReport::formatSummary(const RealtimeInstrumentationSnapshot& s, uint64_t _elapsedNanoseconds)
	{
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << "{\"type\":\"summary\",\"elapsedNanoseconds\":" << _elapsedNanoseconds;
#define FIELD(name) out << ",\"" #name "\":" << s.name
		FIELD(outerHostCallbackCount); FIELD(outerHostCallbackNanoseconds);
		FIELD(outerHostCallbackMaxNanoseconds); FIELD(outerHostCallbackOverrunCount);
		FIELD(outerHostCallbackMaxOverrunNanoseconds); FIELD(offlineCallbackCount);
		FIELD(bypassedCallbackCount); FIELD(synthProcessCount); FIELD(synthProcessNanoseconds);
		FIELD(synthProcessMaxNanoseconds); FIELD(synthProcessLockWaitNanoseconds);
		FIELD(synthProcessLockWaitMaxNanoseconds); FIELD(resamplerCallCount);
		FIELD(resamplerNanoseconds); FIELD(resamplerMaxNanoseconds); FIELD(resamplerHostFrames);
		FIELD(resamplingActiveCallbackCount); FIELD(deviceProcessNanoseconds); FIELD(deviceProcessMaxNanoseconds);
		FIELD(jitCompilationCount); FIELD(liveJitCompilationCount); FIELD(deferredCandidateJitCompilationCount);
		FIELD(callbacksWithJitCompilation); FIELD(deferredDualMachineCallbackCount);
		FIELD(deferredCandidateAdvanceCount); FIELD(deferredCandidateFrames);
		FIELD(deferredCandidateNanoseconds); FIELD(deferredCandidateMaxNanoseconds);
		FIELD(callbacksWithAuxOutputBuses); FIELD(latestActiveOutputBuses); FIELD(latestActiveOutputChannels);
		FIELD(maximumActiveOutputBuses); FIELD(maximumActiveOutputChannels); FIELD(slowCallbacksDropped);
#undef FIELD
		out << ",\"realtimeBudgetHistogram\":[";
		for(size_t i = 0; i < s.realtimeBudgetHistogram.size(); ++i)
			out << (i ? "," : "") << s.realtimeBudgetHistogram[i];
		return out.str() + "]}\n";
	}

	std::string PerformanceReport::formatCallback(const RealtimeSlowCallback& c)
	{
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << "{\"type\":\"callback\"";
#define FIELD(name) out << ",\"" #name "\":" << c.name
		FIELD(index); FIELD(startNanoseconds); FIELD(durationNanoseconds); FIELD(budgetNanoseconds);
		FIELD(interCallbackNanoseconds); FIELD(synthNanoseconds); FIELD(lockWaitNanoseconds);
		FIELD(resamplerNanoseconds); FIELD(deviceNanoseconds); FIELD(deferredNanoseconds);
		FIELD(liveJitCompilations); FIELD(deferredJitCompilations); FIELD(frames); FIELD(sampleRate);
		FIELD(deviceSampleRate); FIELD(resamplerMode); FIELD(dspClockPercent);
		FIELD(outputBuses); FIELD(outputChannels); FIELD(midiEvents); FIELD(midiBytes);
		out << std::boolalpha;
		FIELD(bypassed); FIELD(playing); FIELD(offline); FIELD(resamplingActive); FIELD(dualMachine);
#undef FIELD
		return out.str() + "}\n";
	}

	void PerformanceReport::run(const std::string& _filename, const Context& _context, Limits _limits) noexcept
	{
		try
		{
			std::unique_ptr<FILE, decltype(&std::fclose)> file(
				baseLib::filesystem::openFile(_filename, "wb"), &std::fclose);
			if(!file) throw std::runtime_error("Cannot open performance report");
			size_t bytes = 0;
			const auto write = [&](const std::string& line)
			{
				if(std::fwrite(line.data(), 1, line.size(), file.get()) != line.size())
					throw std::runtime_error("Cannot write performance report");
				bytes += line.size();
			};
			// Keep room for a final summary and stop reason, including a full queue.
			constexpr size_t trailerReserve = 8192;
			_limits.bytes = std::max(_limits.bytes, trailerReserve * 2);
			_limits.flushInterval = std::max(_limits.flushInterval, std::chrono::milliseconds(10));
			const auto header = formatContext(_context);
			if(header.size() > _limits.bytes - trailerReserve)
				throw std::runtime_error("Performance report context is too large");
			write(header);
			RealtimeSlowCallback callback;
			while(m_instrumentation.popSlowCallback(callback)) {}
			m_instrumentation.reset();
			const auto begin = std::chrono::steady_clock::now();
			const auto elapsed = [&begin]
			{
				return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - begin).count());
			};
			const auto beginNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count());
			// stop() may have been requested while opening the file.
			if(!m_stop.load(std::memory_order_acquire)) m_instrumentation.setEnabled(true);
			m_status.store(Status::Recording, std::memory_order_release);
			bool limited = false;
			for(;;)
			{
				for(size_t drained = 0; drained < RealtimeInstrumentation::SlowCallbackCapacity
					&& m_instrumentation.popSlowCallback(callback); ++drained)
				{
					if(callback.startNanoseconds < beginNs) continue;
					callback.startNanoseconds -= beginNs;
					const auto line = formatCallback(callback);
					if(bytes + line.size() > _limits.bytes - trailerReserve) { limited = true; break; }
					write(line);
				}
				const auto summary = formatSummary(m_instrumentation.snapshot(), elapsed());
				if(bytes + summary.size() > _limits.bytes - trailerReserve) limited = true;
				else write(summary);
				if(std::fflush(file.get()) != 0) throw std::runtime_error("Cannot flush performance report");
				limited |= std::chrono::steady_clock::now() - begin >= _limits.duration;
				if(limited || m_stop.load(std::memory_order_acquire)) break;
				std::unique_lock lock(m_waitMutex);
				m_wake.wait_for(lock, _limits.flushInterval, [this]{ return m_stop.load(std::memory_order_acquire); });
			}
			m_instrumentation.setEnabled(false);
			write(formatSummary(m_instrumentation.snapshot(), elapsed()));
			write(limited ? "{\"type\":\"end\",\"reason\":\"capture_limit\"}\n"
				: "{\"type\":\"end\",\"reason\":\"stopped\"}\n");
			if(std::fflush(file.get()) != 0) throw std::runtime_error("Cannot flush performance report");
			m_status.store(limited ? Status::LimitReached : Status::Stopped, std::memory_order_release);
		}
		catch(...)
		{
			m_instrumentation.setEnabled(false);
			m_status.store(Status::Error, std::memory_order_release);
		}
	}
}
