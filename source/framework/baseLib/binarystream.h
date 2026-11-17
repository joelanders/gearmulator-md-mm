#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <cstring>

namespace baseLib
{
	class StreamBuffer
	{
	protected:
		struct ReadState
		{
			size_t position;
			bool failed;
		};

	public:
		StreamBuffer() = default;
		explicit StreamBuffer(std::vector<uint8_t>&& _buffer) : m_vector(std::move(_buffer))
		{
		}
		explicit StreamBuffer(const size_t _capacity)
		{
			m_vector.reserve(_capacity);
		}
		StreamBuffer(uint8_t* _buffer, const size_t _size) : m_buffer(_buffer), m_size(_size), m_fixedSize(true)
		{
		}
		StreamBuffer(StreamBuffer& _parent, const size_t _childSize)
			: m_size(_childSize), m_fixedSize(true)
		{
			if(!_parent.canRead(_childSize))
			{
				_parent.m_fail = true;
				m_fail = true;
				throw std::range_error("invalid child stream range");
			}

			m_buffer = _parent.buffer();
			if(_parent.tellg() != 0)
				m_buffer += _parent.tellg();
			_parent.m_readPos += _childSize;
		}
		StreamBuffer(StreamBuffer&& _source) noexcept
			: m_buffer(_source.m_buffer)
			, m_size(_source.m_size)
			, m_fixedSize(_source.m_fixedSize)
			, m_readPos(_source.m_readPos)
			, m_writePos(_source.m_writePos)
			, m_vector(std::move(_source.m_vector))
			, m_fail(_source.m_fail)
		{
			_source.destroy();
		}

		StreamBuffer& operator = (StreamBuffer&& _source) noexcept
		{
			if(this == &_source)
				return *this;

			m_buffer = _source.m_buffer;
			m_size = _source.m_size;
			m_fixedSize = _source.m_fixedSize;
			m_readPos = _source.m_readPos;
			m_writePos = _source.m_writePos;
			m_vector = std::move(_source.m_vector);
			m_fail = _source.m_fail;

			_source.destroy();

			return *this;
		}

		void seekg(const size_t _pos)
		{
			if(_pos > size())
			{
				m_fail = true;
				return;
			}
			m_readPos = _pos;
		}
		size_t tellg() const				{ return m_readPos; }
		void seekp(const size_t _pos)
		{
			if(_pos > size())
			{
				m_fail = true;
				return;
			}
			m_writePos = _pos;
		}
		size_t tellp() const				{ return m_writePos; }
		bool eof() const					{ return tellg() >= size(); }
		bool fail() const					{ return m_fail; }

		bool read(uint8_t* _dst, size_t _size)
		{
			if(!canRead(_size))
			{
				m_fail = true;
				return false;
			}
			if(_size)
				::memcpy(_dst, buffer() + m_readPos, _size);
			m_readPos += _size;
			return true;
		}
		bool write(const uint8_t* _src, size_t _size)
		{
			if(tellp() > size() || _size > std::numeric_limits<size_t>::max() - tellp())
			{
				m_fail = true;
				return false;
			}

			const auto requiredSize = tellp() + _size;
			if(requiredSize > size())
			{
				if(m_fixedSize)
				{
					m_fail = true;
					return false;
				}
				m_vector.resize(requiredSize);
			}
			if(_size)
				::memcpy(buffer() + m_writePos, _src, _size);
			m_writePos += _size;
			return true;
		}

		explicit operator bool () const
		{
			return !eof();
		}

		auto& getVector() { return m_vector; }

	protected:
		bool canRead(const size_t _size) const
		{
			return !m_fail && tellg() <= size() && _size <= size() - tellg();
		}

		void markFailed()
		{
			m_fail = true;
		}

		ReadState getReadState() const
		{
			return {m_readPos, m_fail};
		}

		void restoreReadState(const ReadState& _state)
		{
			m_readPos = _state.position;
			m_fail = _state.failed;
		}

	private:
		size_t size() const					{ return m_fixedSize ? m_size : m_vector.size(); }

		uint8_t* buffer()
		{
			if(m_fixedSize)
				return m_buffer;
			return m_vector.data();
		}

		void destroy()
		{
			m_buffer = nullptr;
			m_size = 0;
			m_fixedSize = false;
			m_readPos = 0;
			m_writePos = 0;
			m_vector.clear();
			m_fail = false;
		}

		uint8_t* m_buffer = nullptr;
		size_t m_size = 0;
		bool m_fixedSize = false;
		size_t m_readPos = 0;
		size_t m_writePos = 0;
		std::vector<uint8_t> m_vector;
		bool m_fail = false;
	};

	using StreamSizeType = uint32_t;

	class BinaryStream final : StreamBuffer
	{
	public:
		using Base = StreamBuffer;
		using SizeType = StreamSizeType;

