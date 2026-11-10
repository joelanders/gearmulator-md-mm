#include "mdEditor.h"
#include "mdLcdViewport.h"
#include "mdPluginEditorState.h"
#include "mdPixelPerfectPanel.h"
#include "mdPluginProcessor.h"

#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/rmlElemCanvas.h"
#include "juceRmlUi/rmlInterfaces.h"

#include "RmlUi/Core/Context.h"

#include "mdLib/mdpanel.h"
#include "synthLib/realtimeInstrumentation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace mdJucePlugin
{
	struct EditorIdentityTestAccess
	{
		static void installSurface(Editor& _editor,
			const std::optional<lcdInteraction::State>& _state)
		{
			_editor.m_frontPanelSnapshotValid = true;
			_editor.m_lcdInteractionInputChanged = false;
			_editor.m_lcdInteractionState = _state;
		}

		static void publishPanel(Editor& _editor, const md::FrontPanel& _panel)
		{
			_editor.m_frontPanelSnapshot = _panel;
			_editor.m_frontPanelSnapshotValid = true;
			_editor.m_lcdInteractionInputChanged = true;
			_editor.updateLcdInteractionState();
		}

		static const std::optional<lcdInteraction::State>& interactionState(
			const Editor& _editor)
		{
			return _editor.m_lcdInteractionState;
		}

		static juceRmlUi::ElemCanvas& canvas(Editor& _editor)
		{
			if(!_editor.m_lcdCanvas)
				throw std::runtime_error("editor did not create its LCD canvas");
			return *_editor.m_lcdCanvas;
		}

		static bool dragActive(const Editor& _editor)
		{
			return _editor.m_lcdDragGesture.active();
		}

		static void paintLcd(const Editor& _editor, juce::Image& _image)
		{
			juce::Graphics graphics(_image);
			_editor.paintLcd(_image, graphics);
		}
	};
}

namespace juceRmlUi
{
	struct RenderingTestAccess
	{
		static void update(RmlComponent& _component) { _component.m_renderOffScreenForTests = true; _component.update(); }
	};
}

namespace
{
	using Pixels = std::array<uint8_t,
		md::FrontPanel::g_lcdWidth * md::FrontPanel::g_lcdHeight>;

