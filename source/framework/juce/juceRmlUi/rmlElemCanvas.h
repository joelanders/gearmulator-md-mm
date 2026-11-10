#pragma once

#include "rmlElement.h"

#include "RmlUi/Core/CallbackTexture.h"
#include "RmlUi/Core/Geometry.h"

#include <optional>

namespace juce
{
	class Graphics;
}

namespace juce
{
	class Image;
}

namespace juceRmlUi
{
	class ElemCanvas : public Element
	{
	public:
		struct RenderedRect
		{
			Rml::Vector2f origin;
			Rml::Vector2f size;
		};

		using RepaintCallback = std::function<void(std::vector<uint8_t>&)>;
		using RepaintGraphicsCallback = std::function<void(juce::Image&, juce::Graphics&)>;

		explicit ElemCanvas(Rml::CoreInstance& _coreInstance, const Rml::String& _tag);

		void setRepaintCallback(const RepaintCallback& _callback) { m_repaintCallback = _callback; }
		void setRepaintGraphicsCallback(const RepaintGraphicsCallback& _callback) { m_repaintGraphicsCallback = _callback; }

		void repaint();

		void setClearEveryFrame(bool _clearEveryFrame);
		void setPixelAligned(bool _enabled);
		Rml::Vector2i getPaintSize() const { return m_pixelAligned ? m_paintSize : m_textureSize; }
		std::optional<RenderedRect> getRenderedRect() const;

		// Render into a texture of this size instead of one matching the element's on-screen
		// size. The quad still covers the element, so the GPU scales the texture to fit with
		// linear filtering. Content drawn at a fixed resolution - an emulated LCD, say - then
		// keeps a stable pixel grid at any window size, instead of being re-rasterized to a
		// different, usually non-integer, scale every time the window is resized.
		// Pass (0, 0) to go back to following the on-screen size.
		void setFixedTextureSize(int _width, int _height);

		static ElemCanvas* create(Rml::Element* _parent);

	private:
		void generateGeometry();
		void generateTexture();

		void OnRender() override;
		void OnResize() override;
		void OnPropertyChange(const Rml::PropertyIdSet& _changedProperties) override;

		void updateImage();

		bool m_geometryDirty = true;
		bool m_textureDirty = true;
		Rml::Geometry m_geometry;
		Rml::Vector2i m_textureSize{ 0, 0 };
		Rml::Vector2i m_fixedTextureSize{ 0, 0 };
		Rml::CallbackTexture m_texture;

		std::unique_ptr<juce::Image> m_image;
		std::vector<uint8_t> m_imageBuffer;
		std::unique_ptr<juce::Graphics> m_graphics;

		RepaintCallback m_repaintCallback;
		RepaintGraphicsCallback m_repaintGraphicsCallback;

		bool m_clearEveryFrame = false;
		bool m_pixelAligned = false;
		Rml::Vector2i m_paintSize{0, 0};
		Rml::Vector2f m_quadOrigin{0, 0};
		Rml::Vector2f m_quadSize{0, 0};
		std::optional<RenderedRect> m_lastRenderedRect;
	};
}