		BinaryStream() = default;

		using StreamBuffer::operator bool;

		explicit BinaryStream(BinaryStream& _parent, SizeType _length) : StreamBuffer(_parent, _length)
		{
		}

		explicit BinaryStream(const size_t _capacity) : StreamBuffer(_capacity)
		{
		}

		template<typename T> explicit BinaryStream(const std::vector<T>& _data)
		{
			const auto bytes = checkedByteSize<T>(_data.size());
			Base::write(reinterpret_cast<const uint8_t*>(_data.data()), bytes);
			checkFail();
			seekg(0);
		}

		// ___________________________________
		// tools
		//

		template<typename Alloc>
		void toVector(std::vector<uint8_t, Alloc>& _buffer, const bool _append = false)
		{
			const auto size = tellp();
			if(size <= 0)
			{
				if(!_append)
					_buffer.clear();
				return;
			}

			seekg(0);

			if(_append)
			{
				const auto currentSize = _buffer.size();
				if(currentSize > std::numeric_limits<size_t>::max() - size)
					throw std::length_error("binary stream output size overflow");
				_buffer.resize(currentSize + size);
				Base::read(&_buffer[currentSize], size);
			}
			else
			{
				_buffer.resize(size);
				Base::read(_buffer.data(), size);
			}
			checkFail();
		}

		bool checkString(const std::string& _str)
		{
			const auto state = getReadState();
			try
			{
				const auto size = read<SizeType>();
				if(size != _str.size())
				{
					restoreReadState(state);
					return false;
				}
				std::string s;
				requireReadable(size);
				s.resize(size);
				Base::read(reinterpret_cast<uint8_t*>(s.data()), size);
				const auto result = _str == s;
				restoreReadState(state);
				return result;
			}
			catch(const std::range_error&)
			{
				restoreReadState(state);
				return false;
			}
		}

		uint32_t getWritePos() const			{ return static_cast<uint32_t>(tellp()); }
		uint32_t getReadPos() const				{ return static_cast<uint32_t>(tellg()); }
		bool endOfStream() const				{ return eof(); }

		void setWritePos(const uint32_t _pos)	{ seekp(_pos); checkFail(); }
		void setReadPos(const uint32_t _pos)	{ seekg(_pos); checkFail(); }
		
		using StreamBuffer::getVector;

		// ___________________________________
		// write
		//

		template<typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>> void write(const T& _value)
		{
			Base::write(reinterpret_cast<const uint8_t*>(&_value), sizeof(_value));
			checkFail();
		}

		template<typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>> void write(const T* _data, const size_t _size)
		{
			if(!_size)
				return;
			Base::write(reinterpret_cast<const uint8_t*>(_data), checkedByteSize<T>(_size));
			checkFail();
		}

		template<typename T, typename Alloc, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>> void write(const std::vector<T, Alloc>& _vector)
		{
			if(_vector.size() > std::numeric_limits<SizeType>::max())
				throw std::length_error("binary stream vector is too large");
			const auto size = static_cast<SizeType>(_vector.size());
			write(size);
			if(size)
			{
				Base::write(reinterpret_cast<const uint8_t*>(_vector.data()), checkedByteSize<T>(size));
				checkFail();
			}
		}

		void write(const std::string& _string)
		{
			if(_string.size() > std::numeric_limits<SizeType>::max())
				throw std::length_error("binary stream string is too large");
			const auto s = static_cast<SizeType>(_string.size());
			write(s);
			Base::write(reinterpret_cast<const uint8_t*>(_string.c_str()), s);
			checkFail();
		}

		void write(const char* const _value)
		{
			write(std::string(_value));
		}

		template<size_t N, std::enable_if_t<N == 5, void*> = nullptr>
		void write4CC(char const(&_str)[N])
		{
			write(_str[0]);
			write(_str[1]);
			write(_str[2]);
			write(_str[3]);
		}


		// ___________________________________
		// read
		//

		template<typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>> T read()
		{
			T v{};
			Base::read(reinterpret_cast<uint8_t*>(&v), sizeof(v));
			checkFail();
			return v;
		}

		template<typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>> T& read(T& _dst)
		{
			Base::read(reinterpret_cast<uint8_t*>(&_dst), sizeof(_dst));
			checkFail();
			return _dst;
		}

		template<typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>> void read(T* _out, const size_t _size)
		{
			if(_size)
			{
				const auto bytes = requireReadableElements<T>(_size);
				Base::read(reinterpret_cast<uint8_t*>(_out), bytes);
			}
			checkFail();
		}

