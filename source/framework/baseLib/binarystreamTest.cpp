#include "binarystream.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace baseLib
{
	struct ChunkWriterTestAccess
	{
		static bool validateChunkWriterPositions(const size_t _lengthWritePos,
			const size_t _currentWritePos, BinaryStream::SizeType& _chunkDataLength)
		{
			return ChunkWriter::validateFinalizationPositions(
				_lengthWritePos, _currentWritePos, _chunkDataLength);
		}
	};
}

namespace
{
	static_assert(std::is_nothrow_destructible_v<baseLib::ChunkWriter>,
		"ChunkWriter destruction must be non-throwing");

	void require(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return;
		std::cerr << "baseLibBinaryStreamTest: " << _message << '\n';
		std::exit(1);
	}

	template<typename Callback>
	void requireRangeError(Callback&& _callback, const std::string& _message)
	{
		try
		{
			_callback();
		}
		catch(const std::range_error&)
		{
			return;
		}
		require(false, _message);
	}

	void verifyGoldenRoundTrip()
	{
		baseLib::BinaryStream stream;
		{
			baseLib::ChunkWriter chunk(stream, "TEST", 1);
			stream.write<uint8_t>(0x7f);
			stream.write(std::string("ok"));
			stream.write(std::vector<uint8_t>{0xaa, 0xbb});
			stream.write<uint8_t>(0xee);
		}

		std::vector<uint8_t> bytes;
		stream.toVector(bytes);
		const std::vector<uint8_t> expected{
			'T', 'E', 'S', 'T',
			0x01, 0x00, 0x00, 0x00,
			0x0e, 0x00, 0x00, 0x00,
			0x7f,
			0x02, 0x00, 0x00, 0x00, 'o', 'k',
			0x02, 0x00, 0x00, 0x00, 0xaa, 0xbb,
			0xee};
		require(bytes == expected, "wire-format golden bytes changed");

		baseLib::BinaryStream input(bytes);
		auto [payload, version] = input.tryReadChunk("TEST", 1, 2);
		require(version == 1, "round-trip changed the chunk version");
		require(payload.read<uint8_t>() == 0x7f, "round-trip changed the scalar");
		require(payload.readString() == "ok", "round-trip changed the string");
		std::vector<uint8_t> vector;
		payload.read(vector);
		require(vector == std::vector<uint8_t>({0xaa, 0xbb}),
			"round-trip changed the vector");
		// Readers may intentionally leave fields for a newer schema revision.
		require(payload.read<uint8_t>() == 0xee, "round-trip changed trailing data");
		require(payload.endOfStream(), "round-trip left unexpected bytes");
	}

	void verifyBoundedReads()
	{
		baseLib::BinaryStream truncated(std::vector<uint8_t>{1, 2, 3});
		requireRangeError([&] { (void)truncated.read<uint32_t>(); },
			"truncated scalar did not fail");

		baseLib::BinaryStream raw(std::vector<uint8_t>{1, 2});
		uint8_t rawBytes[4]{};
		requireRangeError([&] { raw.read(rawBytes, 4); },
			"truncated raw array did not fail immediately");

		baseLib::BinaryStream multiplied;
		uint64_t value = 0;
		requireRangeError([&]
		{
			multiplied.read(&value, std::numeric_limits<size_t>::max());
		}, "element-count multiplication overflow was accepted");

		baseLib::BinaryStream hugeVector;
		hugeVector.write(std::numeric_limits<baseLib::BinaryStream::SizeType>::max());
		hugeVector.write<uint8_t>(0x55);
		hugeVector.setReadPos(0);
		std::vector<uint64_t> vector;
		requireRangeError([&] { hugeVector.read(vector); },
			"oversized vector length was allocated or accepted");
		require(vector.empty(), "failed vector read modified its destination");
		require(hugeVector.getReadPos() == sizeof(baseLib::BinaryStream::SizeType),
			"failed vector read moved beyond its length prefix");
		requireRangeError([&] { (void)hugeVector.read<uint8_t>(); },
			"failed vector read did not poison the stream");

		baseLib::BinaryStream hugeString;
		hugeString.write(std::numeric_limits<baseLib::BinaryStream::SizeType>::max());
		hugeString.write<uint8_t>(0x66);
		hugeString.setReadPos(0);
		requireRangeError([&] { (void)hugeString.readString(); },
			"oversized string length was allocated or accepted");
		require(hugeString.getReadPos() == sizeof(baseLib::BinaryStream::SizeType),
			"failed string read moved beyond its length prefix");
		requireRangeError([&] { (void)hugeString.read<uint8_t>(); },
			"failed string read did not poison the stream");

		baseLib::BinaryStream checkedString;
		checkedString.write<baseLib::BinaryStream::SizeType>(5);
		checkedString.setReadPos(0);
		require(!checkedString.checkString("hello"),
			"truncated checkString input was accepted");
		require(checkedString.getReadPos() == 0,
			"checkString did not restore the cursor");
		require(checkedString.read<baseLib::BinaryStream::SizeType>() == 5,
			"checkString left the stream failed");
	}

