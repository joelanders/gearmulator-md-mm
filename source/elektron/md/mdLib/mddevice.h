#pragma once

#include <memory>
#include <utility>

#include "mdhardware.h"

#include "synthLib/device.h"

namespace md
{
	struct DevicePreparedStateTestAccess;

	class Device : public synthLib::Device
	{
	public:
		class PreparationContext
		{
		public:
			PreparationContext(const PreparationContext&) = delete;
			PreparationContext& operator=(const PreparationContext&) = delete;
			~PreparationContext() = default;

		private:
			friend class Device;
			friend struct DevicePreparedStateTestAccess;

			PreparationContext(const synthLib::DeviceCreateParams& _params,
				MachineModel _model, std::shared_ptr<FrontPanelPublisher> _frontPanelPublisher,
				std::shared_ptr<MidiSysexTransferProgressPublisher> _midiSysexProgressPublisher)
				: m_romData(_params.romData)
				, m_romName(_params.romName)
				, m_model(_model)
				, m_frontPanelPublisher(std::move(_frontPanelPublisher))
				, m_midiSysexProgressPublisher(std::move(_midiSysexProgressPublisher))
			{
			}

			// Preparation may continue after the originating Device is replaced, so this
			// context must own ROM bytes supplied in DeviceCreateParams. That adds exactly
			// params.romData.size() persistent bytes (8 MiB for an in-memory MD/MM ROM,
			// zero for the normal filesystem-discovery path). Capturing the shared context
			// itself remains O(1); changing Hardware/Rom to shared immutable storage is a
			// separate, wider memory-layout change.
			const std::vector<uint8_t> m_romData;
			const std::string m_romName;
			const MachineModel m_model;
			const std::shared_ptr<FrontPanelPublisher> m_frontPanelPublisher;
			const std::shared_ptr<MidiSysexTransferProgressPublisher>
				m_midiSysexProgressPublisher;
		};

		class PreparedState
		{
		public:
			PreparedState(const PreparedState&) = delete;
			PreparedState& operator=(const PreparedState&) = delete;
			PreparedState(PreparedState&&) noexcept = default;
			PreparedState& operator=(PreparedState&&) noexcept = default;
			~PreparedState() = default;

		private:
			friend class Device;
			friend struct DevicePreparedStateTestAccess;

			PreparedState(std::shared_ptr<const PreparationContext> _context,
				std::unique_ptr<Hardware> _hardware)
				: m_context(std::move(_context)), m_hardware(std::move(_hardware))
			{
			}

			std::shared_ptr<const PreparationContext> m_context;
			std::unique_ptr<Hardware> m_hardware;
			bool m_committed = false;
		};

		Device(const synthLib::DeviceCreateParams& _params,
			const std::vector<uint8_t>& _initialPatchRam = {});

		float getSamplerate() const override;
		bool isValid() const override;
		bool getState(std::vector<uint8_t>& _state, synthLib::StateType _type) override;
		bool setState(const std::vector<uint8_t>& _state, synthLib::StateType _type) override;
		std::shared_ptr<const PreparationContext> getPreparationContext() const
		{
			return m_preparationContext;
		}
		static std::unique_ptr<PreparedState> prepareState(
			std::shared_ptr<const PreparationContext> _context,
			const std::vector<uint8_t>& _state, synthLib::StateType _type);
		// Requires exclusive access to this Device. A successful exchange leaves the
		// retired Hardware in _prepared so its destruction can happen after the
		// caller releases any process/control lock.
		bool commitPreparedState(PreparedState& _prepared);
		uint32_t getChannelCountIn() override;
		uint32_t getChannelCountOut() override;
		bool setDspClockPercent(uint32_t _percent) override;
		uint32_t getDspClockPercent() const override;
		uint64_t getDspClockHz() const override;
		MachineModel getModel() const { return m_model; }
		void setNativeProgramChangesEnabled(bool _enabled) { m_nativeProgramChangesEnabled = _enabled; }
		bool nativeProgramChangesEnabled() const { return m_nativeProgramChangesEnabled; }
		FrontPanel getFrontPanelSnapshot() const { return m_frontPanelPublisher->read(); }
		MidiSysexTransferProgress getMidiSysexTransferProgress() const
		{
			return m_midiSysexProgressPublisher->read();
		}

		// Direct access for the in-process editor (option B): the front-panel LCD/LED state
		// and panel-event injection. Only valid for a local (non-bridged) device instance.
		Hardware& getHardware() { return *m_hardware; }
		const Hardware& getHardware() const { return *m_hardware; }

	protected:
		void readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut) override;
		void processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, size_t _samples) override;
		bool sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response) override;

	private:
		friend struct DevicePreparedStateTestAccess;

		const MachineModel m_model;
		std::shared_ptr<FrontPanelPublisher> m_frontPanelPublisher;
		std::shared_ptr<MidiSysexTransferProgressPublisher> m_midiSysexProgressPublisher;
		std::shared_ptr<const PreparationContext> m_preparationContext;
		std::unique_ptr<Hardware> m_hardware;
		uint32_t m_numSamplesProcessed = 0;
		bool m_nativeProgramChangesEnabled = false;
	};
}