	void require(const bool _condition, const std::string& _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	void setPixel(Pixels& _pixels, const int _x, const int _y)
	{
		_pixels[static_cast<size_t>(_y) * md::FrontPanel::g_lcdWidth
			+ static_cast<size_t>(_x)] = 1;
	}

	void setLedBank(md::FrontPanel& _panel, const uint8_t _bank,
		const uint8_t _value)
	{
		_panel.processByte(_bank);
		_panel.processByte(_value);
	}

	md::FrontPanel makePanel(const Pixels& _pixels)
	{
		md::FrontPanel panel;
		for(unsigned half = 0; half < 2; ++half)
			for(unsigned page = 0; page < 8; ++page)
				for(unsigned base = 0; base < 64; base += 8)
				{
					panel.processByte(static_cast<uint8_t>(0x10 | (half << 3) | page));
					panel.processByte(static_cast<uint8_t>(base));
					for(unsigned column = base; column < base + 8; ++column)
					{
						uint8_t value = 0;
						for(unsigned bit = 0; bit < 8; ++bit)
						{
							const auto x = half * 64 + column;
							const auto y = page * 8 + bit;
							if(_pixels[y * md::FrontPanel::g_lcdWidth + x])
								value |= static_cast<uint8_t>(1u << bit);
						}
						panel.processByte(value);
					}
				}
		return panel;
	}

	md::FrontPanel makeStandardPanel(const md::MachineModel _model)
	{
		using namespace mdJucePlugin::lcdInteraction;
		Pixels pixels{};
		for(unsigned index = 0; index < 8; ++index)
		{
			const auto rect = encoderRect(LayoutKind::Standard, index);
			for(int x = rect.x + 1; x < rect.x + rect.width; x += 2)
				setPixel(pixels, x, rect.y);
			for(int y = rect.y + 2; y < rect.y + 31; y += 2)
				setPixel(pixels, rect.x + rect.width - 1, y);
			for(int y = rect.y + 14; y < rect.y + 29; ++y)
			{
				setPixel(pixels, rect.x + 2, y);
				setPixel(pixels, rect.x + 3, y);
			}
		}
		auto panel = makePanel(pixels);
		if(_model == md::MachineModel::Monomachine)
		{
			setLedBank(panel, 0x25, 0xe9);
			setLedBank(panel, 0x26, 0xd7);
			setLedBank(panel, 0x27, 0x01);
		}
		else
		{
			setLedBank(panel, 0x22, 0x74);
			setLedBank(panel, 0x23, 0xf9);
		}
		return panel;
	}

	Rml::Vector2i encoderCenter(juceRmlUi::ElemCanvas& _canvas,
		const mdJucePlugin::lcdInteraction::LayoutKind _layout,
		const unsigned _index)
	{
		const auto offset = _canvas.GetAbsoluteOffset(Rml::BoxArea::Content);
		const auto size = _canvas.GetBox().GetSize(Rml::BoxArea::Content);
		const auto scale = std::min(size.x / 128.f, size.y / 64.f);
		const auto contentWidth = 128.f * scale;
		const auto contentHeight = 64.f * scale;
		const auto contentX = (size.x - contentWidth) * 0.5f;
		const auto contentY = (size.y - contentHeight) * 0.5f;
		const auto rect = mdJucePlugin::lcdInteraction::encoderRect(_layout, _index);
		return {static_cast<int>(std::lround(offset.x + contentX
			+ (rect.x + rect.width * 0.5f) * scale)),
			static_cast<int>(std::lround(offset.y + contentY
			+ (rect.y + rect.height * 0.5f) * scale))};
	}

	bool imagesEqual(const juce::Image& _a, const juce::Image& _b)
	{
		if(_a.getBounds() != _b.getBounds())
			return false;
		const juce::Image::BitmapData a(_a, juce::Image::BitmapData::readOnly);
		const juce::Image::BitmapData b(_b, juce::Image::BitmapData::readOnly);
		for(int y = 0; y < _a.getHeight(); ++y)
			if(std::memcmp(a.getLinePointer(y), b.getLinePointer(y),
				static_cast<size_t>(_a.getWidth()) * a.pixelStride) != 0)
				return false;
		return true;
	}

	unsigned requireOnlyEncoderInput(
		synthLib::RealtimeInstrumentation& _instrumentation,
		const md::MachineModel _model, const unsigned _index,
		const std::string& _label)
	{
		const auto encoder = static_cast<md::PanelEncoder>(
			static_cast<unsigned>(md::PanelEncoder::DataEntryA) + _index);
		const auto expected = md::panelEncoderCommand(_model, encoder);
		require(expected.has_value(), _label + ": encoder has no panel command");
		unsigned count = 0;
		synthLib::RealtimeEvent event;
		while(_instrumentation.popTimelineEvent(event))
			if(event.kind == synthLib::RealtimeEventKind::PanelInput)
			{
				require(event.command == *expected,
					_label + ": pointer emitted the wrong DATA ENTRY command");
				require(event.argument == 0x01,
					_label + ": right/up input emitted the wrong sign");
				++count;
			}
		return count;
	}

	void requireNoPanelInput(synthLib::RealtimeInstrumentation& _instrumentation,
		const std::string& _label)
	{
		synthLib::RealtimeEvent event;
		while(_instrumentation.popTimelineEvent(event))
			require(event.kind != synthLib::RealtimeEventKind::PanelInput,
				_label + ": inactive LCD cell emitted panel input");
	}

	void exerciseCells(mdJucePlugin::Editor& _editor, Rml::Context& _context,
		juceRmlUi::ElemCanvas& _canvas,
		synthLib::RealtimeInstrumentation& _instrumentation,
		const md::MachineModel _model,
		const mdJucePlugin::lcdInteraction::LayoutKind _layout,
		const uint8_t _activeMask, const std::string& _label)
	{
		using namespace mdJucePlugin::lcdInteraction;
		if(_activeMask == 0)
			mdJucePlugin::EditorIdentityTestAccess::installSurface(_editor, std::nullopt);
		else
		{
			const auto surface = _layout == LayoutKind::Lfo ? SurfaceKind::Lfo
				: _layout == LayoutKind::MasterFx ? SurfaceKind::MasterFxEcho
				: SurfaceKind::EditGrid;
			mdJucePlugin::EditorIdentityTestAccess::installSurface(_editor,
				State{surface, _layout, _activeMask,
					static_cast<uint64_t>(_layout) + 1});
		}

		for(unsigned index = 0; index < 8; ++index)
		{
			const auto point = encoderCenter(_canvas, _layout, index);
			const auto active = (_activeMask & (1u << index)) != 0;

			// Plain clicks acquire/release a target but never turn or press it.
			_instrumentation.reset();
			_context.ProcessMouseMove(point.x, point.y, 0);
			_context.ProcessMouseButtonDown(0, 0);
			_context.ProcessMouseButtonUp(0, 0);
			require(!mdJucePlugin::EditorIdentityTestAccess::dragActive(_editor),
				_label + ": click left a gesture active");
			requireNoPanelInput(_instrumentation, _label + " plain click");

			_instrumentation.reset();
			_context.ProcessMouseMove(point.x, point.y, 0);
			_context.ProcessMouseButtonDown(0, 0);
			require(mdJucePlugin::EditorIdentityTestAccess::dragActive(_editor) == active,
				_label + ": pointer acquisition disagrees with mask at cell "
					+ std::to_string(index));
			_context.ProcessMouseMove(point.x + 30, point.y, 0);
			_context.ProcessMouseButtonUp(0, 0);
			require(!mdJucePlugin::EditorIdentityTestAccess::dragActive(_editor),
				_label + ": gesture remained active after release");
			if(active)
				require(requireOnlyEncoderInput(_instrumentation, _model, index,
					_label + " drag") != 0, _label + ": active cell ignored drag");
			else
				requireNoPanelInput(_instrumentation, _label + " drag");

			_instrumentation.reset();
			_context.ProcessMouseMove(point.x, point.y, 0);
			_context.ProcessMouseWheel(Rml::Vector2f{0.f, -1.f}, 0);
			if(active)
				require(requireOnlyEncoderInput(_instrumentation, _model, index,
					_label + " wheel") != 0, _label + ": active cell ignored wheel");
			else
				requireNoPanelInput(_instrumentation, _label + " wheel");
		}
	}
}

int main()
{
	try
	{
		juce::ScopedJuceInitialiser_GUI gui;
		#if defined(MD_LCD_POINTER_TEST_MM)
		constexpr auto model = md::MachineModel::Monomachine;
		constexpr auto product = "MM";
		#else
		constexpr auto model = md::MachineModel::Machinedrum;
		constexpr auto product = "MD";
		#endif

		mdJucePlugin::AudioPluginAudioProcessor processor(
			model,
			mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig{std::string{}}, false);
		processor.setForceSoftwareRendererForSession(true);
		require(!processor.getConfig().containsKey(
			mdJucePlugin::lcdInteraction::configKey),
			"ephemeral config unexpectedly contains the LCD interaction setting");
		require(processor.getConfig().getBoolValue(
			mdJucePlugin::lcdInteraction::configKey,
			mdJucePlugin::lcdInteraction::defaultEnabled),
			"fresh product config silently disables LCD rotary interaction");

		auto& editorState = static_cast<mdJucePlugin::PluginEditorState&>(
			processor.getOrCreateEditorState());
		auto* editor = dynamic_cast<mdJucePlugin::Editor*>(editorState.getEditor());
		require(editor != nullptr, "processor did not create the editor");
		auto* component = editor->getRmlComponent();
		require(component && component->getContext(), "editor has no RmlUi context");

		auto& instrumentation = processor.getPlugin().getRealtimeInstrumentation();
		instrumentation.setEnabled(true);
		instrumentation.reset();

		juceRmlUi::RmlInterfaces::ScopedAccess access(*component);
		auto& context = *component->getContext();
		context.Update();
		auto& canvas = mdJucePlugin::EditorIdentityTestAccess::canvas(*editor);
		require(canvas.GetBox().GetSize(Rml::BoxArea::Content).x > 0,
			"LCD canvas was not laid out");

		using namespace mdJucePlugin::lcdInteraction;
		auto publishedPanel = makeStandardPanel(model);
		mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor, publishedPanel);
		const auto publishedState =
			mdJucePlugin::EditorIdentityTestAccess::interactionState(*editor);
		require(publishedState && publishedState->surface == SurfaceKind::EditGrid
			&& publishedState->activeEncoderMask == 0xff,
			"published LCD/LED state did not reach editor classification");
		const auto publishedIdentity = publishedState->identityToken;
		const auto publishedPoint = encoderCenter(canvas, LayoutKind::Standard, 0);
		if(model == md::MachineModel::Monomachine)
		{
			constexpr uint8_t page25High[]{0xe0, 0xd0, 0xb0, 0x70, 0xf0, 0xf0, 0xf0};
			constexpr uint8_t page26[]{0xd7, 0xd7, 0xd7, 0xd7, 0xd6, 0xd5, 0xd3};
			// The low nibble of bank 0x25 is track-colour state, not a mode.
			// Sweep every possible value so every real track transition is covered.
			for(unsigned trackLeds = 0; trackLeds < 16; ++trackLeds)
				for(unsigned page = 0; page < 7; ++page)
				{
					auto transitionPanel = makeStandardPanel(model);
					setLedBank(transitionPanel, 0x25,
						static_cast<uint8_t>(page25High[page] | trackLeds));
					setLedBank(transitionPanel, 0x26, page26[page]);
					mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor,
						transitionPanel);
					const auto transitionState =
						mdJucePlugin::EditorIdentityTestAccess::interactionState(*editor);
					require(transitionState && transitionState->surface == SurfaceKind::EditGrid
						&& transitionState->activeEncoderMask == 0xff,
						"MM track/page transition lost the EDIT grid");

					instrumentation.reset();
					context.ProcessMouseMove(publishedPoint.x, publishedPoint.y, 0);
					context.ProcessMouseButtonDown(0, 0);
					context.ProcessMouseMove(publishedPoint.x + 30, publishedPoint.y, 0);
					context.ProcessMouseButtonUp(0, 0);
					require(requireOnlyEncoderInput(instrumentation, model, 0,
						"MM track/page transition drag") != 0,
						"MM track/page transition ignored pointer drag");
				}
			struct SpecialSurface
			{
				uint8_t bank25;
				uint8_t bank26;
				uint8_t mask;
			};
			for(const auto special : {SpecialSurface{0x09, 0x57, 0xff},
				SpecialSurface{0x09, 0x17, 0xff},
				SpecialSurface{0xed, 0x17, 0xff},
				SpecialSurface{0xf9, 0x57, 0x0f}})
			{
				auto transitionPanel = makeStandardPanel(model);
				setLedBank(transitionPanel, 0x25, special.bank25);
				setLedBank(transitionPanel, 0x26, special.bank26);
				mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor,
					transitionPanel);
				const auto transitionState =
					mdJucePlugin::EditorIdentityTestAccess::interactionState(*editor);
				require(transitionState
					&& transitionState->activeEncoderMask == special.mask,
					"MM special DATA surface lost its qualified controls");

				instrumentation.reset();
				context.ProcessMouseMove(publishedPoint.x, publishedPoint.y, 0);
				context.ProcessMouseButtonDown(0, 0);
				context.ProcessMouseMove(publishedPoint.x + 30, publishedPoint.y, 0);
				context.ProcessMouseButtonUp(0, 0);
				require(requireOnlyEncoderInput(instrumentation, model, 0,
					"MM special DATA surface drag") != 0,
					"MM special DATA surface ignored pointer drag");
			}
			publishedPanel = makeStandardPanel(model);
			mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor, publishedPanel);
		}
		else
		{
			for(const auto bank22 : {uint8_t{0x74}, uint8_t{0xb4}, uint8_t{0xd4}})
			{
				auto transitionPanel = makeStandardPanel(model);
				setLedBank(transitionPanel, 0x22, bank22);
				mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor,
					transitionPanel);
				const auto transitionState =
					mdJucePlugin::EditorIdentityTestAccess::interactionState(*editor);
				require(transitionState && transitionState->surface == SurfaceKind::EditGrid
					&& transitionState->activeEncoderMask == 0xff,
					"MD page transition lost the EDIT grid");

				instrumentation.reset();
				context.ProcessMouseMove(publishedPoint.x, publishedPoint.y, 0);
				context.ProcessMouseButtonDown(0, 0);
				context.ProcessMouseMove(publishedPoint.x + 30, publishedPoint.y, 0);
				context.ProcessMouseButtonUp(0, 0);
				require(requireOnlyEncoderInput(instrumentation, model, 0,
					"MD page transition drag") != 0,
					"MD page transition ignored pointer drag");
			}
			publishedPanel = makeStandardPanel(model);
			mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor, publishedPanel);
		}
		instrumentation.reset();
		context.ProcessMouseMove(publishedPoint.x, publishedPoint.y, 0);
		context.ProcessMouseButtonDown(0, 0);
		context.ProcessMouseMove(publishedPoint.x + 30, publishedPoint.y, 0);
		context.ProcessMouseButtonUp(0, 0);
		require(requireOnlyEncoderInput(instrumentation, model, 0,
			"published-state drag") != 0,
			"classified published state did not route pointer input");

		if(model == md::MachineModel::Monomachine)
			setLedBank(publishedPanel, 0x26, 0xcf);
		else
			setLedBank(publishedPanel, 0x23, 0xf5);
		mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor, publishedPanel);
		const auto changedBankState =
			mdJucePlugin::EditorIdentityTestAccess::interactionState(*editor);
		require(changedBankState && changedBankState->activeEncoderMask == 0xff
			&& changedBankState->identityToken == publishedIdentity,
			"bank-group change disabled or replaced the published surface");

		instrumentation.reset();
		context.ProcessMouseMove(publishedPoint.x, publishedPoint.y, 0);
		context.ProcessMouseButtonDown(0, 0);
		require(mdJucePlugin::EditorIdentityTestAccess::dragActive(*editor),
			"published-state transition test did not begin a drag");
		if(model == md::MachineModel::Monomachine)
			setLedBank(publishedPanel, 0x27, 0x00);
		else
			setLedBank(publishedPanel, 0x23, 0xe5);
		mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor, publishedPanel);
		require(!mdJucePlugin::EditorIdentityTestAccess::dragActive(*editor),
			"unsupported published state did not cancel the active drag");
		context.ProcessMouseMove(publishedPoint.x + 30, publishedPoint.y, 0);
		context.ProcessMouseButtonUp(0, 0);
		requireNoPanelInput(instrumentation,
			"pointer input after unsupported published state");

		publishedPanel = makeStandardPanel(model);
		mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor, publishedPanel);
		instrumentation.reset();
		context.ProcessMouseMove(publishedPoint.x, publishedPoint.y, 0);
		context.ProcessMouseButtonDown(0, 0);
		require(mdJucePlugin::EditorIdentityTestAccess::dragActive(*editor),
			"disable transition test did not begin a drag");
		processor.getConfig().setValue(configKey, false);
		editor->applyLcdInteraction();
		require(!mdJucePlugin::EditorIdentityTestAccess::dragActive(*editor)
			&& !mdJucePlugin::EditorIdentityTestAccess::interactionState(*editor),
			"disabling LCD interaction did not cancel the active drag");
		context.ProcessMouseMove(publishedPoint.x + 30, publishedPoint.y, 0);
		context.ProcessMouseButtonUp(0, 0);
		requireNoPanelInput(instrumentation, "pointer input after feature disable");
		processor.getConfig().setValue(configKey, true);
		editor->applyLcdInteraction();

		mdJucePlugin::EditorIdentityTestAccess::installSurface(*editor,
			State{SurfaceKind::EditGrid, LayoutKind::Standard, 0xff, 1});
		const auto start = encoderCenter(canvas, LayoutKind::Standard, 0);
		const auto canvasSize = canvas.GetBox().GetSize(Rml::BoxArea::Content);
		juce::Image beforeHover(juce::Image::ARGB,
			static_cast<int>(canvasSize.x), static_cast<int>(canvasSize.y), true);
		juce::Image duringHover(juce::Image::ARGB,
			static_cast<int>(canvasSize.x), static_cast<int>(canvasSize.y), true);
		mdJucePlugin::EditorIdentityTestAccess::paintLcd(*editor, beforeHover);
		context.ProcessMouseMove(start.x, start.y, 0);
		mdJucePlugin::EditorIdentityTestAccess::paintLcd(*editor, duringHover);
		require(imagesEqual(beforeHover, duringHover),
			"LCD pixels changed merely because a field was hovered");
		require(instrumentation.snapshot().timelineEvents == 0,
			"LCD hover touched the firmware/audio-facing panel path");

		context.ProcessMouseButtonDown(0, 0);
		context.ProcessMouseMove(start.x, start.y - 30, 0);
		require(requireOnlyEncoderInput(instrumentation, model, 0,
			"vertical drag") != 0,
			"vertical drag did not emit DATA ENTRY A");
		context.ProcessMouseButtonUp(0, 0);

		instrumentation.reset();
		context.ProcessMouseMove(start.x, start.y, 0);
		context.ProcessMouseButtonDown(0, 0);
		context.ProcessMouseMove(start.x + 30, start.y, 0);
		const auto ordinarySteps = requireOnlyEncoderInput(
			instrumentation, model, 0, "horizontal drag");
		context.ProcessMouseButtonUp(0, 0);
		require(ordinarySteps != 0, "horizontal drag did not emit DATA ENTRY A");

		instrumentation.reset();
		context.ProcessMouseMove(start.x, start.y, 0);
		context.ProcessMouseButtonDown(0, 0);
		context.ProcessMouseMove(start.x + 30, start.y, Rml::Input::KM_META);
		const auto fineSteps = requireOnlyEncoderInput(
			instrumentation, model, 0, "fine horizontal drag");
		context.ProcessMouseButtonUp(0, Rml::Input::KM_META);
		require(fineSteps != 0 && fineSteps < ordinarySteps,
			"Command-drag did not produce a slower turn");

		mdJucePlugin::EditorIdentityTestAccess::installSurface(*editor,
			State{SurfaceKind::EditGrid, LayoutKind::Standard, 0xff, 1});
		const auto other = encoderCenter(canvas, LayoutKind::Standard, 1);
		context.ProcessMouseMove(other.x, other.y, 0);
		context.ProcessMouseMove(start.x, start.y, 0);
		instrumentation.reset();
		// RmlUi wheel deltas are in notches since the upstream merge (RmlComponent::mouseWheelMove,
		// one detent = range/32, about 3 encoder steps). The old raw JUCE delta of -1 was about four
		// detents, which is what saturates the DATA ENTRY knob burst.
		context.ProcessMouseWheel(Rml::Vector2f{0.f, -4.f}, 0);
		const auto ordinaryWheelSteps = requireOnlyEncoderInput(
			instrumentation, model, 0, "ordinary LCD wheel");
		require(ordinaryWheelSteps == 8,
			"ordinary LCD wheel did not match the DATA ENTRY knob burst");

		context.ProcessMouseMove(other.x, other.y, 0);
		context.ProcessMouseMove(start.x, start.y, 0);
		instrumentation.reset();
		context.ProcessMouseWheel(Rml::Vector2f{0.f, -1.f}, Rml::Input::KM_META);
		const auto fineWheelSteps = requireOnlyEncoderInput(
			instrumentation, model, 0, "fine LCD wheel");
		require(fineWheelSteps == 1,
			"Command/Ctrl LCD wheel did not use the DATA ENTRY knob single step");

		context.ProcessMouseMove(other.x, other.y, 0);
		context.ProcessMouseMove(start.x, start.y, 0);
		instrumentation.reset();
		context.ProcessMouseWheel(Rml::Vector2f{0.f, -0.01f}, 0);
		requireNoPanelInput(instrumentation,
			"fractional ordinary LCD wheel emitted before one detent");
		context.ProcessMouseWheel(Rml::Vector2f{0.f, -0.01f}, Rml::Input::KM_META);
		require(requireOnlyEncoderInput(instrumentation, model, 0,
			"fractional fine LCD wheel") == 1,
			"fractional Command/Ctrl wheel did not emit one fine step");

		exerciseCells(*editor, context, canvas, instrumentation, model,
			LayoutKind::Standard, 0xff, "standard full");
		exerciseCells(*editor, context, canvas, instrumentation, model,
			LayoutKind::Standard, 0x55, "standard sparse");
		exerciseCells(*editor, context, canvas, instrumentation, model,
			LayoutKind::Lfo, 0xff, "LFO");
		exerciseCells(*editor, context, canvas, instrumentation, model,
			LayoutKind::MasterFx, 0xff, "Master FX");
		exerciseCells(*editor, context, canvas, instrumentation, model,
			LayoutKind::Standard, 0, "disabled surface");

		// The pixel-perfect renderer snaps the outer canvas quad independently
		// of RmlUi's fractional layout position. Drive real pointer events at the
		// *absolute painted* A/B boundary, rather than round-tripping a viewport.
		mdJucePlugin::EditorIdentityTestAccess::publishPanel(*editor,
			makeStandardPanel(model));
		processor.getConfig().setValue(mdJucePlugin::PixelPerfectPanel::configKey, true);
		editor->applyPixelPerfectPanel();
		auto* lcdArea = canvas.GetParentNode();
		lcdArea->SetProperty("left", "30.25px");
		lcdArea->SetProperty("top", "40.25px");
		lcdArea->SetProperty("width", "229.5px");
		lcdArea->SetProperty("height", "125.5px");
		for(int frame = 0; frame < 4; ++frame)
		{
			juceRmlUi::RenderingTestAccess::update(*component);
			juce::Image image(juce::Image::ARGB,
				component->getWidth(), component->getHeight(), true);
			juce::Graphics graphics(image);
			component->paint(graphics);
		}
		const auto layoutOffset = canvas.GetAbsoluteOffset(Rml::BoxArea::Content);
		const auto rendered = canvas.getRenderedRect();
		const auto paintSize = canvas.getPaintSize();
		require(rendered && std::abs(layoutOffset.x - rendered->origin.x) > 0.1f
			&& std::abs(layoutOffset.y - rendered->origin.y) > 0.1f,
			"fractional canvas origin did not diverge from snapped painted origin");
		require(rendered->size.x == paintSize.x && rendered->size.y == paintSize.y,
			"rendered canvas dimensions disagree with pixel-aligned paint size");
		const auto viewport = Viewport::create(
			rendered->size.x, rendered->size.y,
			paintSize.x, paintSize.y, true);
		const auto lcd = viewport.contentInPaintSpace();
		const auto boundaryX = static_cast<int>(rendered->origin.x + lcd.x + 68);
		const auto boundaryY = static_cast<int>(rendered->origin.y + lcd.y + 10);
		for(const auto sample : {std::pair{boundaryX - 1, 0u},
			std::pair{boundaryX, 1u}})
		{
			instrumentation.reset();
			context.ProcessMouseMove(sample.first, boundaryY, 0);
			context.ProcessMouseButtonDown(0, 0);
			require(mdJucePlugin::EditorIdentityTestAccess::dragActive(*editor),
				"painted boundary click did not acquire a DATA ENTRY field");
			context.ProcessMouseMove(sample.first + 30, boundaryY, 0);
			context.ProcessMouseButtonUp(0, 0);
			require(requireOnlyEncoderInput(instrumentation, model, sample.second,
				"absolute painted A/B boundary drag") != 0,
				"painted boundary drag did not emit its displayed encoder");
		}

		std::printf("%s LcdEditorPointerTest: PASS\n", product);
		return 0;
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "mdLcdEditorPointerTest: %s\n", error.what());
		return 1;
	}
}
