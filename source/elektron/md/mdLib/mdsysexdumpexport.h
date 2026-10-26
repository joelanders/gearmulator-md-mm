#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "mdtypes.h"

#include "synthLib/midiTypes.h"

namespace md
{
	enum class SysexDumpScope : uint8_t
	{
		CurrentKit,
		CurrentPattern,
		AllUserData
	};

	enum class SysexDumpExportState : uint8_t
	{
		Idle,
		Running,
		Complete,
		Failed
	};

	struct SysexDumpExportProgress
	{
		SysexDumpExportState state = SysexDumpExportState::Idle;
		SysexDumpScope scope = SysexDumpScope::AllUserData;
		size_t done = 0;
		size_t total = 0;
		// The slot a CurrentKit/CurrentPattern export resolved to, or 0xff.
		uint8_t slot = 0xff;
	};

	// Asks the machine for user-data dumps over MIDI, one request at a time, so
	// the editor can save them without the user navigating the SYSEX SEND page.
	// Replies are recorded by MidiSysexRecorder; this class only sequences the
	// requests and watches for each reply. Every call requires the owning
	// Plugin/processor lock, like the recorder.
	class SysexDumpExport
	{
	public:
		using Request = std::array<uint8_t, 9>;
		static constexpr uint32_t g_replyTimeoutSamples = g_samplerate * 3;

		explicit SysexDumpExport(const MachineModel _model) : m_model(_model) {}

		// _saveKitFirst stores the live kit into its own slot before requesting it,
		// like pressing SAVE KIT; dumps otherwise return the stored kit without
		// unsaved edits. Only meaningful for CurrentKit.
		bool start(const SysexDumpScope _scope, const bool _saveKitFirst)
		{
			if(m_progress.state == SysexDumpExportState::Running)
				return false;
			m_steps.clear();
			if(_scope == SysexDumpScope::CurrentKit)
			{
				m_steps.push_back({StepKind::Status, g_statusKit, 0});
				if(_saveKitFirst)
					m_steps.push_back({StepKind::SaveCurrentKit, 0, 0});
				m_steps.push_back({StepKind::DumpCurrent, g_kitDump, 0});
			}
			else if(_scope == SysexDumpScope::CurrentPattern)
			{
				m_steps.push_back({StepKind::Status, g_statusPattern, 0});
				m_steps.push_back({StepKind::DumpCurrent, g_patternDump, 0});
			}
			else
			{
				const bool mm = m_model == MachineModel::Monomachine;
				appendSlots(g_globalDump, 8);
				appendSlots(g_kitDump, mm ? 128 : 64);
				appendSlots(g_patternDump, 128);
				appendSlots(g_songDump, mm ? 24 : 32);
			}
			m_step = 0;
			m_waiting = false;
			m_waitedSamples = 0;
			m_current = 0xff;
			m_progress = {};
			m_progress.state = SysexDumpExportState::Running;
			m_progress.scope = _scope;
			m_progress.total = dumpCount();
			return true;
		}

		void cancel()
		{
			if(m_progress.state == SysexDumpExportState::Running)
				m_progress.state = SysexDumpExportState::Failed;
		}

		// Look for the reply to the outstanding request in this block's machine
		// output, then return the next request to send, if any.
		std::optional<Request> service(const std::vector<synthLib::SMidiEvent>& _events,
			const size_t _from)
		{
			if(m_progress.state != SysexDumpExportState::Running)
				return std::nullopt;
			if(m_waiting)
			{
				for(size_t i = _from; i < _events.size() && m_waiting; ++i)
					acceptReply(_events[i].sysex);
				if(m_waiting)
					return std::nullopt;
			}
			while(m_step < m_steps.size())
			{
				const auto& step = m_steps[m_step];
				if(step.kind == StepKind::SaveCurrentKit)
				{
					// No reply. The firmware handles MIDI in order, so the following
					// dump request is answered after the save has been applied.
					++m_step;
					return message(g_kitSave, m_current);
				}
				m_waiting = true;
				m_waitedSamples = 0;
				if(step.kind == StepKind::Status)
					return message(g_statusRequest, step.value);
				const auto slot = step.kind == StepKind::DumpCurrent ? m_current : step.slot;
				return message(uint8_t(step.value + 1), slot);
			}
			m_progress.state = SysexDumpExportState::Complete;
			return std::nullopt;
		}

