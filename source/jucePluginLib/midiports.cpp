#include "midiports.h"

#include "processor.h"

#include "dsp56kBase/threadtools.h"

#include "juce_audio_devices/juce_audio_devices.h"

#include "synthLib/midiBufferParser.h"

#include <cassert>

namespace pluginLib
{
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
		const std::lock_guard lock(m_mutexOutput);
		return m_midiOutput != nullptr ? m_midiOutput->getIdentifier() : juce::String();
	}

	void MidiPorts::saveChunkData(baseLib::BinaryStream& _binaryStream) const
	{
		baseLib::ChunkWriter cw(_binaryStream, "mpIO", 1);

		if(m_midiInput)
			_binaryStream.write(m_midiInput->getIdentifier().toStdString());
		else
			_binaryStream.write(std::string());
		{
			const std::lock_guard lock(m_mutexOutput);
			if(m_midiOutput)
				_binaryStream.write(m_midiOutput->getIdentifier().toStdString());
			else
				_binaryStream.write(std::string());
		}
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

	void MidiPorts::pushOutputMessage(juce::MidiMessage&& _message)
	{
		assert(!outputQueueFull());
		m_midiOutMessages[m_midiOutWrite] = std::move(_message);
		m_midiOutWrite = (m_midiOutWrite + 1) % MidiOutCapacity;
		++m_midiOutCount;
	}

	juce::MidiMessage MidiPorts::popOutputMessage()
	{
		assert(m_midiOutCount > 0);
		auto result = std::move(m_midiOutMessages[m_midiOutRead]);
		m_midiOutRead = (m_midiOutRead + 1) % MidiOutCapacity;
		--m_midiOutCount;
		return result;
	}

	void MidiPorts::clearOutputMessages()
	{
		m_midiOutRead = 0;
		m_midiOutWrite = 0;
		m_midiOutCount = 0;
	}

	void MidiPorts::send(juce::MidiMessage&& _message)
	{
		std::unique_lock lock(m_mutexOutput);
		if(m_midiOutput == nullptr || m_outputStopping)
			return;
		m_outputCondition.wait(lock, [this]
		{
			return !outputQueueFull() || m_outputStopping || m_midiOutput == nullptr;
		});
		if(m_midiOutput == nullptr || m_outputStopping)
			return;
		pushOutputMessage(std::move(_message));
		lock.unlock();
		m_outputCondition.notify_one();
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
		std::unique_lock lock(m_mutexOutput, std::try_to_lock);
		if(!lock.owns_lock() || m_outputStopping)
			return false;
		if(m_midiOutput == nullptr)
			return true;
		if(outputQueueFull())
			return false;
		pushOutputMessage(toJuceMidiMessage(_message));
		lock.unlock();
		m_outputCondition.notify_one();
		return true;
	}

	bool MidiPorts::isMidiOutValid() const
	{
		const std::lock_guard lock(m_mutexOutput);
		return m_midiOutput != nullptr && !m_outputStopping;
	}

	bool MidiPorts::setMidiOutput(const juce::String& _out)
	{
		const std::lock_guard configurationLock(m_mutexOutputConfiguration);
		{
			std::lock_guard lock(m_mutexOutput);
			m_outputStopping = true;
		}
		m_outputCondition.notify_all();

		if (m_threadOutput)
		{
			m_threadOutput->join();
			m_threadOutput.reset();
		}

		{
			std::lock_guard lock(m_mutexOutput);
			if(m_midiOutput != nullptr && m_midiOutput->isBackgroundThreadRunning())
				m_midiOutput->stopBackgroundThread();
			m_midiOutput.reset();
			clearOutputMessages();
		}

		std::unique_ptr<juce::MidiOutput> output;
		if(!_out.isEmpty())
			output = juce::MidiOutput::openDevice(_out);
		if(output != nullptr)
			output->startBackgroundThread();

		const auto opened = output != nullptr;
		{
			std::lock_guard lock(m_mutexOutput);
			m_midiOutput = std::move(output);
			m_outputStopping = false;
		}
		if(opened)
			m_threadOutput.reset(new std::thread([this] { senderThread(); }));
		return opened;
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

	void MidiPorts::senderThread()
	{
		dsp56k::ThreadTools::setCurrentThreadName("MIdiOutputSender");

		for(;;)
		{
			std::unique_lock lock(m_mutexOutput);
			m_outputCondition.wait(lock, [this]
			{
				return m_outputStopping || m_midiOutCount > 0;
			});
			if(m_outputStopping)
				return;

			auto msg = popOutputMessage();
			auto* const out = m_midiOutput.get();
			lock.unlock();
			m_outputCondition.notify_all();
			if(out != nullptr)
				out->sendMessageNow(msg);
		}
	}
}
