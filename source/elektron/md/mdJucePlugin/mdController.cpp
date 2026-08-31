#include "mdController.h"

#include "mdPluginProcessor.h"
#include "mdLib/mdautomation.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdsysexautomation.h"

#include <algorithm>
#include <chrono>
#include <set>

namespace mdJucePlugin
{
	namespace
	{
		constexpr uint64_t g_dumpRequestRetryMs = 2000;

		class AutomationParameter final : public pluginLib::Parameter
		{
		public:
			using pluginLib::Parameter::Parameter;

		protected:
			bool shouldSendRepeatedHostValues() const override { return true; }
		};

		pluginLib::SysEx toPluginSysex(
			const md::automation::sysex::Message& _message)
		{
			return pluginLib::SysEx(_message.begin(), _message.end());
		}
	}

	Controller::Controller(AudioPluginAudioProcessor& _p)
		: pluginLib::Controller(_p, _p.getModel() == md::MachineModel::Monomachine
			? "parameterDescriptions_mm.json" : "parameterDescriptions_md.json")
		, m_model(_p.getModel())
	{
		registerParams(_p, [](const uint8_t _part, const bool _nonPartSensitive)
		{
			return _nonPartSensitive ? juce::String("Global")
				: juce::String("Track ") + juce::String(_part + 1);
		});

		// Give mute (which is not part of a Kit dump) a defined initial cache value.
		// The other values are replaced by the firmware snapshot below.
		for(const auto& [address, parameters] : getExposedParameters())
		{
			(void)address;
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(parameter->getDefault(),
					pluginLib::Parameter::Origin::PresetChange);
		}

		requestAutomationState();
	}

	Controller::~Controller() = default;

	uint8_t Controller::getPartCount() const
	{
		return m_model == md::MachineModel::Monomachine
			? md::automation::monomachine::TrackCount
			: md::automation::machinedrum::TrackCount;
	}

	pluginLib::Parameter* Controller::createParameter(
		pluginLib::Controller& _controller,
		const pluginLib::Description& _description, const uint8_t _part,
		const int _uid, const pluginLib::Parameter::PartFormatter& _formatter)
	{
		return new AutomationParameter(_controller, _description, _part, _uid,
			_formatter);
	}

	void Controller::onStateLoaded()
	{
		requestAutomationState();
	}

	std::vector<uint8_t> Controller::createAutomationSnapshot() const
	{
		if(!m_haveGlobal.load(std::memory_order_acquire)
			|| m_currentKit.load(std::memory_order_acquire) == 0xff)
			return {};
		std::vector<uint8_t> result;
		result.reserve(6 + getExposedParameters().size() * 4);
		result.push_back(1);
		result.push_back(static_cast<uint8_t>(m_model));
		result.push_back(getAutomationBaseChannel());
		result.push_back(m_currentKit.load(std::memory_order_acquire));
		const auto count = static_cast<uint16_t>(getExposedParameters().size());
		result.push_back(static_cast<uint8_t>(count >> 8));
		result.push_back(static_cast<uint8_t>(count & 0xff));
		for(const auto& [address, parameters] : getExposedParameters())
		{
			if(parameters.empty())
				continue;
			result.push_back(address.page);
			result.push_back(address.partNum);
			result.push_back(address.paramNum);
			result.push_back(static_cast<uint8_t>(std::clamp(
				parameters.front()->getUnnormalizedValue(), 0, 127)));
		}
		return result;
	}

