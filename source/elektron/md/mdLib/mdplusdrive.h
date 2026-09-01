#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace md
{
	constexpr size_t g_plusDriveMaxSerializedBytes = 512u * 1024u * 1024u;

	// Serial MMC device connected to the Machinedrum MKII Port A pins. The
	// ColdFire drives clock on bit 2 and uses bit 3 as the bidirectional command
	// line; bits 7..4 are the four data lines. Storage commands are implemented
	// separately from the wire protocol so a sparse backing store can be used.
	class PlusDrive
	{
	public:
		void reset();
		void setEnabled(bool _enabled);
		void portChanged(uint8_t _direction, uint8_t _output);
		uint8_t inputLevel() const;

		bool enabled() const { return m_enabled; }
		bool initialized() const { return m_initialized; }
		uint64_t commandCount() const { return m_commandCount; }
		uint8_t lastCommand() const { return m_lastCommand; }
		size_t storedSectorCount() const { return m_sectors.size(); }
		std::vector<uint8_t> copyStorage() const;
		static bool validateStorage(const uint8_t* _data, size_t _size);
		static bool validateStorage(const std::vector<uint8_t>& _data)
		{
			return validateStorage(_data.data(), _data.size());
		}
		static bool isBlankStorage(const std::vector<uint8_t>& _data);
		bool replaceStorage(const std::vector<uint8_t>& _data, bool _dirty = false);
		bool storageDirty() const { return m_storageGeneration != m_persistedGeneration; }
		uint64_t storageGeneration() const { return m_storageGeneration; }
		uint64_t persistedGeneration() const { return m_persistedGeneration; }
		void markStoragePersisted(uint64_t _generation);

	private:
		void clockEdge(uint8_t _direction, uint8_t _output);
		void receiveCommandBit(bool _bit);
		void executeCommand();
		void advanceReadData(uint8_t _direction);
		void advanceWriteData(uint8_t _direction, uint8_t _output);
		void queueShortResponse(uint32_t _value);
		void queueLongResponse(const std::array<uint8_t, 16>& _value);
		void queueResponse(const uint8_t* _bytes, size_t _size);
		void storageChanged(bool _dirty);

		bool m_enabled = false;
		bool m_clock = false;
		bool m_commandOutput = true;
		std::array<uint8_t, 6> m_command{};
		size_t m_commandBits = 0;
		std::array<uint8_t, 17> m_response{};
		size_t m_responseBits = 0;
		size_t m_responseCursor = 0;
		size_t m_responseDelay = 0;
		bool m_initialized = false;
		bool m_reading = false;
		bool m_singleBlock = false;
		size_t m_dataDelay = 0;
		size_t m_dataCursor = 0;
		uint8_t m_dataOutput = 0xf0;
		uint32_t m_readSector = 0;
		enum class WriteState { Idle, AwaitStart, Data, Crc, Response };
		WriteState m_writeState = WriteState::Idle;
		bool m_singleWrite = false;
		uint32_t m_writeSector = 0;
		size_t m_writeCursor = 0;
		size_t m_writeResponseCursor = 0;
		std::array<uint8_t, 512> m_writeBlock{};
		std::unordered_map<uint32_t, std::array<uint8_t, 512>> m_sectors;
		bool m_writeSawIdleData = false;
		uint64_t m_storageGeneration = 0;
		uint64_t m_persistedGeneration = 0;
		mutable std::vector<uint8_t> m_serializedStorage;
		uint64_t m_commandCount = 0;
		uint8_t m_lastCommand = 0xff;
	};
}
