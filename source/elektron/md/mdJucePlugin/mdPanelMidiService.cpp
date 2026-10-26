#include "mdPanelMidiService.h"

namespace mdJucePlugin::panelMidi
{
	Service::Service(const md::MachineModel _model, std::string _filePath)
		: m_model(_model)
		, m_controller(_model, std::move(_filePath), {}, Table())
	{
		m_pending.reserve(g_maxPending);
		m_draining.reserve(g_maxPending);
		m_controller.setChangedHandler([this] { publish(); });
		publish();
		// Short enough that a pad press feels immediate.
		startTimer(5);
	}

	Service::~Service()
	{
		stopTimer();
		m_controller.setChangedHandler({});
	}

	void Service::setActionHandler(Controller::ActionHandler _handler)
	{
		m_controller.setActionHandler(std::move(_handler));
	}

	bool Service::intercept(const uint8_t _status, const uint8_t _data1, const uint8_t _data2)
	{
		if(!isBindableMessage(_status))
			return false;
		const auto channel = m_channel.load(std::memory_order_relaxed);
		if(channel && (_status & 0x0f) + 1 != channel)
			return false;

		const auto number = static_cast<uint8_t>(_data1 & 0x7f);
		const bool bound = (_status & 0xf0) == 0xb0
			? m_controllers[number].load(std::memory_order_relaxed)
			: m_notes[number].load(std::memory_order_relaxed);
		if(!bound && !m_learning.load(std::memory_order_relaxed))
			return false;

		// Never wait on the message thread here. A message that loses the race
		// is dropped, but it still belongs to the panel, not the machine.
		if(m_pendingMutex.try_lock())
		{
			if(m_pending.size() < g_maxPending)
				m_pending.push_back({ _status, number, static_cast<uint8_t>(_data2 & 0x7f) });
			m_pendingMutex.unlock();
		}
		return true;
	}

	std::string Service::getProjectState() const
	{
		const std::lock_guard lock(m_stateMutex);
		return m_projectState;
	}

	void Service::setProjectState(std::string _text)
	{
		const std::lock_guard lock(m_stateMutex);
		m_loadedState = std::move(_text);
	}

	void Service::service()
	{
		std::optional<std::string> loaded;
		{
			const std::lock_guard lock(m_stateMutex);
			loaded.swap(m_loadedState);
		}
		Table table;
		if(loaded && fromText(*loaded, m_model, table))
			m_controller.loadTable(table);

		{
			const std::lock_guard lock(m_pendingMutex);
			m_draining.swap(m_pending);
		}
		for(const auto& message : m_draining)
			m_controller.handleMessage(message);
		m_draining.clear();
	}

	void Service::publish()
	{
		const auto& table = m_controller.getTable();

		std::array<bool, 128> notes{};
		std::array<bool, 128> controllers{};
		const auto add = [&](const Source& _source)
		{
			if(_source.kind == Source::Kind::Note)
				notes[_source.number & 0x7f] = true;
			else if(_source.kind == Source::Kind::Controller)
				controllers[_source.number & 0x7f] = true;
		};
		for(const auto& encoder : table.encoders)
			add(encoder.source);
		for(const auto& button : table.buttons)
			add(button);
		for(const auto& push : table.pushes)
			add(push);

		for(size_t i = 0; i < 128; ++i)
		{
			m_notes[i].store(notes[i], std::memory_order_relaxed);
			m_controllers[i].store(controllers[i], std::memory_order_relaxed);
		}
		m_channel.store(table.channel, std::memory_order_relaxed);
		m_learning.store(m_controller.getLearnTarget().active(), std::memory_order_relaxed);

		const std::lock_guard lock(m_stateMutex);
		m_projectState = toText(table);
	}
}