	bool Controller::restoreAutomationSnapshot(
		const std::vector<uint8_t>& _snapshot)
	{
		if(_snapshot.size() < 6 || _snapshot[0] != 1
			|| _snapshot[1] != static_cast<uint8_t>(m_model))
			return false;
		const auto count = static_cast<size_t>((_snapshot[4] << 8) | _snapshot[5]);
		const auto kitCount = m_model == md::MachineModel::Monomachine ? 128 : 64;
		if(_snapshot.size() != 6 + count * 4
			|| count != getExposedParameters().size()
			|| !(_snapshot[2] < 16 || _snapshot[2] == 0x7f)
			|| _snapshot[3] >= kitCount)
			return false;
		std::set<Address> addresses;
		for(size_t position = 6; position < _snapshot.size(); position += 4)
		{
			const Address address{_snapshot[position], _snapshot[position + 1],
				_snapshot[position + 2]};
			if(_snapshot[position + 3] > 127 || !addresses.insert(address).second
				|| findSynthParam(address.track, address.page, address.index).empty())
				return false;
		}

		m_automationReady.store(false, std::memory_order_release);
		m_baseChannel.store(_snapshot[2], std::memory_order_release);
		m_currentKit.store(_snapshot[3], std::memory_order_release);
		std::map<Address, pluginLib::ParamValue> restored;
		for(size_t position = 6; position < _snapshot.size(); position += 4)
		{
			const Address address{_snapshot[position], _snapshot[position + 1],
				_snapshot[position + 2]};
			const auto& parameters = findSynthParam(_snapshot[position + 1],
				_snapshot[position], _snapshot[position + 2]);
			if(parameters.empty())
				return false;
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(_snapshot[position + 3],
					pluginLib::Parameter::Origin::PresetChange);
			restored[address] = _snapshot[position + 3];
		}
		{
			const std::lock_guard lock(m_pendingMutex);
			for(const auto& [address, value] : restored)
				m_pendingChanges[address] = value;
		}
		m_haveGlobal.store(false, std::memory_order_release);
		m_haveKit.store(false, std::memory_order_release);
		getProcessor().updateHostDisplay(
			juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
		return true;
	}

	void Controller::requestAutomationState()
	{
		m_automationReady.store(false, std::memory_order_release);
		m_haveGlobal.store(false, std::memory_order_release);
		m_haveKit.store(false, std::memory_order_release);
		m_currentGlobal.store(0xff, std::memory_order_release);
		m_currentKit.store(0xff, std::memory_order_release);
		m_globalDumpRequestMs.store(0, std::memory_order_release);
		m_kitDumpRequestMs.store(0, std::memory_order_release);
		sendMissingSynchronizationRequests();
	}

	void Controller::requestKitState()
	{
		m_automationReady.store(false, std::memory_order_release);
		m_haveKit.store(false, std::memory_order_release);
		m_kitDumpRequestMs.store(0, std::memory_order_release);
		sendMissingSynchronizationRequests();
	}

	uint64_t Controller::milliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void Controller::sendMissingSynchronizationRequests()
	{
		m_lastSynchronizationRequestMs.store(milliseconds(), std::memory_order_release);
		// Do not accumulate status requests in the plug-in MIDI queue while the
		// firmware DSPs are still booting. Once consumed, those duplicates can yield
		// late Kit dumps that overwrite host writes after synchronization completed.
		if(!firmwareReadyForAutomation())
			return;
		if(!m_haveGlobal.load(std::memory_order_acquire))
			sendSysEx(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Global)));
		if(!m_haveKit.load(std::memory_order_acquire))
			sendSysEx(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Kit)));
	}

	void Controller::onControllerTimer()
	{
		completeSynchronizationIfReady();
		const auto now = milliseconds();
		if(m_automationReady.load(std::memory_order_acquire))
		{
			// Firmware Global is authoritative for the MIDI channel, and a front-panel
			// kit selection bypasses the controller. Poll their tiny status messages;
			// only a changed kit number causes the much larger Kit dump.
			if(now - m_lastStatePollMs.load(std::memory_order_acquire) < 5000)
				return;
			m_lastStatePollMs.store(now, std::memory_order_release);
			sendSysEx(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Global)));
			sendSysEx(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Kit)));
			return;
		}
		if(now - m_lastSynchronizationRequestMs.load(std::memory_order_acquire) < 500)
			return;
		sendMissingSynchronizationRequests();
	}

	void Controller::sendParameterChange(const pluginLib::Parameter& _parameter,
		const pluginLib::ParamValue _value, pluginLib::Parameter::Origin)
	{
		const auto& description = _parameter.getDescription();
		const md::automation::ParameterChange change{
			description.page,
			_parameter.getPart(),
			description.index,
			static_cast<uint8_t>(std::clamp<pluginLib::ParamValue>(_value, 0, 127))
		};
		if(!m_automationReady.load(std::memory_order_acquire)
			|| getAutomationBaseChannel() == 0x7f)
		{
			const std::lock_guard lock(m_pendingMutex);
			if(!m_automationReady.load(std::memory_order_relaxed)
				|| getAutomationBaseChannel() == 0x7f)
			{
				m_pendingChanges[{change.page, change.track, change.index}] = _value;
				return;
			}
		}
		transmitParameterChange(change);
	}

	void Controller::transmitParameterChange(
		const md::automation::ParameterChange& _change)
	{
		if(const auto message = md::automation::encodeParameterChange(
			m_model, _change, getAutomationBaseChannel()))
		{
			auto observed = m_transmittedAutomationDigest.load(std::memory_order_relaxed);
			for(;;)
			{
				auto desired = observed;
				for(const auto byte : *message)
				{
					desired ^= byte;
					desired *= 1099511628211ull;
				}
				if(m_transmittedAutomationDigest.compare_exchange_weak(observed,
					desired, std::memory_order_release, std::memory_order_relaxed))
					break;
			}
			m_transmittedAutomationChanges.fetch_add(1, std::memory_order_release);
			sendMidiEvent((*message)[0], (*message)[1], (*message)[2]);
		}
	}

	void Controller::applyKitParameters(
		const std::vector<md::automation::ParameterChange>& _changes)
	{
		for(const auto& change : _changes)
		{
			const auto& parameters = findSynthParam(change.track, change.page,
				change.index);
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(change.value,
					pluginLib::Parameter::Origin::PresetChange);
		}
		getProcessor().updateHostDisplay(
			juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
	}

	void Controller::completeSynchronizationIfReady()
	{
		if(!m_haveGlobal.load(std::memory_order_acquire)
			|| !m_haveKit.load(std::memory_order_acquire)
			|| !firmwareReadyForAutomation())
			return;

		// Writes can arrive on the host audio thread while a dump is being applied.
		// Keep draining until no such write remains, then publish the lock-free ready
		// state. This preserves the newest host value over the older firmware snapshot.
		for(;;)
		{
			std::map<Address, pluginLib::ParamValue> pending;
			{
				const std::lock_guard lock(m_pendingMutex);
				if(m_pendingChanges.empty())
				{
					m_lastStatePollMs.store(milliseconds(), std::memory_order_release);
					m_automationReady.store(true, std::memory_order_release);
					return;
				}
				pending.swap(m_pendingChanges);
			}

			for(const auto& [address, value] : pending)
			{
				const auto clamped = static_cast<uint8_t>(
					std::clamp<pluginLib::ParamValue>(value, 0, 127));
				const auto& parameters = findSynthParam(address.track, address.page,
					address.index);
				for(auto* const parameter : parameters)
					parameter->setValueFromSynth(clamped,
						pluginLib::Parameter::Origin::HostAutomation);
				transmitParameterChange({address.page, address.track, address.index,
					clamped});
			}
		}
	}

	bool Controller::firmwareReadyForAutomation() const
	{
		bool ready = true;
		getProcessor().getPlugin().withDeviceLocked(
			[&ready](synthLib::Device* const _device)
			{
				if(const auto* const device = dynamic_cast<md::Device*>(_device))
					ready = device->getHardware().isAudioReady();
			});
		return ready;
	}

	bool Controller::parseSysexMessage(const pluginLib::SysEx& _message,
		synthLib::MidiEventSource)
	{
		if(const auto status = md::automation::sysex::parseStatusResponse(
			m_model, _message))
		{
			switch(status->parameter)
			{
			case md::automation::sysex::StatusParameter::Global:
			{
				const auto previous = m_currentGlobal.exchange(status->value,
					std::memory_order_acq_rel);
				const auto changed = previous != status->value;
				if(!m_haveGlobal.load(std::memory_order_acquire) || changed)
				{
					m_automationReady.store(false, std::memory_order_release);
					m_haveGlobal.store(false, std::memory_order_release);
					const auto now = milliseconds();
					const auto requested =
						m_globalDumpRequestMs.load(std::memory_order_acquire);
					if(changed || requested == 0
						|| now - requested >= g_dumpRequestRetryMs)
					{
						m_globalDumpRequestMs.store(now, std::memory_order_release);
						sendSysEx(toPluginSysex(md::automation::sysex::globalRequest(
							m_model, status->value)));
					}
				}
				return true;
			}
			case md::automation::sysex::StatusParameter::Kit:
			{
				const auto previous = m_currentKit.exchange(status->value,
					std::memory_order_acq_rel);
				const auto changed = previous != status->value;
				if(!m_haveKit.load(std::memory_order_acquire) || changed)
				{
					m_automationReady.store(false, std::memory_order_release);
					m_haveKit.store(false, std::memory_order_release);
					const auto now = milliseconds();
					const auto requested =
						m_kitDumpRequestMs.load(std::memory_order_acquire);
					if(changed || requested == 0
						|| now - requested >= g_dumpRequestRetryMs)
					{
						m_kitDumpRequestMs.store(now, std::memory_order_release);
						sendSysEx(toPluginSysex(md::automation::sysex::kitRequest(
							m_model, status->value)));
					}
				}
				return true;
			}
			case md::automation::sysex::StatusParameter::Pattern:
				return true;
			}
		}

		if(const auto channel = md::automation::sysex::parseBaseChannel(
			m_model, _message))
		{
			m_baseChannel.store(*channel, std::memory_order_release);
			m_haveGlobal.store(true, std::memory_order_release);
			m_globalDumpRequestMs.store(0, std::memory_order_release);
			completeSynchronizationIfReady();
			return true;
		}

		if(const auto parameters = md::automation::sysex::parseKitParameters(
			m_model, _message))
		{
			applyKitParameters(*parameters);
			m_haveKit.store(true, std::memory_order_release);
			m_kitDumpRequestMs.store(0, std::memory_order_release);
			completeSynchronizationIfReady();
			return true;
		}

		// External SET STATUS messages can change the active Global, selected Kit,
		// or Pattern without going through the controller. Refresh after the firmware
		// consumes the same queued MIDI event.
		if(_message.size() == 10 && _message[0] == 0xf0
			&& _message[4] == (m_model == md::MachineModel::Monomachine ? 0x03 : 0x02)
			&& _message[6] == 0x71)
		{
			if(_message[7] == static_cast<uint8_t>(
				md::automation::sysex::StatusParameter::Global))
			{
				m_currentGlobal.store(_message[8], std::memory_order_release);
				m_automationReady.store(false, std::memory_order_release);
				m_haveGlobal.store(false, std::memory_order_release);
				m_globalDumpRequestMs.store(milliseconds(), std::memory_order_release);
				sendSysEx(toPluginSysex(md::automation::sysex::globalRequest(
					m_model, _message[8])));
			}
			else if(_message[7] == static_cast<uint8_t>(
				md::automation::sysex::StatusParameter::Kit)
				|| _message[7] == static_cast<uint8_t>(
					md::automation::sysex::StatusParameter::Pattern))
			{
				requestKitState();
			}
		}
		return false;
	}

	bool Controller::parseControllerMessage(const synthLib::SMidiEvent& _event)
	{
		const auto change = md::automation::decodeParameterChange(m_model,
			{_event.a, _event.b, _event.c}, getAutomationBaseChannel());
		if(!change)
			return false;

		const auto& parameters = findSynthParam(change->track, change->page,
			change->index);
		if(parameters.empty())
			return false;
		const auto origin = midiEventSourceToParameterOrigin(_event.source);
		for(auto* const parameter : parameters)
			parameter->setValueFromSynth(change->value, origin);
		return true;
	}

	bool Controller::parseMidiMessage(const synthLib::SMidiEvent& _event)
	{
		const auto handled = pluginLib::Controller::parseMidiMessage(_event);
		// Device-origin Program Change is outgoing firmware MIDI (for example from
		// an MM MIDI machine), not an instruction selecting the plug-in's Kit.
		if(_event.source != synthLib::MidiEventSource::Device
			&& _event.sysex.empty() && (_event.a & 0xf0) == 0xc0)
			requestKitState();
		return handled;
	}
}
