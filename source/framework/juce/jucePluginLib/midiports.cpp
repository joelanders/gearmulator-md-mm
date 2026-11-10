#include "midiports.h"

#include <set>

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

		// The instances that reopen their ports when the system's MIDI devices change. The change arrives on the message
		// thread, a host may delete a plugin on another one: the destructor waits for a change that is handled
		std::mutex g_instancesMutex;
		std::set<MidiPorts*> g_instances;

		// One for all instances, made on the message thread. Never deleted: JUCE's broadcaster is a static of its own,
		// which of the two goes first at unload is unknown. JUCE reports a change only if the device lists differ, a
		// device that comes back fast with its ports in the same order goes unnoticed
		juce::MidiDeviceListConnection* g_deviceListConnection = nullptr;

		// where a device is in a list of the system's MIDI devices, -1: not there
		int indexOf(const juce::Array<juce::MidiDeviceInfo>& _devices, const juce::String& _id)
		{
			for (int i = 0; i < _devices.size(); ++i)
			{
				if (_devices.getReference(i).identifier == _id)
					return i;
			}
			return -1;
		}
	}

	void MidiPorts::listenToDeviceList()
	{
		if (g_deviceListConnection)
			return;

		g_deviceListConnection = new juce::MidiDeviceListConnection(juce::MidiDeviceListConnection::make([]
		{
			onDevicesChanged();
		}));
	}

	void MidiPorts::onDevicesChanged()
	{
		const auto inputs = juce::MidiInput::getAvailableDevices();
		const auto outputs = juce::MidiOutput::getAvailableDevices();

		const std::lock_guard lock(g_instancesMutex);

		// All close first: JUCE shares a Windows MIDI input between all that open it, and opens the device anew only
		// once none of them uses it
		for (auto* ports : g_instances)
			ports->closeMovedPorts(inputs, outputs);

		for (auto* ports : g_instances)
			ports->openClosedPorts();
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
		const std::lock_guard lock(g_instancesMutex);

		g_instances.insert(this);

		if (juce::MessageManager::existsAndIsCurrentThread())
		{
			listenToDeviceList();
		}
		else
		{
			juce::MessageManager::callAsync([]
			{
				const std::lock_guard l(g_instancesMutex);
				listenToDeviceList();
			});
		}
	}

	MidiPorts::~MidiPorts()
	{
		{
			const std::lock_guard lock(g_instancesMutex);
			g_instances.erase(this);
		}

		close();
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

		// the selection, also while its device is away
		const std::lock_guard lock(m_mutexSelection);
		_binaryStream.write(m_selectedInput.toStdString());
		_binaryStream.write(m_selectedOutput.toStdString());
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

	bool MidiPorts::setMidiOutput(const juce::String& _out)
	{
		const std::lock_guard lock(m_mutexSelection);
		m_selectedOutput = _out;
		return openMidiOutput(_out);
	}

	bool MidiPorts::setMidiInput(const juce::String& _in)
	{
		const std::lock_guard lock(m_mutexSelection);
		m_selectedInput = _in;
		return openMidiInput(_in);
	}

	void MidiPorts::closeMovedPorts(const juce::Array<juce::MidiDeviceInfo>& _inputs, const juce::Array<juce::MidiDeviceInfo>& _outputs)
	{
		const std::lock_guard lock(m_mutexSelection);

		if (m_midiInput && indexOf(_inputs, m_selectedInput) != m_inputIndex)
			openMidiInput({});

		if (m_midiOutput.isValid() && indexOf(_outputs, m_selectedOutput) != m_outputIndex)
			openMidiOutput({});
	}

	void MidiPorts::openClosedPorts()
	{
		const std::lock_guard lock(m_mutexSelection);

		if (!m_midiInput && !m_selectedInput.isEmpty())
			openMidiInput(m_selectedInput);

		if (!m_midiOutput.isValid() && !m_selectedOutput.isEmpty())
			openMidiOutput(m_selectedOutput);
	}

	bool MidiPorts::openMidiOutput(const juce::String& _out)
	{
		// closing stops and joins the sender and drops what it had queued, nothing goes out to the next port
		m_midiOutput.close();

		if(_out.isEmpty())
			return false;

		// For an output it does not find, JUCE opens the Microsoft GS Wavetable Synth on Windows. A device that is away
		// opens once it is back
		const auto index = indexOf(juce::MidiOutput::getAvailableDevices(), _out);
		if (index < 0)
			return false;

		auto output = juce::MidiOutput::openDevice(_out);
		if (output == nullptr)
			return false;

		m_outputIndex = index;
		return m_midiOutput.setOutput(std::make_unique<JuceMidiOutputSink>(std::move(output)));
	}

	bool MidiPorts::openMidiInput(const juce::String& _in)
	{
		if (m_midiInput != nullptr)
		{
			m_midiInput->stop();
			m_midiInput = nullptr;
		}

		if(_in.isEmpty())
			return false;

		// a device that is away opens once it is back
		const auto index = indexOf(juce::MidiInput::getAvailableDevices(), _in);
		if (index < 0)
			return false;

		m_midiInput = juce::MidiInput::openDevice(_in, this);
		if (m_midiInput != nullptr)
		{
			m_inputIndex = index;
			m_midiInput->start();
			return true;
		}
		return false;
	}

	void MidiPorts::handleIncomingMidiMessage(juce::MidiInput* _source, const juce::MidiMessage& _message)
	{
		m_processor.handleIncomingMidiMessage(_source, _message);
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
}
