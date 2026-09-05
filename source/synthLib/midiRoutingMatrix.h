#pragma once

#include <array>
#include <atomic>
#include <set>
#include <string_view>

#include "midiTypes.h"

#include "baseLib/binarystream.h"

namespace baseLib
{
	class ConfigFile;
}

namespace synthLib
{
	class MidiRoutingMatrix
	{
	public:
		static constexpr uint32_t Size = static_cast<uint32_t>(MidiEventSource::Count);

		enum class EventType : uint8_t
		{
			None          = 0x00,

			Note          = 0x01,
			SysEx         = 0x02,
			Controller    = 0x04,
			PolyPressure  = 0x08,
			Aftertouch    = 0x10,
			PitchBend     = 0x20,
			ProgramChange = 0x40,
			Other         = 0x80,

			First         = Note,
			Last          = Other,
			All           = 0xff,
		};

		friend constexpr EventType operator | (EventType _a, EventType _b) { return static_cast<EventType>(static_cast<uint8_t>(_a) | static_cast<uint8_t>(_b)); }
		friend constexpr EventType operator & (EventType _a, EventType _b) { return static_cast<EventType>(static_cast<uint8_t>(_a) & static_cast<uint8_t>(_b)); }
		friend constexpr EventType& operator |= (EventType& _a, const EventType _b) { _a = _a | _b; return _a; }
		friend constexpr EventType& operator &= (EventType& _a, const EventType _b) { _a = _a & _b; return _a; }
		friend constexpr EventType operator ~ (EventType& _a) { return static_cast<EventType>(~static_cast<uint8_t>(_a)); }

		void saveChunkData(baseLib::BinaryStream& _binaryStream) const;
		void loadChunkData(baseLib::ChunkReader& _cr);

		MidiRoutingMatrix();
		MidiRoutingMatrix(const MidiRoutingMatrix& _other);
		MidiRoutingMatrix(MidiRoutingMatrix&& _other) noexcept;
		MidiRoutingMatrix& operator=(const MidiRoutingMatrix& _other);
		MidiRoutingMatrix& operator=(MidiRoutingMatrix&& _other) noexcept;

		static EventType getEventType(const SMidiEvent& _event)
		{
			if (!_event.sysex.empty())
				return EventType::SysEx;

			switch (_event.a & 0xf0)
			{
			case M_NOTEON:
			case M_NOTEOFF:			return EventType::Note;
			case M_CONTROLCHANGE:	return EventType::Controller;
			case M_AFTERTOUCH:		return EventType::Aftertouch;
			case M_PITCHBEND:		return EventType::PitchBend;
			case M_POLYPRESSURE:	return EventType::PolyPressure;
			case M_PROGRAMCHANGE:	return EventType::ProgramChange;
			default:				return EventType::Other;
			}
		}

		bool enabled(const MidiEventSource _source, const MidiEventSource _destination, const EventType _type) const
		{
			assert(_source != MidiEventSource::Unknown && _destination != MidiEventSource::Unknown);
			return (static_cast<uint8_t>(get(_source, _destination))
				& static_cast<uint8_t>(_type)) != 0;
		}

		bool enabled(const SMidiEvent& _event, const MidiEventSource _destination) const
		{
			return enabled(_event.source, _destination, getEventType(_event));
		}

		void setEnabled(const MidiEventSource _source, const MidiEventSource _destination, EventType _type, const bool _enabled)
		{
			auto& cell = getAtomic(_source, _destination);
			auto current = cell.load(std::memory_order_relaxed);
			const auto type = static_cast<uint8_t>(_type);
			uint8_t desired;
			do
			{
				desired = _enabled ? static_cast<uint8_t>(current | type)
					: static_cast<uint8_t>(current & ~type);
			}
			while(!cell.compare_exchange_weak(current, desired,
				std::memory_order_relaxed, std::memory_order_relaxed));
		}

		static std::string_view toString(MidiEventSource _source);
		static std::string_view toString(EventType _type);

		EventType get(const MidiEventSource _source, const MidiEventSource _destination) const
		{
			return static_cast<EventType>(getAtomic(_source, _destination).load(
				std::memory_order_relaxed));
		}

		bool writeToFile(const std::string& _filename, const std::set<MidiEventSource>& _skipSources = { MidiEventSource::Internal, MidiEventSource::Unknown }) const;
		void writeToFile(baseLib::ConfigFile& _configFile, const std::set<MidiEventSource>& _skipSources = { MidiEventSource::Internal, MidiEventSource::Unknown }) const;

		bool readFromFile(const std::string& _filename);
		bool readFromFile(const baseLib::ConfigFile& _configFile);

		bool operator == (const MidiRoutingMatrix& _other) const
		{
			for(uint32_t source = 0; source < Size; ++source)
				for(uint32_t destination = 0; destination < Size; ++destination)
					if(get(static_cast<MidiEventSource>(source),
						static_cast<MidiEventSource>(destination))
						!= _other.get(static_cast<MidiEventSource>(source),
							static_cast<MidiEventSource>(destination)))
						return false;
			return true;
		}

	private:
		using AtomicEventType = std::atomic<uint8_t>;
		static_assert(AtomicEventType::is_always_lock_free,
			"Realtime MIDI routing requires lock-free byte atomics");

		AtomicEventType& getAtomic(const MidiEventSource _source,
			const MidiEventSource _destination)
		{
			return m_matrix[static_cast<uint32_t>(_source)][static_cast<uint32_t>(_destination)];
		}
		const AtomicEventType& getAtomic(const MidiEventSource _source,
			const MidiEventSource _destination) const
		{
			return m_matrix[static_cast<uint32_t>(_source)][static_cast<uint32_t>(_destination)];
		}

		std::array<std::array<AtomicEventType, Size>, Size> m_matrix;
	};
}