		template<typename T, typename Alloc, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>> void read(std::vector<T, Alloc>& _vector)
		{
			const auto size = read<SizeType>();
			checkFail();
			if (!size)
			{
				_vector.clear();
				return;
			}
			const auto bytes = requireReadableElements<T>(size);
			_vector.resize(size);
			Base::read(reinterpret_cast<uint8_t*>(_vector.data()), bytes);
			checkFail();
		}

		std::string readString()
		{
			const auto size = read<SizeType>();
			requireReadable(size);
			std::string s;
			s.resize(size);
			Base::read(reinterpret_cast<uint8_t*>(s.data()), size);
			checkFail();
			return s;
		}

		template<size_t N, std::enable_if_t<N == 5, void*> = nullptr>
		void read4CC(char const(&_str)[N])
		{
			char res[5];
			read4CC(res);

			return strcmp(res, _str) == 0;
		}

		void read4CC(std::array<char, 5>& _fourCC)
		{
			_fourCC.fill(0);

			_fourCC[0] = read<char>();
			_fourCC[1] = read<char>();
			_fourCC[2] = read<char>();
			_fourCC[3] = read<char>();
		}

		template<size_t N, std::enable_if_t<N == 5, void*> = nullptr>
		void read4CC(char (&_str)[N])
		{
			_str[0] = 'E';
			_str[1] = 'R';
			_str[2] = 'R';
			_str[3] = 'R';
			_str[4] = 0;

			_str[0] = read<char>();
			_str[1] = read<char>();
			_str[2] = read<char>();
			_str[3] = read<char>();
		}

		BinaryStream readChunk();
		template<size_t N, std::enable_if_t<N == 5, void*> = nullptr>
		BinaryStream tryReadChunk(char const(&_4Cc)[N], const uint32_t _version = 1)
		{
			return tryReadChunkInternal(_4Cc, _version, _version).first;
		}

		template<size_t N, std::enable_if_t<N == 5, void*> = nullptr>
		std::pair<BinaryStream,uint32_t> tryReadChunk(char const(&_4Cc)[N], const uint32_t _minVersion, const uint32_t _maxVersion)
		{
			return tryReadChunkInternal(_4Cc, _minVersion, _maxVersion);
		}

	private:
		std::pair<BinaryStream, uint32_t> tryReadChunkInternal(const char* _4Cc, const uint32_t _minVersion, const uint32_t _maxVersion);

		// ___________________________________
		// helpers
		//

	private:
		template<typename T>
		static size_t checkedByteSize(const size_t _count)
		{
			if(_count > std::numeric_limits<size_t>::max() / sizeof(T))
				throw std::range_error("binary stream size overflow");
			return sizeof(T) * _count;
		}

		void requireReadable(const size_t _size)
		{
			if(!canRead(_size))
			{
				markFailed();
				throw std::range_error("end-of-stream");
			}
		}

		template<typename T>
		size_t requireReadableElements(const size_t _count)
		{
			if(_count > std::numeric_limits<size_t>::max() / sizeof(T))
			{
				markFailed();
				throw std::range_error("binary stream size overflow");
			}
			const auto bytes = sizeof(T) * _count;
			requireReadable(bytes);
			return bytes;
		}

		using ReadState = StreamBuffer::ReadState;
		ReadState readState() const { return getReadState(); }
		void restore(const ReadState& _state) { restoreReadState(_state); }

		friend class ChunkReader;
		friend class ChunkWriter;

		void checkFail() const
		{
			if(fail())
				throw std::range_error("end-of-stream");
		}
	};

	struct Chunk
	{
		using SizeType = BinaryStream::SizeType;

		char fourCC[5];
		uint32_t version;
		SizeType length;
		BinaryStream data;

		bool read(BinaryStream& _parentStream)
		{
			_parentStream.read4CC(fourCC);
			version = _parentStream.read<uint32_t>();
			length = _parentStream.read<SizeType>();
			data = BinaryStream(_parentStream, length);
			return !data.endOfStream();
		}
	};

	class ChunkWriter
	{
	public:
		using SizeType = BinaryStream::SizeType;

		template<size_t N, std::enable_if_t<N == 5, void*> = nullptr>
		ChunkWriter(BinaryStream& _stream, char const(&_4Cc)[N], const uint32_t _version = 1)
			: m_stream(_stream)
			, m_uncaughtExceptions(std::uncaught_exceptions())
		{
			m_stream.write4CC(_4Cc);
			m_stream.write(_version);
			m_lengthWritePos = m_stream.tellp();
			m_stream.write<SizeType>(0);
		}

		ChunkWriter() = delete;
		ChunkWriter(ChunkWriter&&) = delete;
		ChunkWriter(const ChunkWriter&) = delete;
		ChunkWriter& operator = (ChunkWriter&&) = delete;
		ChunkWriter& operator = (const ChunkWriter&) = delete;

