#include "mdPanelMidiInput.h"

#include <map>
#include <set>

namespace mdJucePlugin::panelMidi
{
	namespace
	{
		std::mutex g_instanceMutex;
		std::map<std::string, std::set<int>> g_instanceNumbers;	// per base name

		int claimInstanceNumber(const std::string& _baseName)
		{
			const std::lock_guard lock(g_instanceMutex);
			auto& used = g_instanceNumbers[_baseName];
			int number = 1;
			while(used.count(number))
				++number;
			used.insert(number);
			return number;
		}

		void releaseInstanceNumber(const std::string& _baseName, const int _number)
		{
			const std::lock_guard lock(g_instanceMutex);
			g_instanceNumbers[_baseName].erase(_number);
		}
	}

	Input::Input(const std::string& _portName, Handler _handler)
		: m_baseName(_portName)
		, m_instanceNumber(claimInstanceNumber(_portName))
		, m_portName(m_instanceNumber == 1 ? _portName : _portName + " " + std::to_string(m_instanceNumber))
		, m_handler(std::move(_handler))
	{
#if JUCE_LINUX || JUCE_BSD || JUCE_MAC || JUCE_IOS
		m_input = juce::MidiInput::createNewDevice(m_portName, this);
		if(m_input)
			m_input->start();
#endif
	}

	Input::~Input()
	{
		// Stop the port before the queue and handler go away; the destructor of
		// MidiInput waits for a callback that is still running.
		m_input.reset();
		cancelPendingUpdate();
		releaseInstanceNumber(m_baseName, m_instanceNumber);
	}

	void Input::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& _message)
	{
		const auto count = _message.getRawDataSize();
		if(count < 1 || count > 3)
			return;	// sysex and anything longer is not a panel message

		const auto* const data = _message.getRawData();
		if(!isBindableMessage(data[0]))
			return;	// pressure, pitch bend, program change, clock ...

		const RawMessage message{ data[0], count > 1 ? data[1] : uint8_t(0), count > 2 ? data[2] : uint8_t(0) };

		{
			const std::lock_guard lock(m_mutex);
			if(m_pending.size() >= g_maxPending)
				return;
			m_pending.push_back(message);
		}
		triggerAsyncUpdate();
	}

	void Input::handleAsyncUpdate()
	{
		std::vector<RawMessage> messages;
		{
			const std::lock_guard lock(m_mutex);
			messages.swap(m_pending);
		}
		for(const auto& message : messages)
			m_handler(message);
	}
}