		void advance(const size_t _samples)
		{
			if(m_progress.state != SysexDumpExportState::Running || !m_waiting)
				return;
			m_waitedSamples += _samples;
			if(m_waitedSamples >= g_replyTimeoutSamples)
				m_progress.state = SysexDumpExportState::Failed;
		}

		// Called once the editor has taken the recording.
		void reset()
		{
			if(m_progress.state != SysexDumpExportState::Running)
				m_progress = {};
		}

		bool running() const { return m_progress.state == SysexDumpExportState::Running; }
		const SysexDumpExportProgress& progress() const { return m_progress; }

	private:
		enum class StepKind : uint8_t { Status, SaveCurrentKit, DumpCurrent, Dump };

		struct Step
		{
			StepKind kind;
			uint8_t value;	// status parameter or dump command
			uint8_t slot;
		};

		static constexpr uint8_t g_globalDump = 0x50;
		static constexpr uint8_t g_kitDump = 0x52;
		static constexpr uint8_t g_kitSave = 0x59;
		static constexpr uint8_t g_patternDump = 0x67;
		static constexpr uint8_t g_songDump = 0x69;
		static constexpr uint8_t g_statusRequest = 0x70;
		static constexpr uint8_t g_statusResponse = 0x72;
		static constexpr uint8_t g_statusKit = 0x02;
		static constexpr uint8_t g_statusPattern = 0x04;

		void appendSlots(const uint8_t _command, const uint8_t _count)
		{
			for(uint8_t slot = 0; slot < _count; ++slot)
				m_steps.push_back({StepKind::Dump, _command, slot});
		}

		size_t dumpCount() const
		{
			size_t count = 0;
			for(const auto& step : m_steps)
				count += step.kind == StepKind::Dump || step.kind == StepKind::DumpCurrent;
			return count;
		}

		uint8_t product() const { return m_model == MachineModel::Monomachine ? 3 : 2; }

		bool isElektron(const synthLib::SysexBuffer& _sysex) const
		{
			return _sysex.size() >= 10 && _sysex[0] == 0xf0 && _sysex[1] == 0 && _sysex[2] == 0x20
				&& _sysex[3] == 0x3c && _sysex[4] == product();
		}

		void acceptReply(const synthLib::SysexBuffer& _sysex)
		{
			if(!isElektron(_sysex))
				return;
			const auto& step = m_steps[m_step];
			if(step.kind == StepKind::Status)
			{
				if(_sysex[6] != g_statusResponse || _sysex[7] != step.value || _sysex[8] > 0x7f)
					return;
				m_current = _sysex[8];
				m_progress.slot = m_current;
			}
			else
			{
				const auto slot = step.kind == StepKind::DumpCurrent ? m_current : step.slot;
				if(_sysex[6] != step.value || _sysex[9] != slot)
					return;
				++m_progress.done;
			}
			m_waiting = false;
			++m_step;
		}

		Request message(const uint8_t _command, const uint8_t _value) const
		{
			return {0xf0, 0x00, 0x20, 0x3c, product(), 0x00, _command, uint8_t(_value & 0x7f), 0xf7};
		}

		const MachineModel m_model;
		std::vector<Step> m_steps;
		size_t m_step = 0;
		bool m_waiting = false;
		size_t m_waitedSamples = 0;
		uint8_t m_current = 0xff;
		SysexDumpExportProgress m_progress;
	};
}