	void verifyChunkWriterUnwinding()
	{
		bool caught = false;
		try
		{
			baseLib::BinaryStream stream;
			baseLib::ChunkWriter chunk(stream, "FAIL", 1);
			stream.setWritePos(std::numeric_limits<uint32_t>::max());
		}
		catch(const std::range_error&)
		{
			caught = true;
		}
		require(caught,
			"failed stream did not unwind safely through ChunkWriter destruction");

		baseLib::BinaryStream rejectedPayload;
		caught = false;
		try
		{
			baseLib::ChunkWriter chunk(rejectedPayload, "SIZE", 1);
			const uint64_t value = 0;
			rejectedPayload.write(&value, std::numeric_limits<size_t>::max());
		}
		catch(const std::range_error&)
		{
			caught = true;
		}
		require(caught, "oversized payload write was not rejected");
		requireRangeError([&]
		{
			std::vector<uint8_t> output;
			rejectedPayload.toVector(output);
		}, "exceptional payload write finalized an apparently valid partial chunk");

		baseLib::BinaryStream invalidOrder;
		{
			baseLib::ChunkWriter chunk(invalidOrder, "BACK", 1);
			invalidOrder.setWritePos(0);
		}
		requireRangeError([&] { invalidOrder.setWritePos(0); },
			"invalid ChunkWriter finalization did not poison the stream");
	}

	void verifyChunkWriterPositionValidation()
	{
		using SizeType = baseLib::BinaryStream::SizeType;
		const auto validate = [](const size_t _lengthPos, const size_t _currentPos,
			SizeType& _payloadLength)
		{
			return baseLib::ChunkWriterTestAccess::validateChunkWriterPositions(
				_lengthPos, _currentPos, _payloadLength);
		};

		SizeType payloadLength = 99;
		require(validate(8, 12, payloadLength) && payloadLength == 0,
			"zero-length chunk positions were rejected");
		const auto maxPosition = static_cast<size_t>(std::numeric_limits<SizeType>::max());
		require(validate(0, maxPosition, payloadLength) &&
			payloadLength == std::numeric_limits<SizeType>::max() - sizeof(SizeType),
			"maximum representable total position was rejected");
		require(!validate(8, 7, payloadLength),
			"reversed chunk positions were accepted");
		require(!validate(8, 11, payloadLength),
			"position before the complete length field was accepted");
		if constexpr(sizeof(size_t) > sizeof(SizeType))
		{
			require(!validate(0, maxPosition + 1, payloadLength),
				"total position beyond the public 32-bit contract was accepted");
			require(!validate(maxPosition + 1, maxPosition + 1, payloadLength),
				"length position beyond the public 32-bit contract was accepted");
		}
	}

	void verifyInvalidRanges()
	{
		baseLib::BinaryStream readSeek(std::vector<uint8_t>{1});
		requireRangeError([&] { readSeek.setReadPos(2); },
			"invalid read seek was accepted");

		baseLib::BinaryStream writeSeek;
		writeSeek.write<uint8_t>(1);
		requireRangeError([&] { writeSeek.setWritePos(2); },
			"invalid write seek was accepted");

		baseLib::BinaryStream parent(std::vector<uint8_t>{1});
		requireRangeError([&]
		{
			baseLib::BinaryStream child(parent, 2);
			(void)child;
		}, "invalid child stream length was accepted");
		require(parent.getReadPos() == 0,
			"invalid child stream advanced its parent");
	}

	void verifyChunkCompatibility()
	{
		baseLib::BinaryStream stream;
		{
			baseLib::ChunkWriter zero(stream, "ZERO", 1);
		}
		{
			baseLib::ChunkWriter unknown(stream, "UNKN", 9);
			stream.write<uint8_t>(0x11);
		}
		{
			baseLib::ChunkWriter old(stream, "OLD!", 1);
			stream.write<uint8_t>(0x22);
			stream.write<uint8_t>(0x23);
		}
		{
			baseLib::ChunkWriter future(stream, "NEW!", 3);
			stream.write<uint8_t>(0x33);
		}
		stream.setReadPos(0);

		baseLib::ChunkReader reader(stream);
		bool sawZero = false;
		bool sawOld = false;
		bool sawFuture = false;
		reader.add("ZERO", 1, [&](baseLib::BinaryStream& _data, uint32_t)
		{
			sawZero = true;
			require(_data.endOfStream(), "zero-length chunk was not empty");
		});
		reader.add("OLD!", 2, [&](baseLib::BinaryStream& _data, const uint32_t _version)
		{
			sawOld = true;
			require(_version == 1, "older chunk version changed");
			require(_data.read<uint8_t>() == 0x22, "older chunk payload changed");
			// Deliberately leave 0x23 unread to preserve trailing-field tolerance.
		});
		reader.add("NEW!", 2, [&](baseLib::BinaryStream&, uint32_t)
		{
			sawFuture = true;
		});
		reader.read();

		require(sawZero, "zero-length chunk callback was not invoked");
		require(sawOld, "older supported chunk version was not accepted");
		require(!sawFuture, "too-new chunk version was accepted");
		require(reader.numChunks() == 4, "unknown/versioned chunks were not counted");
		require(reader.numRead() == 2, "unknown/versioned chunks changed read count");
	}

