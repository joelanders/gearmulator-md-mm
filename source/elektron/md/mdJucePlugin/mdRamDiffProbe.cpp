#include "mdRamDiffProbe.h"

#if GEARMULATOR_MDMM_RAM_DIAGNOSTICS

#include "baseLib/logging.h"
#include "mdEditor.h"
#include "mdRamDiffOverlay.h"

namespace mdJucePlugin {

RamDiffProbe::RamDiffProbe(Editor& _editor)
    : m_editor(_editor)
{
}

RamDiffProbe::~RamDiffProbe() = default;

void RamDiffProbe::setCapture(std::function<bool(md::ramDiff::RegionKind, std::vector<uint8_t>&)> _capture)
{
    m_capture = std::move(_capture);
}

void RamDiffProbe::setEnabled(bool _enabled)
{
    if(m_enabled == _enabled)
        return;

    m_enabled = _enabled;
    if(!_enabled)
    {
        m_beforeValid = false;
        m_holdCount = 0;
        m_settleTicks = 0;
        m_label.clear();
        m_overlay.reset();
        baseLib::logging::logToConsole("RAM diff probe disabled");
        return;
    }

    if(!m_overlay)
        m_overlay = std::make_unique<RamDiffOverlay>(m_editor);
    baseLib::logging::logToConsole(
        "RAM diff probe enabled: live snapshot of the selected region at 5 Hz");
}

void RamDiffProbe::begin(const std::string& _label)
{
    if(!m_enabled)
        return;

    if(m_settleTicks > 0 && m_holdCount == 0)
        complete();

    if(m_holdCount == 0)
    {
        m_beforeValid = true;
        m_label = _label;
    }
    else if(m_label.find(_label) == std::string::npos)
    {
        m_label += ", ";
        m_label += _label;
    }
    ++m_holdCount;
}

void RamDiffProbe::end()
{
    if(!m_enabled || m_holdCount == 0)
        return;

    --m_holdCount;
    if(m_holdCount == 0)
        m_settleTicks = g_settleTicks;
}

void RamDiffProbe::service()
{
    if(!m_enabled || m_holdCount != 0 || m_settleTicks == 0)
        return;
    if(--m_settleTicks == 0)
        complete();
}

bool RamDiffProbe::captureRamRegion(const md::ramDiff::RegionKind _kind,
    std::vector<uint8_t>& _destination) const
{
    if(!m_capture)
        return false;
    return m_capture(_kind, _destination);
}

void RamDiffProbe::complete()
{
    m_settleTicks = 0;
    m_holdCount = 0;
    if(!m_beforeValid)
    {
        m_label.clear();
        return;
    }

    m_beforeValid = false;
    const std::string label = std::move(m_label);
    m_label.clear();
    if(m_overlay)
        m_overlay->noteAction(label);
}

} // namespace mdJucePlugin

#endif