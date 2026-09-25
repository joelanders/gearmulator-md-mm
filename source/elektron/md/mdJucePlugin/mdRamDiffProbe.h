#pragma once

#include "mdLib/mdramdiff.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mdJucePlugin {

class Editor;
class RamDiffOverlay;

class RamDiffProbe
{
public:
    explicit RamDiffProbe(Editor& _editor);
    ~RamDiffProbe();

    RamDiffProbe(RamDiffProbe&&) = delete;
    RamDiffProbe(const RamDiffProbe&) = delete;
    RamDiffProbe& operator=(RamDiffProbe&&) = delete;
    RamDiffProbe& operator=(const RamDiffProbe&) = delete;

    void setCapture(std::function<bool(md::ramDiff::RegionKind, std::vector<uint8_t>&)> _capture);

    bool enabled() const { return m_enabled; }

    void setEnabled(bool _enabled);
    void begin(const std::string& _label);
    void end();
    void service();

    bool captureRamRegion(md::ramDiff::RegionKind _kind,
        std::vector<uint8_t>& _destination) const;

private:
    enum { g_settleTicks = 8 };

    void complete();

    bool m_enabled = false;
    bool m_beforeValid = false;
    std::string m_label;
    int m_holdCount = 0;
    int m_settleTicks = 0;

    std::function<bool(md::ramDiff::RegionKind, std::vector<uint8_t>&)> m_capture;
    Editor& m_editor;
    std::unique_ptr<RamDiffOverlay> m_overlay;
};
}