		~ChunkWriter() noexcept
		{
			try
			{
				// A payload write may reject its arguments or fail an allocation before
				// StreamBuffer can set its fail bit. Never turn that exceptional exit
				// into an apparently valid partial chunk.
				if(std::uncaught_exceptions() > m_uncaughtExceptions)
				{
					m_stream.markFailed();
					return;
				}

				if(m_stream.fail())
					return;

				const size_t currentWritePos = m_stream.tellp();
				SizeType chunkDataLength = 0;
				if(!validateFinalizationPositions(m_lengthWritePos, currentWritePos, chunkDataLength))
				{
					m_stream.markFailed();
					return;
				}

				m_stream.seekp(m_lengthWritePos);
				m_stream.checkFail();
				m_stream.write(chunkDataLength);
				m_stream.seekp(currentWritePos);
				m_stream.checkFail();
			}
			catch(...)
			{
				m_stream.markFailed();
				// Destruction must never replace an active exception or terminate unwinding.
			}
		}

	private:
		static bool validateFinalizationPositions(const size_t _lengthWritePos,
			const size_t _currentWritePos, SizeType& _chunkDataLength) noexcept
		{
			_chunkDataLength = 0;
			constexpr auto maxPosition = static_cast<size_t>(std::numeric_limits<SizeType>::max());
			if(_lengthWritePos > maxPosition || _currentWritePos > maxPosition)
				return false;
			if(_currentWritePos < _lengthWritePos)
				return false;

			const auto lengthAndPayload = _currentWritePos - _lengthWritePos;
			if(lengthAndPayload < sizeof(SizeType))
				return false;
			const auto payloadLength = lengthAndPayload - sizeof(SizeType);
			if(payloadLength > std::numeric_limits<SizeType>::max())
				return false;

			_chunkDataLength = static_cast<SizeType>(payloadLength);
			return true;
		}

		friend struct ChunkWriterTestAccess;

		BinaryStream& m_stream;
		size_t m_lengthWritePos = 0;
		int m_uncaughtExceptions = 0;
	};

	class ChunkReader
	{
	public:
		using SizeType = ChunkWriter::SizeType;
		using ChunkCallback = std::function<void(BinaryStream&, uint32_t)>;	// data, version

		struct ChunkCallbackData
		{
			char fourCC[5];
			uint32_t expectedVersion;
			ChunkCallback callback;
		};

		explicit ChunkReader(BinaryStream& _stream) : m_stream(_stream)
		{
		}

		template<size_t N, std::enable_if_t<N == 5, void*> = nullptr>
		void add(char const(&_4Cc)[N], const uint32_t _version, const ChunkCallback& _callback)
		{
			ChunkCallbackData c;
			strcpy(c.fourCC, _4Cc);
			c.expectedVersion = _version;
			c.callback = _callback;
			supportedChunks.emplace_back(std::move(c));
		}

		void read(const uint32_t _count = 0)
		{
			m_stream.checkFail();
			uint32_t count = 0;

			while(!m_stream.endOfStream() && (!_count || ++count <= _count))
			{
				Chunk chunk;
				chunk.read(m_stream);

				++m_numChunks;

				for (const auto& chunkData : supportedChunks)
				{
					if(0 != strcmp(chunkData.fourCC, chunk.fourCC))
						continue;

					if(chunk.version > chunkData.expectedVersion)
						break;

					++m_numRead;
					chunkData.callback(chunk.data, chunk.version);
					break;
				}
			}
		}

		// Chunk framing is validated before callbacks run. Stream state and internal
		// counters are transactional, but external side effects made by a callback
		// cannot be rolled back if that callback rejects its payload. Non-range
		// exceptions are rethrown after internal state has been restored.
		bool tryRead(const uint32_t _count = 0)
		{
			const auto state = m_stream.readState();
			const auto numRead = m_numRead;
			const auto numChunks = m_numChunks;
			try
			{
				preflight(_count);
				m_stream.restore(state);
				read(_count);
				return true;
			}
			catch(const std::range_error&)
			{
				m_stream.restore(state);
				m_numRead = numRead;
				m_numChunks = numChunks;
				return false;
			}
			catch(...)
			{
				m_stream.restore(state);
				m_numRead = numRead;
				m_numChunks = numChunks;
				throw;
			}
		}

		uint32_t numRead() const
		{
			return m_numRead;
		}

		uint32_t numChunks() const
		{
			return m_numChunks;
		}

	private:
		void preflight(const uint32_t _count)
		{
			m_stream.checkFail();
			uint32_t count = 0;
			while(!m_stream.endOfStream() && (!_count || ++count <= _count))
			{
				Chunk chunk;
				chunk.read(m_stream);
			}
		}

		BinaryStream& m_stream;
		std::vector<ChunkCallbackData> supportedChunks;
		uint32_t m_numRead = 0;
		uint32_t m_numChunks = 0;
	};
}