	void verifyTryReadRollback()
	{
		baseLib::BinaryStream stream;
		{
			baseLib::ChunkWriter good(stream, "GOOD", 1);
			stream.write<uint8_t>(0x44);
		}
		stream.write<uint8_t>(0xde);
		stream.write<uint8_t>(0xad);
		stream.write<uint8_t>(0xbe);
		stream.setReadPos(0);

		baseLib::ChunkReader reader(stream);
		uint32_t callbacks = 0;
		reader.add("GOOD", 1, [&](baseLib::BinaryStream& _data, uint32_t)
		{
			++callbacks;
			require(_data.read<uint8_t>() == 0x44, "rollback test payload changed");
		});

		require(!reader.tryRead(), "truncated chunk header was accepted");
		require(callbacks == 0,
			"framing preflight invoked a callback before detecting truncation");
		require(stream.getReadPos() == 0, "tryRead did not restore its cursor");
		require(reader.numChunks() == 0 && reader.numRead() == 0,
			"tryRead did not restore its counters");
		require(reader.tryRead(1), "stream remained failed after rollback");
		require(reader.numChunks() == 1 && reader.numRead() == 1,
			"bounded retry counters are wrong");
		require(callbacks == 1,
			"rollback unexpectedly suppressed or duplicated a callback");
		require(!reader.tryRead(), "trailing malformed header was accepted");
		require(reader.numChunks() == 1 && reader.numRead() == 1,
			"failed trailing retry changed counters");

		baseLib::BinaryStream oversized;
		oversized.write4CC("HUGE");
		oversized.write<uint32_t>(1);
		oversized.write(std::numeric_limits<baseLib::BinaryStream::SizeType>::max());
		oversized.setReadPos(0);
		baseLib::ChunkReader oversizedReader(oversized);
		require(!oversizedReader.tryRead(), "oversized child chunk was accepted");
		require(oversized.getReadPos() == 0,
			"oversized child rollback did not restore its cursor");
		require(oversizedReader.numChunks() == 0 && oversizedReader.numRead() == 0,
			"oversized child rollback changed counters");

		baseLib::BinaryStream malformedPayload;
		{
			baseLib::ChunkWriter chunk(malformedPayload, "BAD!", 1);
			malformedPayload.write<uint8_t>(0x77);
		}
		malformedPayload.setReadPos(0);
		baseLib::ChunkReader malformedReader(malformedPayload);
		uint32_t malformedCallbacks = 0;
		malformedReader.add("BAD!", 1, [&](baseLib::BinaryStream& _data, uint32_t)
		{
			++malformedCallbacks;
			(void)_data.read<uint32_t>();
		});
		require(!malformedReader.tryRead(), "malformed callback payload was accepted");
		require(malformedPayload.getReadPos() == 0 && malformedReader.numChunks() == 0 &&
			malformedReader.numRead() == 0,
			"callback payload failure did not restore stream state and counters");
		require(malformedCallbacks == 1,
			"tryRead unexpectedly rolled back an external callback side effect");

		baseLib::BinaryStream exceptionalPayload;
		{
			baseLib::ChunkWriter chunk(exceptionalPayload, "ERR!", 1);
			exceptionalPayload.write<uint8_t>(0x88);
		}
		exceptionalPayload.setReadPos(0);
		baseLib::ChunkReader exceptionalReader(exceptionalPayload);
		uint32_t exceptionalCallbacks = 0;
		exceptionalReader.add("ERR!", 1, [&](baseLib::BinaryStream& _data, uint32_t)
		{
			++exceptionalCallbacks;
			require(_data.read<uint8_t>() == 0x88, "exception callback payload changed");
			throw std::runtime_error("callback failure");
		});
		bool rethrown = false;
		try
		{
			(void)exceptionalReader.tryRead();
		}
		catch(const std::runtime_error&)
		{
			rethrown = true;
		}
		require(rethrown, "non-range callback exception was not rethrown");
		require(exceptionalPayload.getReadPos() == 0 && exceptionalReader.numChunks() == 0 &&
			exceptionalReader.numRead() == 0,
			"non-range callback exception did not restore internal state");
		require(exceptionalCallbacks == 1,
			"tryRead unexpectedly rolled back a throwing callback side effect");
	}
}

int main()
{
	verifyGoldenRoundTrip();
	verifyBoundedReads();
	verifyInvalidRanges();
	verifyChunkWriterUnwinding();
	verifyChunkWriterPositionValidation();
	verifyChunkCompatibility();
	verifyTryReadRollback();
	std::cout << "baseLibBinaryStreamTest: PASS\n";
	return 0;
}
