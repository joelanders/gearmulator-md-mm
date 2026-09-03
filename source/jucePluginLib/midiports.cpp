#include "midiports.h"

#include "processor.h"

#include "dsp56kBase/threadtools.h"

#include "juce_audio_devices/juce_audio_devices.h"

#include "synthLib/midiBufferParser.h"

#include <cassert>

namespace pluginLib
{
	namespace
	{
		class JuceMidiOutputSink final : public MidiOutputSink
		{
		public:
			explicit JuceMidiOutputSink(std::unique_ptr<juce::MidiOutput> _output)
				: m_output(std::move(_output)) {}

			juce::String getIdentifier() const override
			{
				return m_output->getIdentifier();
			}
			void start() override { m_output->startBackgroundThread(); }
			void stop() override
			{
				if(m_output->isBackgroundThreadRunning())
					m_output->stopBackgroundThread();
			}
			bool isRunning() const override
			{
				return m_output->isBackgroundThreadRunning();
			}
			void sendMessageNow(const juce::MidiMessage& _message) override
			{
				m_output->sendMessageNow(_message);
			}

		private:
			std::unique_ptr<juce::MidiOutput> m_output;
		};
	}

	MidiOutputDispatcher::~MidiOutputDispatcher()
	{
		close();
	}

	void MidiOutputDispatcher::push(juce::MidiMessage&& _message)
	{
		assert(!outputQueueFull());
		m_messages[m_write] = std::move(_message);
		m_write = (m_write + 1) % Capacity;
		++m_count;
	}

	juce::MidiMessage MidiOutputDispatcher::pop()
	{
		assert(m_count > 0);
		auto result = std::move(m_messages[m_read]);
		m_read = (m_read + 1) % Capacity;
		--m_count;
		return result;
	}

	void MidiOutputDispatcher::clear()
	{
		m_read = 0;
		m_write = 0;
		m_count = 0;
	}

	void MidiOutputDispatcher::send(juce::MidiMessage&& _message)
	{
		std::unique_lock lock(m_mutex);
		if(m_output == nullptr || m_stopping)
			return;
		m_condition.wait(lock, [this]
		{
			return !outputQueueFull() || m_stopping || m_output == nullptr;
		});
		if(m_output == nullptr || m_stopping)
			return;
		push(std::move(_message));
		lock.unlock();
		m_condition.notify_one();
	}

	void MidiOutputDispatcher::send(const juce::MidiMessage& _message)
	{
		auto copy = _message;
		send(std::move(copy));
	}

	bool MidiOutputDispatcher::trySend(juce::MidiMessage&& _message)
	{
		std::unique_lock lock(m_mutex, std::try_to_lock);
		if(!lock.owns_lock() || m_stopping)
			return false;
		if(m_output == nullptr)
			return true;
		if(outputQueueFull())
			return false;
		push(std::move(_message));
		lock.unlock();
		m_condition.notify_one();
		return true;
	}

	bool MidiOutputDispatcher::setOutput(std::unique_ptr<MidiOutputSink> _output)
	{
		const std::lock_guard configurationLock(m_configurationMutex);
		{
			std::lock_guard lock(m_mutex);
			m_stopping = true;
		}
		m_condition.notify_all();

		if(m_thread)
		{
			m_thread->join();
			m_thread.reset();
		}

		{
			std::lock_guard lock(m_mutex);
			if(m_output != nullptr && m_output->isRunning())
				m_output->stop();
			m_output.reset();
			clear();
		}

		if(_output != nullptr)
			_output->start();
		const auto opened = _output != nullptr;
		{
			std::lock_guard lock(m_mutex);
			m_output = std::move(_output);
			m_stopping = false;
		}
		if(opened)
			m_thread = std::make_unique<std::thread>([this] { senderThread(); });
		return opened;
	}

	void MidiOutputDispatcher::close()
	{
		(void)setOutput({});
	}

	bool MidiOutputDispatcher::isValid() const
	{
		const std::lock_guard lock(m_mutex);
		return m_output != nullptr && !m_stopping;
	}

	juce::String MidiOutputDispatcher::getOutputId() const
	{
		const std::lock_guard lock(m_mutex);
		return m_output != nullptr ? m_output->getIdentifier() : juce::String();
	}

