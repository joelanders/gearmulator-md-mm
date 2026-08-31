#pragma once

#include "jucePluginLib/controller.h"
#include "mdLib/mdautomation.h"
#include "mdLib/mdtypes.h"

#include <atomic>
#include <map>
#include <mutex>

namespace mdJucePlugin
{
	class AudioPluginAudioProcessor;

	class Controller : public pluginLib::Controller
	{
	public:
		explicit Controller(AudioPluginAudioProcessor& _p);
		~Controller() override;

		void onStateLoaded() override;

		uint8_t getPartCount() const override;

		bool parseSysexMessage(const pluginLib::SysEx&,
			synthLib::MidiEventSource) override;
		bool parseControllerMessage(const synthLib::SMidiEvent& _event) override;
		bool parseMidiMessage(const synthLib::SMidiEvent& _event) override;

		void sendParameterChange(const pluginLib::Parameter& _parameter,
			pluginLib::ParamValue _value, pluginLib::Parameter::Origin _origin) override;

		bool isAutomationSynchronized() const
		{
			return m_automationReady.load(std::memory_order_acquire);
		}
		uint8_t getAutomationBaseChannel() const
		{
			return m_baseChannel.load(std::memory_order_acquire);
		}
		bool hasAutomationGlobalSnapshot() const
		{
			return m_haveGlobal.load(std::memory_order_acquire);
		}
		bool hasAutomationKitSnapshot() const
		{
			return m_haveKit.load(std::memory_order_acquire);
		}
		uint64_t getTransmittedAutomationChangeCount() const
		{
			return m_transmittedAutomationChanges.load(std::memory_order_acquire);
		}
		uint64_t getTransmittedAutomationDigest() const
		{
			return m_transmittedAutomationDigest.load(std::memory_order_acquire);
		}
		void requestAutomationState();
		std::vector<uint8_t> createAutomationSnapshot() const;
		bool restoreAutomationSnapshot(const std::vector<uint8_t>& _snapshot);

	private:
		struct Address
		{
			uint8_t page;
			uint8_t track;
			uint8_t index;

			bool operator<(const Address& _other) const
			{
				if(page != _other.page) return page < _other.page;
				if(track != _other.track) return track < _other.track;
				return index < _other.index;
			}
		};

		pluginLib::Parameter* createParameter(pluginLib::Controller& _controller,
			const pluginLib::Description& _description, uint8_t _part, int _uid,
			const pluginLib::Parameter::PartFormatter& _formatter) override;
		void requestKitState();
		void transmitParameterChange(const md::automation::ParameterChange& _change);
		void completeSynchronizationIfReady();
		bool firmwareReadyForAutomation() const;
		void applyKitParameters(const std::vector<md::automation::ParameterChange>& _changes);
		void onControllerTimer() override;
		void sendMissingSynchronizationRequests();
		void sendSynchronizationRequest(const pluginLib::SysEx& _message) const;
		static uint64_t milliseconds();

		const md::MachineModel m_model;
		std::atomic<uint8_t> m_baseChannel{0x7f};
		std::atomic<bool> m_haveGlobal{false};
		std::atomic<bool> m_haveKit{false};
		std::atomic<bool> m_automationReady{false};
		std::atomic<uint64_t> m_lastSynchronizationRequestMs{0};
		std::atomic<uint64_t> m_lastStatePollMs{0};
		std::atomic<uint64_t> m_globalDumpRequestMs{0};
		std::atomic<uint64_t> m_kitDumpRequestMs{0};
		std::atomic<uint64_t> m_transmittedAutomationChanges{0};
		std::atomic<uint64_t> m_transmittedAutomationDigest{14695981039346656037ull};
		std::atomic<uint8_t> m_currentGlobal{0xff};
		std::atomic<uint8_t> m_currentKit{0xff};
		std::mutex m_pendingMutex;
		std::map<Address, pluginLib::ParamValue> m_pendingChanges;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Controller)
	};
}
