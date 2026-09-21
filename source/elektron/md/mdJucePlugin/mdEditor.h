#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

#include "jucePluginEditorLib/pluginEditor.h"

#include "mdFrontPanelPresentation.h"
#include "mdHelpOverrides.h"
#include "mdLcdGesture.h"
#include "mdLcdInteractionModel.h"
#include "mdPanelAffordances.h"
#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdsyseximport.h"

#include "juce_gui_basics/juce_gui_basics.h"

namespace juce
{
	class Image;
	class Graphics;
}

namespace Rml
{
	class Element;
	class Event;
}

namespace juceRmlUi
{
	class ElemButton;
	class ElemCanvas;
	class ElemKnob;
}

namespace md
{
	class Hardware;
}

namespace mdJucePlugin
{
	class Controller;
	class PixelPerfectPanel;
	struct EditorIdentityTestAccess;

	namespace parameterHelp
	{
		struct Entry;
		struct ValueHash;
	}

	class Editor final : public jucePluginEditorLib::Editor, juce::MultiTimer,
		private juce::FocusChangeListener
	{
	public:
		Editor(jucePluginEditorLib::Processor& _processor, const jucePluginEditorLib::Skin& _skin);
		~Editor() override;

		Editor(Editor&&) = delete;
		Editor(const Editor&) = delete;
		Editor& operator = (Editor&&) = delete;
		Editor& operator = (const Editor&) = delete;

		void create() override;

		std::pair<std::string, std::string> getDemoRestrictionText() const override;

		std::unique_ptr<jucePluginEditorLib::SettingsDeviceSpecific> createDeviceSpecificSettings(
			const std::string& _templateName, Rml::Element* _root) override;
		std::string getSettingsTemplateSuffix() const override;

		// Reapplies the configured wheel/encoder drag-speed percentages to the
		// panel knobs. Called on create and from the settings page.
		void applyPanelSpeeds();
		void applyPixelPerfectPanel();
		void applyLcdInteraction();
		// Rereads the tooltip on/off and pop-up delay settings. Called on create and from the settings page.
		void applyTooltipSettings();
		void loadInstalledFactoryStorage();
		void chooseStorageImage();
		void restorePreviousStorage();
		bool hasStorageRecoveryImage() const;
		void chooseUserSysexFile();
		void cancelUserSysexTransfer();
		bool canResumeUserSysexTransfer() const;
		void resumeUserSysexTransfer();
		std::string getUserSysexMenuText() const;
		bool isUserSysexTransferActive() const;
		bool canCancelUserSysexTransfer() const;
		std::weak_ptr<void> getLifetimeToken() const { return m_lifetimeToken; }

		static constexpr int g_panelSpeedPercents[] = {50, 75, 100, 150, 200, 300};
		static constexpr int g_tooltipDelaysMs[] = {0, 250, 500, 1000, 2000};
		static constexpr int g_defaultTooltipDelayMs = 500;
		static constexpr const char* g_tooltipsEnabledKey = "tooltipsEnabled";
		static constexpr const char* g_tooltipDelayKey = "tooltipDelayMs";

	private:
		friend struct EditorIdentityTestAccess;

		void timerCallback(int _timerId) override;

		std::shared_ptr<md::FrontPanelPublisher> getFrontPanelPublisher() const;
		bool sendPanelEvent(uint8_t _command, uint8_t _argument) const;
		bool refreshFrontPanelState(double _nowMilliseconds);
		md::MachineModel getModel() const;
		void createLcd();
		void updateLcdInteractionState();
		std::optional<unsigned> lcdTargetAt(const Rml::Event& _event) const;
		// Mouse position in native LCD pixels, or nothing outside the drawn display.
		std::optional<std::pair<int, int>> lcdNativePointAt(const Rml::Event& _event) const;
		void updateLcdHover(const Rml::Event& _event);
		void clearLcdHover();
		// DATA PAGE currently lit on the Monomachine: 0 SYNTHESIS .. 6 LFO 3.
		std::optional<int> currentMonomachineDataPage() const;
		// Page currently lit on the Machinedrum: 0 SYNTHESIS, 1 EFFECTS, 2 ROUTING.
		std::optional<int> currentMachinedrumDataPage() const;
		// Hover help for the cryptic parameter abbreviations, read off the LCD.
		struct TooltipText
		{
			std::string abbreviation;	// as the LCD shows it, e.g. ATK
			std::string name;
			std::string description;
			std::string footer;			// where it is, e.g. "Amplification page, knob A"
		};
		// What the pointer is over: a knob or LCD field (encoder), or the machine name on the LCD.
		struct TooltipTarget
		{
			Rml::Element* anchor = nullptr;	// the tooltip is shown under this; null for nothing
			std::optional<unsigned> encoder;
			bool machineName = false;

			bool operator==(const TooltipTarget& _other) const
			{
				return anchor == _other.anchor && encoder == _other.encoder && machineName == _other.machineName;
			}
			bool operator!=(const TooltipTarget& _other) const { return !(*this == _other); }
		};
		void createParameterTooltip();
		void updateParameterTooltip();
		void hideParameterTooltip();
		TooltipTarget tooltipTarget() const;
		// The help for _target on the current screen, or nothing if it isn't recognised.
		std::optional<TooltipText> describe(const TooltipTarget& _target) const;
		std::optional<TooltipText> describeMachineName() const;
		std::optional<TooltipText> describeMachinedrumEncoder(unsigned _encoder) const;
		std::optional<TooltipText> describeMonomachineEncoder(unsigned _encoder) const;
		// On a Monomachine LFO page: " Now: ..." text for PAGE (encoder 0) or DEST (encoder 1).
		std::string lfoTargetDescription(unsigned _encoder) const;
		// A table entry with any user overrides of its text applied.
		TooltipText tooltipFor(const parameterHelp::Entry& _entry, std::string _footer) const;
		// " Now: <value>. <what it means>" for a setting whose value the LCD prints as text.
		std::string nowText(const parameterHelp::ValueHash& _value) const;
		void cancelLcdGesture();
		void emitEncoderSteps(md::PanelEncoder _encoder, int _steps) const;
		void createButtons();
		void createPanelAffordances();
		void bindPanelTarget(const char* _id, md::PanelControl _control);
		void bindPanelChord(const char* _id, md::PanelControl _control);
		void pressPanelButton(juceRmlUi::ElemButton* _button, md::PanelControl _control,
			const md::PanelPacket& _packet, bool _shiftDown);
		void releasePanelButton(juceRmlUi::ElemButton* _button, md::PanelControl _control,
			const md::PanelPacket& _packet);
		void releaseActivePanelButtons();
		void beginPanelGesture(Rml::Element* _element,
			std::initializer_list<md::PanelControl> _controls);
		void endPanelGesture();
		void releasePanelButtonGestures();
		void releaseEncoderPress();
		void cancelPanelInputGestures();
		void releaseAllPanelInputs();
		void globalFocusChanged(juce::Component* _focusedComponent) override;
		void queuePanelPulse(md::PanelControl _control, int _count = 1);
		void servicePanelQueue();
		void servicePanelNavigation();
		void selectMachinedrumTrack(int _track);
		void selectMachinedrumDataPage(int _page);
		void selectMonomachineDataPage(int _page);
		void selectMonomachineTrigMode(int _mode);
		void togglePatternBankLatch(juceRmlUi::ElemButton* _button, const md::PanelPacket& _packet);
		void releasePatternBankLatch();
		void createEncoders();
		void createMasterVolume();
		void configureEncoder(juceRmlUi::ElemKnob* _knob, md::PanelEncoder _encoder,
			float& _last, float& _accum);
		void onEncoderChanged(juceRmlUi::ElemKnob* _knob, md::PanelEncoder _encoder,
			float& _last, float& _accum);
		void createLeds();
		bool updateLeds();
		void paintLcd(const juce::Image& _target, juce::Graphics& _graphics) const;

		enum class StorageImageBookmark
		{
			None,
			Factory,
			Other
		};

		void chooseStorageImage(StorageImageBookmark _bookmark);
		void confirmStorageImage(const juce::File& _file,
			StorageImageBookmark _bookmark);
		void showStorageOperationResult(bool _success, const juce::String& _message);
		std::optional<md::SysexImportProgress> getUserSysexProgress() const;
		void sendUserSysexFile(const juce::File& _file, const md::SysexImportTicket& _ticket);
		void startUserSysexTransfer(const std::shared_ptr<md::PreparedMidiSysexTransfer>& _prepared,
			const juce::File& _file, const md::SysexImportTicket& _ticket, bool _receiveModeConfirmed);
		void launchUserSysexFileChooser(const md::SysexImportTicket& _ticket);
		void showUserSysexError(const juce::String& _message);
		void serviceUserSysexProgress();

		enum class StorageImageFlow
		{
			None,
			Choosing,
			AwaitingConfirmation
		};

		Controller& m_controller;
		const md::MachineModel m_model;
		juceRmlUi::ElemCanvas* m_lcdCanvas = nullptr;
		std::unique_ptr<PixelPerfectPanel> m_pixelPerfectPanel;
		md::FrontPanel m_frontPanelSnapshot;
		bool m_frontPanelSnapshotValid = false;
		bool m_lcdChanged = true;
		bool m_lcdInteractionInputChanged = true;
		std::optional<lcdInteraction::State> m_lcdInteractionState;
		std::optional<unsigned> m_lcdHoverEncoder;
		Rml::Element* m_lcdArea = nullptr;				// tooltip anchor for the LCD
		Rml::Element* m_parameterTooltip = nullptr;
		HelpOverrides m_help;							// user edits to the tooltip text, see mdHelpOverrides.h
		bool m_tooltipsEnabled = true;
		int m_tooltipDelayMs = g_defaultTooltipDelayMs;
		// What the pointer rests on and since when, for the pop-up delay.
		TooltipTarget m_tooltipRestTarget;
		std::chrono::steady_clock::time_point m_tooltipRestSince{};
		std::string m_parameterTooltipContent;			// last rendered content, to skip redundant updates
		std::optional<unsigned> m_tooltipHoverKnob;		// mouse over a panel knob
		bool m_tooltipLcdMachineName = false;			// mouse over the machine name on the LCD
		std::optional<unsigned> m_lcdWheelEncoder;
		lcdInteraction::DragGesture m_lcdDragGesture;
		lcdInteraction::DetentAccumulator m_lcdWheelAccumulator;
		FrontPanelLedPresentation m_ledPresentation;
		bool m_ledsChanged = true;
		md::FrontPanelLedTransitionStatus m_ledTransitionStatus;
		bool m_ledTransitionStatusValid = false;
		bool m_ledResyncPending = false;
		uint64_t m_ledResyncSequence = 0;

		md::PanelRowState m_panelRows;
		juceRmlUi::ElemButton* m_patternBankButton = nullptr;
		std::optional<md::PanelPacket> m_patternBankPacket;
		Rml::Element* m_panelGestureElement = nullptr;
		std::vector<md::PanelPacket> m_panelGesturePackets;
		panelAffordances::ShiftPanelLatch m_shiftPanelLatch;
		panelAffordances::EncoderPressGesture m_encoderPress;
		juceRmlUi::ElemKnob* m_pressedEncoder = nullptr;

		struct ActivePanelButton
		{
			juceRmlUi::ElemButton* button = nullptr;
			md::PanelPacket packet;
		};
		std::vector<ActivePanelButton> m_activePanelButtons;

		struct PanelStep
		{
			md::PanelPacket packet;
			bool press = false;
		};
		std::deque<PanelStep> m_panelSteps;
		int m_panelSettleTicks = 0;
		panelAffordances::PendingTarget<panelAffordances::g_machinedrumDataPages.size()>
			m_machinedrumDataPageTarget;
		panelAffordances::PendingTarget<panelAffordances::g_monomachineDataPages.size()>
			m_monomachineDataPageTarget;
		panelAffordances::PendingTarget<panelAffordances::g_monomachineTrigModes.size()>
			m_monomachineTrigModeTarget;

		std::array<juceRmlUi::ElemKnob*, 8> m_encoders{};
		std::array<float, 8> m_encLast{};
		std::array<float, 8> m_encAccum{};
		juceRmlUi::ElemKnob* m_levelEncoder = nullptr;
		float m_levelLast = 0.0f;
		float m_levelAccum = 0.0f;
		juceRmlUi::ElemKnob* m_soundEncoder = nullptr;
		float m_soundLast = 0.0f;
		float m_soundAccum = 0.0f;
		juceRmlUi::ElemKnob* m_masterVolume = nullptr;

		std::array<Rml::Element*, 16> m_stepLeds{};
		std::array<Rml::Element*, 16> m_drumLeds{};

		struct StatusLedElem
		{
			Rml::Element* elem;
			uint8_t bit;	// md::FrontPanel::StatusLed
		};
		std::array<StatusLedElem, 5> m_statusLeds{};
		std::array<StatusLedElem, 6> m_mdModeLeds{};

		struct RawLedElem
		{
			Rml::Element* elem = nullptr;
			uint8_t bank = 0;
			uint8_t bit = 0;
		};
		std::array<RawLedElem, 4> m_mdPageLeds{};
		std::array<RawLedElem, 20> m_mmPanelLeds{};
		std::unique_ptr<juce::FileChooser> m_storageFileChooser;
		StorageImageFlow m_storageImageFlow = StorageImageFlow::None;
		std::unique_ptr<juce::FileChooser> m_sysexFileChooser;
		bool m_sysexChooserOpen = false;
		bool m_sysexTransferWasActive = false;
		md::SysexImportTicket m_sysexMonitoredTicket;
		md::MidiSysexTransferState m_sysexLastState =
			md::MidiSysexTransferState::Idle;
		size_t m_sysexLastSent = 0;
		uint32_t m_sysexLastServiceSerial = 0;
		uint32_t m_sysexReceivePromptId = 0;
		size_t m_sysexReceivePromptStep = 0;
		double m_sysexLastAdvanceMilliseconds = 0.0;
		bool m_sysexStallWarningShown = false;
		std::shared_ptr<void> m_lifetimeToken = std::make_shared<int>(0);
	};
}