	void MidiOutputDispatcher::senderThread()
	{
		dsp56k::ThreadTools::setCurrentThreadName("MidiOutputSender");
		for(;;)
		{
			std::unique_lock lock(m_mutex);
			m_condition.wait(lock, [this]
			{
				return m_stopping || m_count > 0;
			});
			if(m_stopping)
				return;

			auto message = pop();
			auto* const output = m_output.get();
			lock.unlock();
			m_condition.notify_all();
			if(output != nullptr)
				output->sendMessageNow(message);
		}
	}

	MidiPorts::MidiPorts(Processor& _processor) : m_processor(_processor)
	{
	}

	MidiPorts::~MidiPorts()
	{
		close();
		m_deviceManager.reset();
	}

	juce::MidiInput *MidiPorts::getMidiInput() const
	{
		return m_midiInput.get();
	}

	juce::String MidiPorts::getInputId() const
	{
		return getMidiInput() != nullptr ? getMidiInput()->getIdentifier() : juce::String();
	}

	juce::String MidiPorts::getOutputId() const
	{
		return m_midiOutput.getOutputId();
	}

	void MidiPorts::saveChunkData(baseLib::BinaryStream& _binaryStream) const
	{
		baseLib::ChunkWriter cw(_binaryStream, "mpIO", 1);

		if(m_midiInput)
			_binaryStream.write(m_midiInput->getIdentifier().toStdString());
		else
			_binaryStream.write(std::string());
		_binaryStream.write(m_midiOutput.getOutputId().toStdString());
	}

	void MidiPorts::loadChunkData(baseLib::ChunkReader& _cr)
	{
		_cr.add("mpIO", 1, [&](baseLib::BinaryStream& _data, uint32_t)
		{
			const auto input = _data.readString();
			const auto output = _data.readString();

			setMidiInput(input);
			setMidiOutput(output);
		});
	}

	void MidiPorts::close()
	{
		setMidiInput({});
		setMidiOutput({});
	}

	juce::MidiMessage MidiPorts::toJuceMidiMessage(const synthLib::SMidiEvent& _e)
	{
	    if(!_e.sysex.empty())
	    {
		    assert(_e.sysex.front() == 0xf0);
		    assert(_e.sysex.back() == 0xf7);

		    return {_e.sysex.data(), static_cast<int>(_e.sysex.size()), 0.0};
	    }
	    const auto len = synthLib::MidiBufferParser::lengthFromStatusByte(_e.a);
	    if(len == 1)
		    return {_e.a, 0.0};
	    if(len == 2)
		    return {_e.a, _e.b, 0.0};
	    return {_e.a, _e.b, _e.c, 0.0};
	}

	void MidiPorts::send(juce::MidiMessage&& _message)
	{
		m_midiOutput.send(std::move(_message));
	}

	void MidiPorts::send(const juce::MidiMessage& _message)
	{
		auto copy = _message;
		send(std::move(copy));
	}

	bool MidiPorts::trySend(const synthLib::SMidiEvent& _message)
	{
		// The realtime path is only for fixed-size channel messages. SysEx
		// conversion owns variable-size storage and belongs on send().
		if(!_message.sysex.empty())
			return false;
		return m_midiOutput.trySend(toJuceMidiMessage(_message));
	}

	bool MidiPorts::isMidiOutValid() const
	{
		return m_midiOutput.isValid();
	}

	bool MidiPorts::setMidiOutput(const juce::String& _out)
	{
		std::unique_ptr<juce::MidiOutput> output;
		if(!_out.isEmpty())
			output = juce::MidiOutput::openDevice(_out);
		return m_midiOutput.setOutput(output == nullptr ? nullptr
			: std::make_unique<JuceMidiOutputSink>(std::move(output)));
	}

	bool MidiPorts::setMidiInput(const juce::String& _in)
	{
		if (m_midiInput != nullptr)
		{
			m_midiInput->stop();
			m_midiInput = nullptr;
		}

		if(_in.isEmpty())
			return false;

		if(!m_deviceManager)
			m_deviceManager.reset(new juce::AudioDeviceManager());

		if (!m_deviceManager->isMidiInputDeviceEnabled(_in))
			m_deviceManager->setMidiInputDeviceEnabled(_in, true);

		m_midiInput = juce::MidiInput::openDevice(_in, this);
		if (m_midiInput != nullptr)
		{
			m_midiInput->start();
			return true;
		}
		return false;
	}

	void MidiPorts::handleIncomingMidiMessage(juce::MidiInput* _source, const juce::MidiMessage& _message)
	{
		m_processor.handleIncomingMidiMessage(_source, _message);
	}

}
