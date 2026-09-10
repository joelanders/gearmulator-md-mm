#include "juceRmlUi/rmlMouseInput.h"

#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/Core.h"
#include "RmlUi/Core/CoreInstance.h"
#include "RmlUi/Core/ElementDocument.h"
#include "RmlUi/Core/Event.h"
#include "RmlUi/Core/EventListener.h"
#include "RmlUi/Core/RenderInterface.h"
#include "RmlUi/Core/SystemInterface.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	bool check(const bool _condition, const char* _message)
	{
		if(!_condition)
			std::fprintf(stderr, "rmlMouseInput_test: %s\n", _message);
		return _condition;
	}

	bool checkPosition(const Rml::Vector2i _actual, const Rml::Vector2i _expected,
		const char* _message)
	{
		if(_actual == _expected)
			return true;
		std::fprintf(stderr, "rmlMouseInput_test: %s: got (%d,%d), expected (%d,%d)\n",
			_message, _actual.x, _actual.y, _expected.x, _expected.y);
		return false;
	}

	class RenderInterface final : public Rml::RenderInterface
	{
	public:
		explicit RenderInterface(Rml::CoreInstance& _core) : Rml::RenderInterface(_core) {}

		Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override
		{
			return Rml::CompiledGeometryHandle(++m_nextGeometry);
		}
		void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
		void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
		Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return {}; }
		Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return {}; }
		void ReleaseTexture(Rml::TextureHandle) override {}
		void EnableScissorRegion(bool) override {}
		void SetScissorRegion(Rml::Rectanglei) override {}

	private:
		uintptr_t m_nextGeometry = 0;
	};

	class SystemInterface final : public Rml::SystemInterface
	{
	public:
		explicit SystemInterface(Rml::CoreInstance& _core) : Rml::SystemInterface(_core) {}
		double GetElapsedTime() override { return m_time; }
		void setTime(const double _time) { m_time = _time; }

	private:
		double m_time = 1.0;
	};

	const char* eventName(const Rml::EventId _id)
	{
		switch(_id)
		{
		case Rml::EventId::Mouseover: return "mouseover";
		case Rml::EventId::Mouseout: return "mouseout";
		case Rml::EventId::Mousemove: return "mousemove";
		case Rml::EventId::Mousedown: return "mousedown";
		case Rml::EventId::Mouseup: return "mouseup";
		case Rml::EventId::Click: return "click";
		case Rml::EventId::Dblclick: return "dblclick";
		case Rml::EventId::Dragstart: return "dragstart";
		case Rml::EventId::Drag: return "drag";
		case Rml::EventId::Dragend: return "dragend";
		default: return "other";
		}
	}

	class EventLog final : public Rml::EventListener
	{
	public:
		void ProcessEvent(Rml::Event& _event) override
		{
			const auto* target = _event.GetTargetElement();
			if(!target || target->GetId().empty())
				return; // Pin control ordering; ancestor hover-chain order is not an input contract.
			const auto& id = target->GetId();
			events.emplace_back(std::string(eventName(_event.GetId())) + ":" + id);
		}

		void clear() { events.clear(); }
		std::vector<std::string> events;
	};

	class Fixture
	{
	public:
		Fixture() : renderer(core), system(core)
		{
			Rml::SetRenderInterface(core, &renderer);
			Rml::SetSystemInterface(core, &system);
			initialized = Rml::Initialise(core);
			if(!initialized)
				return;

			context = Rml::CreateContext(core, "mouse-input-test", { 300, 200 });
			if(!context)
				return;

			static constexpr const char* source = R"(
<rml>
<head><style>
body { margin: 0; width: 300px; height: 200px; }
#left { position: absolute; left: 0; top: 0; width: 150px; height: 200px; drag: drag; }
#right { position: absolute; left: 150px; top: 0; width: 150px; height: 200px; }
</style></head>
<body><div id="left"></div><div id="right"></div></body>
</rml>)";
			document = context->LoadDocumentFromMemory(source);
			if(!document)
				return;

			for(const auto event : { Rml::EventId::Mouseover, Rml::EventId::Mouseout,
				Rml::EventId::Mousemove, Rml::EventId::Mousedown, Rml::EventId::Mouseup,
				Rml::EventId::Click, Rml::EventId::Dblclick, Rml::EventId::Dragstart,
				Rml::EventId::Drag, Rml::EventId::Dragend })
				document->AddEventListener(event, &log);

			document->Show();
			context->Update();
		}

		~Fixture()
		{
			if(document)
				document->Close();
			if(context)
				Rml::RemoveContext(core, "mouse-input-test");
			if(initialized)
				Rml::Shutdown(core);
		}

		bool valid() const { return initialized && context && document; }

		Rml::CoreInstance core;
		RenderInterface renderer;
		SystemInterface system;
		Rml::Context* context = nullptr;
		Rml::ElementDocument* document = nullptr;
		EventLog log;
		bool initialized = false;
	};

	bool expectEvents(const EventLog& _log, const std::vector<std::string>& _expected,
		const char* _scenario)
	{
		if(_log.events == _expected)
			return true;

		std::fprintf(stderr, "rmlMouseInput_test: %s ordering mismatch\n  actual:", _scenario);
		for(const auto& event : _log.events)
			std::fprintf(stderr, " %s", event.c_str());
		std::fprintf(stderr, "\n  expected:");
		for(const auto& event : _expected)
			std::fprintf(stderr, " %s", event.c_str());
		std::fprintf(stderr, "\n");
		return false;
	}

	bool testCoordinateMatrix()
	{
		using juceRmlUi::mouseInput::mapComponentToContextPosition;
		bool success = true;

		for(const int percent : { 50, 100, 150, 200, 300 })
		{
			const Rml::Vector2i dimensions { 3 * percent, 2 * percent };
			success &= checkPosition(mapComponentToContextPosition(0, 0,
				dimensions.x, dimensions.y, dimensions), { 0, 0 }, "scaled top-left");
			success &= checkPosition(mapComponentToContextPosition(dimensions.x / 2,
				dimensions.y / 2, dimensions.x, dimensions.y, dimensions),
				{ dimensions.x / 2, dimensions.y / 2 }, "scaled center");
			success &= checkPosition(mapComponentToContextPosition(dimensions.x,
				dimensions.y, dimensions.x, dimensions.y, dimensions),
				dimensions, "scaled bottom-right boundary");
		}

		success &= checkPosition(mapComponentToContextPosition(500, 300,
			1000, 600, { 1500, 800 }), { 750, 400 }, "nonuniform transient center");
		success &= checkPosition(mapComponentToContextPosition(999, 599,
			1000, 600, { 1500, 800 }), { 1498, 799 }, "nonuniform transient last pixel");
		success &= checkPosition(mapComponentToContextPosition(1, 1,
			2, 2, { 3, 5 }), { 2, 2 }, "JUCE-compatible half-to-even rounding");
		success &= checkPosition(mapComponentToContextPosition(-20, 620,
			1000, 600, { 1500, 800 }), { -30, 827 }, "out-of-bounds drag mapping");
		success &= checkPosition(mapComponentToContextPosition(17, 23,
			0, 600, { 1500, 800 }), { 17, 23 }, "zero-width fallback");
		return success;
	}

	bool testHitMapping()
	{
		Fixture fixture;
		if(!check(fixture.valid(), "failed to create RML input fixture"))
			return false;

		const auto leftPosition = juceRmlUi::mouseInput::mapComponentToContextPosition(
			100, 75, 600, 300, fixture.context->GetDimensions());
		const auto rightPosition = juceRmlUi::mouseInput::mapComponentToContextPosition(
			500, 75, 600, 300, fixture.context->GetDimensions());
		const auto* left = fixture.context->GetElementAtPoint(
			{ static_cast<float>(leftPosition.x), static_cast<float>(leftPosition.y) });
		const auto* right = fixture.context->GetElementAtPoint(
			{ static_cast<float>(rightPosition.x), static_cast<float>(rightPosition.y) });
		return check(left && left->GetId() == "left", "mapped left hit missed")
			&& check(right && right->GetId() == "right", "mapped right hit missed");
	}

	bool testEventOrdering()
	{
		bool success = true;

		{
			Fixture fixture;
			if(!check(fixture.valid(), "failed to create first-click fixture"))
				return false;
			auto& context = *fixture.context;

			// Model the stale hover left behind when component and context sizes change
			// on different turns: the pointer used to hit the right control, while the
			// first post-resize click maps to the left control.
			context.ProcessMouseMove(250, 50, 0);
			fixture.log.clear();
			const auto firstClick = juceRmlUi::mouseInput::mapComponentToContextPosition(
				100, 75, 600, 300, context.GetDimensions());
			juceRmlUi::mouseInput::processButtonDown(context, firstClick, 0, 0);
			juceRmlUi::mouseInput::processButtonUp(context, firstClick, 0, 0);
			success &= expectEvents(fixture.log, {
				"mouseout:right", "mouseover:left", "mousemove:left",
				"mousedown:left", "mouseup:left", "click:left"
			}, "first click after resize/scale");
		}

		{
			Fixture fixture;
			if(!check(fixture.valid(), "failed to create drag fixture"))
				return false;
			auto& context = *fixture.context;
			juceRmlUi::mouseInput::processButtonDown(context, { 50, 50 }, 0, 0);
			context.ProcessMouseMove(75, 50, 0);
			juceRmlUi::mouseInput::processButtonUp(context, { 75, 50 }, 0, 0);
			success &= expectEvents(fixture.log, {
				"mouseover:left", "mousemove:left", "mousedown:left",
				"dragstart:left", "drag:left", "mousemove:left",
				"mouseup:left", "click:left", "dragend:left"
			}, "normal knob drag");
		}

		{
			Fixture fixture;
			if(!check(fixture.valid(), "failed to create outside-release fixture"))
				return false;
			auto& context = *fixture.context;
			juceRmlUi::mouseInput::processButtonDown(context, { 50, 50 }, 0, 0);
			context.ProcessMouseMove(250, 50, 0);
			juceRmlUi::mouseInput::processButtonUp(context, { 250, 50 }, 0, 0);
			success &= expectEvents(fixture.log, {
				"mouseover:left", "mousemove:left", "mousedown:left",
				"dragstart:left", "drag:left", "mouseout:left", "mouseover:right", "mousemove:right",
				"mouseup:right", "dragend:left"
			}, "release outside control");
		}

		{
			Fixture fixture;
			if(!check(fixture.valid(), "failed to create double-click fixture"))
				return false;
			auto& context = *fixture.context;
			fixture.system.setTime(10.0);
			juceRmlUi::mouseInput::processButtonDown(context, { 50, 50 }, 0, 0);
			juceRmlUi::mouseInput::processButtonUp(context, { 50, 50 }, 0, 0);
			fixture.system.setTime(10.1);
			juceRmlUi::mouseInput::processButtonDown(context, { 50, 50 }, 0, 0);
			juceRmlUi::mouseInput::processButtonUp(context, { 50, 50 }, 0, 0);
			success &= expectEvents(fixture.log, {
				"mouseover:left", "mousemove:left",
				"mousedown:left", "mouseup:left", "click:left",
				"mousedown:left", "dblclick:left", "mouseup:left", "click:left"
			}, "double click");
		}

		{
			Fixture fixture;
			if(!check(fixture.valid(), "failed to create right-click fixture"))
				return false;
			auto& context = *fixture.context;
			juceRmlUi::mouseInput::processButtonDown(context, { 50, 50 }, 1, 0);
			juceRmlUi::mouseInput::processButtonUp(context, { 50, 50 }, 1, 0);
			success &= expectEvents(fixture.log, {
				"mouseover:left", "mousemove:left",
				"mousedown:left", "mouseup:left"
			}, "right click/context");
		}

		{
			Fixture fixture;
			if(!check(fixture.valid(), "failed to create hover fixture"))
				return false;
			auto& context = *fixture.context;
			context.ProcessMouseMove(50, 50, 0);
			context.ProcessMouseMove(250, 50, 0);
			context.ProcessMouseLeave();
			success &= expectEvents(fixture.log, {
				"mouseover:left", "mousemove:left",
				"mouseout:left", "mouseover:right", "mousemove:right",
				"mouseout:right"
			}, "hover transitions");
		}

		return success;
	}
}

int main()
{
	const bool success = testCoordinateMatrix()
		&& testHitMapping()
		&& testEventOrdering();
	if(success)
		std::puts("rmlMouseInput_test: PASS");
	return success ? 0 : 1;
}
