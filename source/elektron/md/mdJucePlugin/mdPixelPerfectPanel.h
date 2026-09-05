#pragma once

#include "RmlUi/Core/ComputedValues.h"
#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/Geometry.h"
#include "RmlUi/Core/MeshUtilities.h"
#include "RmlUi/Core/RenderManager.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace mdJucePlugin
{
	// Attached only to explicitly authored 1dp, flat, non-interactive panel
	// rules. Leave the parent's layout alone, including connections to labels.
	class PixelPanelRule final : public Rml::Element
	{
	public:
		PixelPanelRule(Rml::CoreInstance& _core, const Rml::String& _tag) : Element(_core, _tag) {}

		void init(Rml::Element& _parent)
		{
			m_color = _parent.GetComputedValues().background_color();
			if (const auto* property = _parent.GetLocalProperty(Rml::PropertyId::BackgroundColor))
				m_originalBackground = *property;
			SetProperty("position", "absolute");
			SetProperty("left", "0px");
			SetProperty("top", "0px");
			SetProperty("width", "100%");
			SetProperty("height", "100%");
			SetProperty("pointer-events", "none");
			setEnabled(false);
		}

		void setEnabled(bool _enabled)
		{
			SetProperty("display", _enabled ? "block" : "none");
			auto* parent = GetParentNode();
			if (_enabled)
				parent->SetProperty("background-color", "transparent");
			else if (m_originalBackground)
				parent->SetProperty(Rml::PropertyId::BackgroundColor, *m_originalBackground);
			else
				parent->RemoveProperty(Rml::PropertyId::BackgroundColor);
		}

	private:
		void OnRender() override
		{
			auto* parent = GetParentNode();
			const auto size = parent->GetBox().GetSize(Rml::BoxArea::Content);
			if (size.x <= 0 || size.y <= 0)
				return;
			const Rml::Vector2f snappedSize(std::max(1.f, std::round(size.x)), std::max(1.f, std::round(size.y)));
			const auto color = m_color.ToPremultiplied(parent->GetComputedValues().opacity());
			if (!m_geometry || snappedSize != m_size || color != m_renderColor)
			{
				auto mesh = m_geometry.Release(Rml::Geometry::ReleaseMode::ClearMesh);
				Rml::MeshUtilities::GenerateQuad(mesh, {}, snappedSize, color);
				m_geometry = GetRenderManager()->MakeGeometry(std::move(mesh));
				m_size = snappedSize;
				m_renderColor = color;
			}
			m_geometry.Render(parent->GetAbsoluteOffset(Rml::BoxArea::Content).Round());
		}

		Rml::Colourb m_color;
		Rml::ColourbPremultiplied m_renderColor;
		std::optional<Rml::Property> m_originalBackground;
		Rml::Vector2f m_size;
		Rml::Geometry m_geometry;
	};
}
