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
		for(const auto& [address, parameters] : getExposedParameters())
		{
			(void)parameters;
			const Address automationAddress{address.page, address.partNum,
				address.paramNum};
			const auto slotIndex = m_automationSlots.size();
			m_automationSlots.emplace_back();
			auto& slot = m_automationSlots.back();
			slot.address = automationAddress;
			m_automationSlotIndices.emplace(automationAddress, slotIndex);
		}

		// Give mute (which is not part of a Kit dump) a defined initial cache value.
		// The other values are replaced by the firmware snapshot below.
		for(const auto& [address, parameters] : getExposedParameters())
		{
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(parameter->getDefault(),
					pluginLib::Parameter::Origin::PresetChange);
			if(auto* const slot = findAutomationSlot(
				{address.page, address.partNum, address.paramNum}))
			{
				slot->publication.store(createPublication(static_cast<uint8_t>(
					std::clamp(parameters.front()->getDefault(), 0, 127)), false),
					std::memory_order_release);
			}
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
		const auto epoch = m_synchronizationEpoch.load(std::memory_order_acquire);
		if(!m_automationReady.load(std::memory_order_acquire)
			|| !m_haveGlobal.load(std::memory_order_acquire)
			|| !m_haveKit.load(std::memory_order_acquire)
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
			const auto* const slot = findAutomationSlot(
				{address.page, address.partNum, address.paramNum});
			if(slot == nullptr)
				return {};
			result.push_back(address.page);
			result.push_back(address.partNum);
			result.push_back(address.paramNum);
			result.push_back(publicationValue(
				slot->publication.load(std::memory_order_acquire)));
		}
		if(!m_automationReady.load(std::memory_order_acquire)
			|| epoch != m_synchronizationEpoch.load(std::memory_order_acquire))
			return {};
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
		m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
		m_baseChannel.store(_snapshot[2], std::memory_order_release);
		m_currentKit.store(_snapshot[3], std::memory_order_release);
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
			publishAutomationIntent({address.page, address.track, address.index,
				_snapshot[position + 3]}, true);
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
		m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
		m_haveGlobal.store(false, std::memory_order_release);
		m_haveKit.store(false, std::memory_order_release);
		m_currentGlobal.store(0xff, std::memory_order_release);
		m_currentKit.store(0xff, std::memory_order_release);
		m_globalDumpRequestMs.store(0, std::memory_order_release);
		m_kitDumpRequestMs.store(0, std::memory_order_release);
		m_kitDumpRequestRevision.store(0, std::memory_order_release);
		sendMissingSynchronizationRequests();
	}

	void Controller::requestKitState()
	{
		m_automationReady.store(false, std::memory_order_release);
		m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
		m_haveKit.store(false, std::memory_order_release);
		m_kitDumpRequestMs.store(0, std::memory_order_release);
		m_kitDumpRequestRevision.store(0, std::memory_order_release);
		sendMissingSynchronizationRequests();
	}

	uint64_t Controller::milliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void Controller::sendMissingSynchronizationRequests()
	{
		// Do not accumulate status requests in the plug-in MIDI queue while the
		// firmware DSPs are still booting. Once consumed, those duplicates can yield
		// late Kit dumps that overwrite host writes after synchronization completed.
		if(!firmwareReadyForAutomation())
			return;
		// A boot-time no-op is not a request and must not start the retry interval.
		// Leaving the timestamp at zero makes the first timer tick after the DSPs
		// become ready send immediately, including in faster-than-realtime renders.
		m_lastSynchronizationRequestMs.store(milliseconds(), std::memory_order_release);
		if(!m_haveGlobal.load(std::memory_order_acquire))
			sendSynchronizationRequest(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Global)));
		if(!m_haveKit.load(std::memory_order_acquire))
			sendSynchronizationRequest(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Kit)));
	}

	void Controller::sendSynchronizationRequest(const pluginLib::SysEx& _message) const
	{
		// Editor is the routable controller-to-device source. Hardware recognizes
		// these exact read-only requests and keeps them factory-baseline-neutral.
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Editor);
		event.sysex = _message;
		m_synchronizationRequests.fetch_add(1, std::memory_order_relaxed);
		sendMidiEvent(event);
	}

	void Controller::onControllerTimer()
	{
		drainRealtimeParameterChanges(RealtimeAutomationCapacity, false);
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
			sendSynchronizationRequest(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Global)));
			sendSynchronizationRequest(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Kit)));
			// A status response identifies the active Global slot, but does not expose
			// edits to that Global's MIDI base channel. Refresh the selected Global too
			// so queued writes survive MIDI NONE and resume when a channel is enabled.
			const auto currentGlobal = m_currentGlobal.load(std::memory_order_acquire);
			const auto requested = m_globalDumpRequestMs.load(std::memory_order_acquire);
			if(currentGlobal != 0xff && (requested == 0
				|| now - requested >= g_dumpRequestRetryMs))
			{
				m_globalDumpRequestMs.store(now, std::memory_order_release);
				sendSynchronizationRequest(toPluginSysex(md::automation::sysex::globalRequest(
					m_model, currentGlobal)));
			}
			return;
		}
		if(now - m_lastSynchronizationRequestMs.load(std::memory_order_acquire) < 500)
			return;
		sendMissingSynchronizationRequests();
	}

	void Controller::sendParameterChange(const pluginLib::Parameter& _parameter,
		const pluginLib::ParamValue _value, const pluginLib::Parameter::Origin _origin)
	{
		const auto& description = _parameter.getDescription();
		const md::automation::ParameterChange change{
			description.page,
			_parameter.getPart(),
			description.index,
			static_cast<uint8_t>(std::clamp<pluginLib::ParamValue>(_value, 0, 127))
		};
		publishAutomationIntent(change,
			_origin != pluginLib::Parameter::Origin::HostAutomation
				|| !m_automationReady.load(std::memory_order_acquire));
		// UI changes use exactly the same ordered publication path as host
		// automation. A non-realtime caller may drain immediately, while a host
		// callback only performs the bounded publication and returns.
		if(_origin != pluginLib::Parameter::Origin::HostAutomation)
			drainRealtimeParameterChanges(RealtimeAutomationCapacity, false);
	}

	uint8_t Controller::publicationValue(const uint64_t _publication)
	{
		return static_cast<uint8_t>(_publication & PublicationValueMask);
	}

	uint64_t Controller::publicationRevision(const uint64_t _publication)
	{
		return (_publication & PublicationRevisionMask) >> 8;
	}

	bool Controller::publicationIsDirty(const uint64_t _publication)
	{
		return (_publication & PublicationDirty) != 0;
	}

	uint64_t Controller::createPublication(const uint8_t _value, const bool _dirty)
	{
		const auto revision = m_nextAutomationRevision.fetch_add(
			1, std::memory_order_relaxed);
		return (_dirty ? PublicationDirty : 0)
			| ((revision << 8) & PublicationRevisionMask)
			| (_value & PublicationValueMask);
	}

	void Controller::publishAutomationIntent(
		const md::automation::ParameterChange& _change,
		const bool _supersedeEarlier)
	{
		const auto found = m_automationSlotIndices.find(
			{_change.page, _change.track, _change.index});
		if(found == m_automationSlotIndices.end())
			return;

		const auto publication = createPublication(_change.value, true);
		auto& slot = m_automationSlots[found->second];
		slot.publication.exchange(publication, std::memory_order_acq_rel);
		const auto advanceDeliveryFloor = [&slot](const uint64_t _revision)
		{
			// Keep the realtime producer strictly bounded: a fixed number of strong
			// attempts can only all fail under sustained same-address contention.
			// Failure to advance merely permits an extra stale value before the latest;
			// it cannot lose or overwrite the authoritative publication.
			auto floor = slot.deliveryFloorRevision.load(std::memory_order_acquire);
			for(size_t attempt = 0; attempt < 8 && floor < _revision; ++attempt)
			{
				if(slot.deliveryFloorRevision.compare_exchange_strong(floor, _revision,
					std::memory_order_release, std::memory_order_acquire))
					return;
			}
		};
		if(_supersedeEarlier)
			advanceDeliveryFloor(publicationRevision(publication));
		if(!m_realtimeAutomationChanges.tryPush(
			{_change, found->second, publication}))
		{
			// The atomic slot remains authoritative. A bounded slot scan will deliver
			// it even when queue capacity or producer contention drops this hint. Once
			// a hint is missing, older queued values can no longer form a complete FIFO
			// stream, so explicitly coalesce them behind the recovered latest value.
			advanceDeliveryFloor(publicationRevision(publication));
			slot.scanPublication.store(publication, std::memory_order_release);
			m_realtimeAutomationOverflows.fetch_add(1, std::memory_order_relaxed);
		}
	}

	Controller::AutomationSlot* Controller::findAutomationSlot(
		const Address& _address)
	{
		const auto found = m_automationSlotIndices.find(_address);
		return found == m_automationSlotIndices.end()
			? nullptr : &m_automationSlots[found->second];
	}

	const Controller::AutomationSlot* Controller::findAutomationSlot(
		const Address& _address) const
	{
		const auto found = m_automationSlotIndices.find(_address);
		return found == m_automationSlotIndices.end()
			? nullptr : &m_automationSlots[found->second];
	}

	uint8_t Controller::publishFirmwareValue(const Address& _address,
		const uint8_t _value, const uint64_t _kitRequestRevision)
	{
		auto* const slot = findAutomationSlot(_address);
		if(slot == nullptr)
			return _value;

		const auto desired = createPublication(_value, false);
		auto observed = slot->publication.load(std::memory_order_acquire);
		for(;;)
		{
			// An undelivered DAW/UI intent always survives a dump. For Kit dumps,
			// direct changes observed after the request watermark survive as well.
			// The watermark is valid because both the read request and later editor/host
			// MIDI enter synthLib::Plugin's FIFO in publication order; processBlock
			// appends its bounded realtime insertions after general ingress already
			// queued for that block. Firmware therefore cannot observe the later change
			// before the earlier request.
			if(publicationIsDirty(observed)
				|| (_kitRequestRevision != 0
					&& publicationRevision(observed) > _kitRequestRevision))
				return publicationValue(observed);
			if(slot->publication.compare_exchange_weak(observed, desired,
				std::memory_order_release, std::memory_order_acquire))
				return _value;
		}
	}

	void Controller::transmitParameterChange(
		const md::automation::ParameterChange& _change)
	{
		if(const auto message = md::automation::encodeParameterChange(
			m_model, _change, getAutomationBaseChannel()))
		{
			auto digest = m_transmittedAutomationDigest.load(std::memory_order_relaxed);
			for(const auto byte : *message)
			{
				digest ^= byte;
				digest *= 1099511628211ull;
			}
			m_transmittedAutomationDigest.store(digest, std::memory_order_release);
			m_transmittedAutomationChanges.fetch_add(1, std::memory_order_release);
			sendMidiEvent((*message)[0], (*message)[1], (*message)[2]);
		}
	}

	bool Controller::transmitRealtimeParameterChange(
		const md::automation::ParameterChange& _change)
	{
		const auto message = md::automation::encodeParameterChange(
			m_model, _change, getAutomationBaseChannel());
		if(!message)
			return true;
		const synthLib::SMidiEvent event(synthLib::MidiEventSource::Editor,
			(*message)[0], (*message)[1], (*message)[2]);
		if(!getProcessor().tryAddRealtimeMidiEvent(event))
			return false;

		auto digest = m_transmittedAutomationDigest.load(std::memory_order_relaxed);
		for(const auto byte : *message)
		{
			digest ^= byte;
			digest *= 1099511628211ull;
		}
		m_transmittedAutomationDigest.store(digest, std::memory_order_release);
		m_transmittedAutomationChanges.fetch_add(1, std::memory_order_release);
		return true;
	}

	bool Controller::deliverAutomationPublication(
		const QueuedAutomationChange& _queued, const bool _realtime)
	{
		if(_queued.slotIndex >= m_automationSlots.size())
			return true;
		auto& slot = m_automationSlots[_queued.slotIndex];
		auto observed = slot.publication.load(std::memory_order_acquire);
		const auto queuedRevision = publicationRevision(_queued.publication);
		if(queuedRevision < slot.deliveryFloorRevision.load(std::memory_order_acquire))
			return true;
		if(observed != _queued.publication)
		{
			// A later ordinary DAW publication does not invalidate this FIFO entry.
			// Transmit the older value now and leave the latest dirty publication for
			// its own hint. If the latest was already delivered, its clean state proves
			// this reordered/stale hint must instead be ignored.
			if(!publicationIsDirty(observed)
				|| publicationRevision(observed) <= queuedRevision)
				return true;
		}
		else if(!publicationIsDirty(observed))
			return true;
		if(!m_automationReady.load(std::memory_order_acquire)
			|| getAutomationBaseChannel() == 0x7f)
			return true;

		if(_realtime)
		{
			if(!transmitRealtimeParameterChange(_queued.change))
				return false;
		}
		else
		{
			transmitParameterChange(_queued.change);
		}

		if(observed == _queued.publication)
		{
			// Clear the delivery bit only if no newer publication replaced this one
			// while MIDI was being queued. A failed CAS leaves that newer value dirty.
			const auto delivered = observed & ~PublicationDirty;
			slot.publication.compare_exchange_strong(observed, delivered,
				std::memory_order_release, std::memory_order_acquire);
		}
		return true;
	}

	void Controller::drainRealtimeParameterChanges(const size_t _maximumChanges,
		const bool _realtime)
	{
		if(_maximumChanges == 0
			|| m_realtimeAutomationDrain.test_and_set(std::memory_order_acquire))
			return;
		struct ClearFlag
		{
			std::atomic_flag& flag;
			~ClearFlag() { flag.clear(std::memory_order_release); }
		} clear{m_realtimeAutomationDrain};

		const auto routable = m_automationReady.load(std::memory_order_acquire)
			&& getAutomationBaseChannel() != 0x7f && !m_automationSlots.empty();
		// A successful hint is the only way to preserve an exact DAW sequence. Do not
		// consume it while firmware routing is unavailable; synchronization completion
		// (or a later MIDI-channel enable) will drain the intact FIFO. Failed hints have
		// their separate recovery marker and remain safe if this queue fills meanwhile.
		if(!routable)
			return;
		// Always reserve part of a routable callback for recovery-marked slots.
		// A full queue can contain thousands of stale hints for one address; without
		// this reservation, an unhinted latest value could wait for all of them.
		size_t reservedScan = routable ? std::min(m_automationSlots.size(),
			std::max<size_t>(1, _maximumChanges / 4)) : size_t{0};
		if(routable && _maximumChanges == 1)
		{
			// A one-event caller cannot serve the FIFO and recovery scan in one pass.
			// Alternate them so neither source can starve; larger budgets serve both.
			reservedScan = m_minimumBudgetRecoveryTurn ? 1 : 0;
			m_minimumBudgetRecoveryTurn = !m_minimumBudgetRecoveryTurn;
		}
		const auto queueLimit = _maximumChanges - reservedScan;
		size_t processed = 0;
		bool queueEmpty = false;
		while(processed < queueLimit)
		{
			QueuedAutomationChange queued;
			if(!m_realtimeAutomationChanges.tryPop(queued))
			{
				queueEmpty = true;
				break;
			}
			if(!deliverAutomationPublication(queued, _realtime))
				return;
			++processed;
		}

		// Queue overflow and producer contention only drop hints. Scan a bounded
		// rotating slice for explicit recovery markers, even while hints remain. Do
		// not deliver ordinary dirty slots from the scan: jumping their healthy FIFO
		// hints would incorrectly coalesce explicit host writes. If the queue
		// empties early, spend the unused budget on the scan. Thus a dirty slot is
		// reconsidered within ceil(slot-count / reserved-scan) callbacks regardless
		// of stale queue depth, while total callback work never exceeds the caller's
		// explicit maximum.
		const auto scanLimit = std::min(m_automationSlots.size(),
			queueEmpty ? _maximumChanges - processed : reservedScan);
		size_t inspected = 0;
		while(inspected < scanLimit)
		{
			if(m_dirtyScanPosition >= m_automationSlots.size())
				m_dirtyScanPosition = 0;
			const auto slotIndex = m_dirtyScanPosition++;
			auto& slot = m_automationSlots[slotIndex];
			auto recovery = slot.scanPublication.load(std::memory_order_acquire);
			if(recovery != 0)
			{
				const auto publication = slot.publication.load(std::memory_order_acquire);
				if(publicationIsDirty(publication))
				{
					const auto& address = slot.address;
					if(!deliverAutomationPublication({
						{address.page, address.track, address.index,
							publicationValue(publication)}, slotIndex, publication}, _realtime))
						return;
				}
				// Retain the marker until the FIFO has actually been observed empty, so a
				// later same-slot host write cannot disappear behind the known stale backlog.
				// CAS still prevents an older scan from clearing a replacement marker.
				if(queueEmpty && !publicationIsDirty(
					slot.publication.load(std::memory_order_acquire)))
					slot.scanPublication.compare_exchange_strong(recovery, 0,
						std::memory_order_release, std::memory_order_acquire);
			}
			++inspected;
		}
	}

	void Controller::processRealtimeParameterChanges(const size_t _maximumChanges)
	{
		drainRealtimeParameterChanges(_maximumChanges, true);
	}

	void Controller::applyKitParameters(
		const std::vector<md::automation::ParameterChange>& _changes)
	{
		const auto requestRevision = m_kitDumpRequestRevision.load(
			std::memory_order_acquire);
		for(const auto& change : _changes)
		{
			const auto value = publishFirmwareValue(
				{change.page, change.track, change.index}, change.value,
				requestRevision);
			const auto& parameters = findSynthParam(change.track, change.page,
				change.index);
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(value,
					pluginLib::Parameter::Origin::PresetChange);
		}
		getProcessor().updateHostDisplay(
			juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
	}

	void Controller::completeSynchronizationIfReady()
	{
		// Requests are withheld while the DSPs boot or project restore is pending.
		// Once both strictly correlated replies arrive, those replies themselves are
		// the readiness proof; consulting asynchronous hardware state again here can
		// only delay publication of an otherwise coherent snapshot.
		if(!m_haveGlobal.load(std::memory_order_acquire)
			|| !m_haveKit.load(std::memory_order_acquire))
			return;
		if(getAutomationBaseChannel() == 0x7f)
		{
			// MIDI NONE is a valid firmware setting. The cache may become ready for
			// reads, but pending DAW intent must remain intact until a routable Global
			// dump is observed.
			m_lastStatePollMs.store(milliseconds(), std::memory_order_release);
			m_automationReady.store(true, std::memory_order_release);
			return;
		}

		m_lastStatePollMs.store(milliseconds(), std::memory_order_release);
		m_automationReady.store(true, std::memory_order_release);
		// Once ready is visible, one serialized non-realtime drain delivers both
		// queued hints and every dirty slot missed because the queue was full.
		drainRealtimeParameterChanges(
			RealtimeAutomationCapacity + m_automationSlots.size(), false);
	}

	bool Controller::firmwareReadyForAutomation() const
	{
		bool ready = true;
		getProcessor().getPlugin().withDeviceLocked(
			[&ready](synthLib::Device* const _device)
			{
				if(const auto* const device = dynamic_cast<md::Device*>(_device))
				{
					const auto& hardware = device->getHardware();
					const auto panel = hardware.getFrontPanelSnapshot();
					ready = hardware.isAudioReady()
						// The DSPs become runnable before the main firmware has finished
						// initializing its peripherals. Use the same observable
						// front-panel boot boundary as the ROM integration tools rather
						// than assuming DSP audio readiness also means MIDI readiness.
						&& (panel.countLitPixels() > 2000
							|| (panel.getTileWriteCount() >= 64
								&& panel.countLitPixels() >= 100))
						&& !hardware.isProjectStateRestorePending();
				}
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
					m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
					m_haveGlobal.store(false, std::memory_order_release);
					const auto now = milliseconds();
					const auto requested =
						m_globalDumpRequestMs.load(std::memory_order_acquire);
					if(changed || requested == 0
						|| now - requested >= g_dumpRequestRetryMs)
					{
						m_globalDumpRequestMs.store(now, std::memory_order_release);
						sendSynchronizationRequest(toPluginSysex(md::automation::sysex::globalRequest(
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
					m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
					m_haveKit.store(false, std::memory_order_release);
					const auto now = milliseconds();
					const auto requested =
						m_kitDumpRequestMs.load(std::memory_order_acquire);
					if(changed || requested == 0
						|| now - requested >= g_dumpRequestRetryMs)
					{
						if(changed || requested == 0)
						{
							const auto next = m_nextAutomationRevision.load(
								std::memory_order_acquire);
							m_kitDumpRequestRevision.store(next > 0 ? next - 1 : 0,
								std::memory_order_release);
						}
						m_kitDumpRequestMs.store(now, std::memory_order_release);
						sendSynchronizationRequest(toPluginSysex(md::automation::sysex::kitRequest(
							m_model, status->value)));
					}
				}
				return true;
			}
			case md::automation::sysex::StatusParameter::Pattern:
				return true;
			}
		}

		if(const auto global = md::automation::sysex::parseGlobalDump(
			m_model, _message))
		{
			if(global->slot != m_currentGlobal.load(std::memory_order_acquire)
				|| m_globalDumpRequestMs.load(std::memory_order_acquire) == 0)
				return true;
			// A periodic refresh may update the base channel while an otherwise-ready
			// snapshot is being serialized. Invalidate first so the snapshot either
			// observes the old generation in full or is rejected and retried.
			m_automationReady.store(false, std::memory_order_release);
			m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
			m_baseChannel.store(global->baseChannel, std::memory_order_release);
			m_haveGlobal.store(true, std::memory_order_release);
			m_globalDumpRequestMs.store(0, std::memory_order_release);
			completeSynchronizationIfReady();
			return true;
		}

		if(const auto kit = md::automation::sysex::parseKitDump(
			m_model, _message))
		{
			if(kit->slot != m_currentKit.load(std::memory_order_acquire)
				|| m_kitDumpRequestMs.load(std::memory_order_acquire) == 0)
				return true;
			applyKitParameters(kit->parameters);
			m_haveKit.store(true, std::memory_order_release);
			m_kitDumpRequestMs.store(0, std::memory_order_release);
			m_kitDumpRequestRevision.store(0, std::memory_order_release);
			completeSynchronizationIfReady();
			return true;
		}

		// External SET STATUS messages can change the active Global, selected Kit,
		// or Pattern without going through the controller. Refresh after the firmware
		// consumes the same queued MIDI event.
		if(const auto status = md::automation::sysex::parseSetStatus(m_model, _message))
		{
			if(status->parameter == md::automation::sysex::StatusParameter::Global)
			{
				m_currentGlobal.store(status->value, std::memory_order_release);
				m_automationReady.store(false, std::memory_order_release);
				m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
				m_haveGlobal.store(false, std::memory_order_release);
				m_globalDumpRequestMs.store(milliseconds(), std::memory_order_release);
				sendSynchronizationRequest(toPluginSysex(md::automation::sysex::globalRequest(
					m_model, status->value)));
			}
			else if(status->parameter == md::automation::sysex::StatusParameter::Kit
				|| status->parameter == md::automation::sysex::StatusParameter::Pattern)
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
		const auto value = publishFirmwareValue(
			{change->page, change->track, change->index}, change->value);
		const auto origin = midiEventSourceToParameterOrigin(_event.source);
		for(auto* const parameter : parameters)
			parameter->setValueFromSynth(value, origin);
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
