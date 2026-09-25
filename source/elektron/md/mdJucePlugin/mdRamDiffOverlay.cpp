#include "mdRamDiffOverlay.h"

#if GEARMULATOR_MDMM_RAM_DIAGNOSTICS

#include "mdEditor.h"
#include "mdController.h"
#include "mdPluginProcessor.h"

#include "mdLib/mdRamLabels.h"

#include "jucePluginEditorLib/pluginProcessor.h"

#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/juceRmlComponentConfig.h"
#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlElemCanvas.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"
#include "juceRmlUi/rmlInterfaces.h"
#include "juceRmlUi/rmlDataProvider.h"

#include "juce_events/juce_events.h"
#include "juce_gui_basics/juce_gui_basics.h"

#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/ElementDocument.h"
#include "RmlUi/Core/Elements/ElementFormControlInput.h"
#include "RmlUi/Core/ID.h"
#include "RmlUi/Core/Types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace mdJucePlugin
{
namespace
{
constexpr int g_rowHeight = 18;
constexpr int g_bytesPerRow = static_cast<int>(md::ramDiff::g_bytesPerRow);
constexpr int g_labelGutter = 180;
constexpr double g_highlightMilliseconds = 5000.0;
constexpr const char* g_binaryConfigKey = "ramDiffBinary";
constexpr const char* g_boundsConfigKey = "ramDiffWindowBounds";
constexpr const char* g_annotateConfigKey = "ramDiffAnnotate";

juce::Font monoFont()
{
    return juce::Font(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain);
}

struct Highlight
{
    uint8_t bits = 0;
    double expireMilliseconds = 0;
};

struct DumpState
{
    void selectRegion(const md::ramDiff::RegionKind _kind)
    {
        if(kind == _kind && !bytes.empty())
            return;
        kind = _kind;
        base = md::ramDiff::regionRange(_kind).begin;
        bytes.clear();
        highlights.clear();
        scrollRow = 0;
        totalRows = 0;
    }

    bool refresh(const std::vector<uint8_t>& _next, const double _nowMilliseconds,
        const double _fadeMilliseconds)
    {
        if(bytes.size() != _next.size())
        {
            bytes = _next;
            highlights.clear();
            rebuildRows();
            return false;
        }
        if(_next.empty() || std::memcmp(bytes.data(), _next.data(), bytes.size()) == 0)
            return false;

        for(size_t i = 0; i < bytes.size(); ++i)
        {
            if(bytes[i] == _next[i])
                continue;
            Highlight highlight;
            highlight.bits = static_cast<uint8_t>(bytes[i] ^ _next[i]);
            highlight.expireMilliseconds = _nowMilliseconds + _fadeMilliseconds;
            highlights[base + static_cast<uint32_t>(i)] = highlight;
            bytes[i] = _next[i];
        }
        return true;
    }

    void rebuildRows()
    {
        totalRows = static_cast<int>((bytes.size() + static_cast<size_t>(g_bytesPerRow) - 1)
            / static_cast<size_t>(g_bytesPerRow));
        scrollRow = std::clamp(scrollRow, 0, std::max(0, totalRows - 1));
    }

    int maxScroll(const int _visibleRows) const
    {
        return std::max(0, totalRows - std::max(1, _visibleRows));
    }

    void setScrollRow(const int _row, const int _visibleRows)
    {
        scrollRow = std::clamp(_row, 0, maxScroll(_visibleRows));
    }

    int firstChangeRow() const
    {
        if(highlights.empty())
            return -1;
        int row = totalRows;
        for(const auto& entry : highlights)
            row = std::min(row, rowForAddress(entry.first));
        return row;
    }

    int nextChangeRow(const int _fromRow) const
    {
        int best = -1;
        for(const auto& entry : highlights)
        {
            const auto row = rowForAddress(entry.first);
            if(row > _fromRow && (best < 0 || row < best))
                best = row;
        }
        return best;
    }

    int previousChangeRow(const int _fromRow) const
    {
        int best = -1;
        for(const auto& entry : highlights)
        {
            const auto row = rowForAddress(entry.first);
            if(row < _fromRow && row > best)
                best = row;
        }
        return best;
    }

    bool tickHighlights(const double _nowMilliseconds)
    {
        if(highlights.empty())
            return false;
        for(auto it = highlights.begin(); it != highlights.end();)
        {
            if(it->second.expireMilliseconds <= _nowMilliseconds)
                it = highlights.erase(it);
            else
                ++it;
        }
        return true;
    }

    int rowForAddress(const uint32_t _address) const
    {
        if(_address < base)
            return 0;
        return static_cast<int>(_address - base) / g_bytesPerRow;
    }

    void paintScrollbar(juce::Image& _image, juce::Graphics& _g, const int _visibleRows) const
    {
        _g.fillAll(juce::Colour(0xff161814));
        const auto height = _image.getHeight();
        const auto width = _image.getWidth();
        if(height <= 4 || totalRows <= 0)
            return;

        const auto track = juce::Rectangle<int>(3, 2, std::max(2, width - 6), height - 4);
        _g.setColour(juce::Colour(0xff2a2c28));
        _g.fillRect(track);

        const auto now = juce::Time::getMillisecondCounterHiRes();
        for(const auto& entry : highlights)
        {
            const auto alpha = highlightAlpha(entry.first, now);
            if(alpha <= 0.0f)
                continue;
            const auto row = rowForAddress(entry.first);
            const auto y = track.getY() + (static_cast<float>(row) / static_cast<float>(totalRows))
                * static_cast<float>(track.getHeight());
            _g.setColour(juce::Colour(0xffe74c3c).withMultipliedAlpha(std::max(0.35f, alpha)));
            _g.fillRect(track.getX(), juce::roundToInt(y) - 1, track.getWidth(), 2);
        }

        const auto span = static_cast<float>(std::max(1, _visibleRows))
            / static_cast<float>(std::max(1, totalRows));
        const auto thumbH = std::max(16.0f, span * static_cast<float>(track.getHeight()));
        const auto travel = std::max(0.0f, static_cast<float>(track.getHeight()) - thumbH);
        const auto t = maxScroll(_visibleRows) > 0
            ? static_cast<float>(scrollRow) / static_cast<float>(maxScroll(_visibleRows))
            : 0.0f;
        const auto thumbY = static_cast<float>(track.getY()) + t * travel;
        _g.setColour(juce::Colour(0xff9a9c96));
        _g.fillRoundedRectangle(static_cast<float>(track.getX()), thumbY,
            static_cast<float>(track.getWidth()), thumbH, 2.0f);
    }

    float highlightAlpha(const uint32_t _address, const double _now) const
    {
        const auto it = highlights.find(_address);
        if(it == highlights.end())
            return 0.0f;
        const auto remain = it->second.expireMilliseconds - _now;
        if(remain <= 0.0)
            return 0.0f;
        return static_cast<float>(std::clamp(remain / fadeMilliseconds, 0.0, 1.0));
    }

    uint8_t highlightBits(const uint32_t _address) const
    {
        const auto it = highlights.find(_address);
        return it == highlights.end() ? uint8_t{0} : it->second.bits;
    }

    uint32_t labelBase(const uint8_t _pattern = 0xff, const uint8_t _kit = 0xff,
        const uint8_t _song = 0xff, const uint8_t _global = 0xff) const
    {
        const auto slot = [&](const uint32_t _base, const uint32_t _stride,
            const uint32_t _count, const uint8_t _index) -> uint32_t
        {
            if(_base == UINT32_MAX)
                return base;
            if(_stride == 0 || _index == 0xff || _index >= _count)
                return _base;
            return _base + _stride * _index;
        };
        switch(annotate)
        {
        case md::ramLabels::Annotate::Kit:
            return slot(labelMap.kitBase, labelMap.kitStride, labelMap.kitCount, _kit);
        case md::ramLabels::Annotate::Song:
            return slot(labelMap.songBase, labelMap.songStride, labelMap.songCount, _song);
        case md::ramLabels::Annotate::Global:
            return slot(labelMap.globalBase, labelMap.globalStride, labelMap.globalCount, _global);
        case md::ramLabels::Annotate::Pattern:
        case md::ramLabels::Annotate::All:
            return slot(labelMap.patternBase, labelMap.patternStride, labelMap.patternCount, _pattern);
        case md::ramLabels::Annotate::Off:
            break;
        }
        return base;
    }

    void paint(juce::Image& _image, juce::Graphics& _g) const
    {
        _g.fillAll(juce::Colour(0xff121410));
        if(bytes.empty())
            return;

        const auto labelsOn = annotate != md::ramLabels::Annotate::Off
            && (kind == md::ramDiff::RegionKind::Patch || kind == md::ramDiff::RegionKind::Main);
        const auto gutter = labelsOn ? g_labelGutter : 0;
        const auto font = monoFont();
        _g.setFont(font);
        const auto charW = std::max(7.0f, font.getStringWidthFloat("0"));
        const auto visibleRows = std::max(1, _image.getHeight() / g_rowHeight + 1);
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto fallback = labelBase();

        if(gutter > 0)
        {
            _g.setColour(juce::Colour(0xff161814));
            _g.fillRect(0, 0, gutter, _image.getHeight());
            _g.setColour(juce::Colour(0xff2a2c28));
            _g.fillRect(gutter - 1, 0, 1, _image.getHeight());
        }

        for(int i = 0; i < visibleRows; ++i)
        {
            const auto row = scrollRow + i;
            if(row >= totalRows)
                break;
            const auto offset = static_cast<size_t>(row) * static_cast<size_t>(g_bytesPerRow);
            if(offset >= bytes.size())
                continue;
            const auto count = std::min(static_cast<size_t>(g_bytesPerRow), bytes.size() - offset);
            const auto address = base + static_cast<uint32_t>(offset);
            const auto y = i * g_rowHeight;
            auto x = 8.0f + static_cast<float>(gutter);

            if(labelsOn)
            {
                if(const auto rowHit = md::ramLabels::annotateRow(labelMap, kind, address,
                    annotate, fallback))
                {
                    _g.setColour(juce::Colour(rowHit->color).withAlpha(0.22f));
                    _g.fillRect(0, y, gutter - 1, g_rowHeight);
                    _g.setColour(juce::Colour(rowHit->color));
                    _g.fillRect(0, y + 1, 4, g_rowHeight - 2);
                    _g.setFont(juce::Font(juce::Font::getDefaultSansSerifFontName(), 11.0f,
                        juce::Font::plain));
                    _g.setColour(juce::Colour(0xffeceae3));
                    _g.drawText(rowHit->text, juce::Rectangle<int>(8, y, gutter - 14, g_rowHeight),
                        juce::Justification::centredLeft, true);
                    _g.setFont(font);
                }
            }

            _g.setColour(juce::Colour(0xff8ec37a));
            _g.drawSingleLineText(juce::String::toHexString(static_cast<int>(address))
                .paddedLeft('0', 8).toUpperCase(), juce::roundToInt(x), y + 13);
            x += 10.0f * charW;

            for(size_t b = 0; b < count; ++b)
            {
                const auto addr = address + static_cast<uint32_t>(b);
                const auto value = bytes[offset + b];
                const auto highlight = highlightAlpha(addr, now);
                const auto cellW = binary ? 9.0f * charW : 3.0f * charW;
                if(highlight > 0.0f)
                {
                    _g.setColour(juce::Colour(0xffe74c3c).withMultipliedAlpha(highlight));
                    _g.fillRect(juce::roundToInt(x) - 1, y + 1, juce::roundToInt(cellW), g_rowHeight - 2);
                }
                else if(labelsOn)
                {
                    if(const auto byteHit = md::ramLabels::hit(labelMap, kind, addr, annotate, fallback))
                    {
                        _g.setColour(juce::Colour(byteHit->color).withAlpha(0.16f));
                        _g.fillRect(juce::roundToInt(x) - 1, y + 1, juce::roundToInt(cellW),
                            g_rowHeight - 2);
                    }
                }

                if(binary)
                {
                    const auto bits = highlightBits(addr);
                    auto bitX = x;
                    for(int bit = 7; bit >= 0; --bit)
                    {
                        const auto on = (value & (1u << bit)) != 0;
                        const auto bitChanged = (bits & (1u << bit)) != 0 && highlight > 0.0f;
                        _g.setColour(bitChanged ? juce::Colour(0xfffff6f4)
                            : juce::Colour(0xffd0cec6));
                        _g.drawSingleLineText(on ? "1" : "0", juce::roundToInt(bitX), y + 13);
                        bitX += charW;
                    }
                    x += 9.0f * charW;
                }
                else
                {
                    _g.setColour(highlight > 0.4f ? juce::Colour(0xfffff6f4)
                        : juce::Colour(0xffd0cec6));
                    _g.drawSingleLineText(juce::String::toHexString(static_cast<int>(value))
                        .paddedLeft('0', 2).toUpperCase(), juce::roundToInt(x), y + 13);
                    x += 3.0f * charW;
                }
            }
        }
    }

    std::vector<uint8_t> bytes;
    std::unordered_map<uint32_t, Highlight> highlights;
    md::ramDiff::RegionKind kind = md::ramDiff::RegionKind::Patch;
    uint32_t base = md::memorymap::g_patchBootstrap.begin;
    int totalRows = 0;
    int scrollRow = 0;
    bool binary = false;
    double fadeMilliseconds = g_highlightMilliseconds;
    md::ramLabels::Annotate annotate = md::ramLabels::Annotate::All;
    md::ramLabels::Map labelMap = md::ramLabels::probedMap();
};

class EditorResources final : public juceRmlUi::DataProvider
{
public:
    explicit EditorResources(Editor& _editor) : m_editor(_editor) {}
    const char* getResourceByFilename(const std::string& _name, uint32_t& _size) override
    {
        return m_editor.findResourceByFilename(_name, _size);
    }
    std::vector<std::string> getAllFilenames() override
    {
        return {
            "Roboto-VariableFont_wdth_wght.ttf",
            "Roboto-Italic-VariableFont_wdth_wght.ttf",
            "tus_default.rcss",
            "tus_juceskin.rcss",
            "mdRamDiff.rcss",
            "mdRamDiffWindow.rml",
        };
    }
private:
    Editor& m_editor;
};

} // namespace

class RamDiffOverlay::Window final : public juce::DocumentWindow, private juce::Timer
{
public:
    explicit Window(Editor& _editor)
        : juce::DocumentWindow("RAM", juce::Colour(0xff1b1d1c),
            juce::DocumentWindow::minimiseButton | juce::DocumentWindow::maximiseButton
                | juce::DocumentWindow::closeButton)
        , m_editor(_editor)
        , m_resources(_editor)
        , m_interfaces(m_resources)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setResizeLimits(520, 320, 8192, 8192);

        juceRmlUi::RmlComponentConfig config;
        config.forceSoftwareRenderer = juceRmlUi::SoftwareRendererMode::ForceOn;
        m_rml.reset(new juceRmlUi::RmlComponent(m_interfaces, m_resources, "mdRamDiffWindow.rml",
            1.0f, {}, {}, config));
        setContentNonOwned(m_rml.get(), true);
        bindDocument();

        const auto restored = restoreBounds();
        addToDesktop();
        setVisible(true);
        if(!restored)
            centreWithSize(getWidth(), getHeight());
        toFront(true);
        pollSnapshot(true);
        startTimerHz(20);
    }

    ~Window() override
    {
        stopTimer();
        saveBounds();
        if(m_canvas)
            m_canvas->setRepaintGraphicsCallback({});
        if(m_scroll)
            m_scroll->setRepaintGraphicsCallback({});
        m_canvas = nullptr;
        m_scroll = nullptr;
        m_status = nullptr;
        m_hex = nullptr;
        m_binary = nullptr;
        m_fadeSlider = nullptr;
        m_fadeValue = nullptr;
        setContentNonOwned(nullptr, false);
        m_rml.reset();
    }

    void noteAction(const std::string& _label)
    {
        m_actionLabel = _label;
        m_autoscrollArmed = m_autoscroll;
    }

    void closeButtonPressed() override
    {
        setVisible(false);
        const auto lifetime = m_editor.getLifetimeToken();
        auto* editor = &m_editor;
        juce::MessageManager::callAsync([lifetime, editor]
        {
            if(lifetime.lock())
                editor->setRamDiffEnabled(false);
        });
    }

    void resized() override
    {
        juce::DocumentWindow::resized();
        if(!m_rml)
            return;
        auto* context = m_rml->getContext();
        auto* document = m_rml->getDocument();
        if(!context || !document)
            return;
        juceRmlUi::RmlInterfaces::ScopedAccess access(*m_rml);
        const auto width = m_rml->getWidth();
        const auto height = m_rml->getHeight();
        context->SetDensityIndependentPixelRatio(m_rml->getOpenGLRenderingScale());
        context->SetDimensions({ width, height });
        document->SetProperty(Rml::PropertyId::Width, Rml::Property(static_cast<float>(width), Rml::Unit::PX));
        document->SetProperty(Rml::PropertyId::Height, Rml::Property(static_cast<float>(height), Rml::Unit::PX));
        repaintDump();
    }

private:
    void bindDocument()
    {
        auto* document = m_rml ? m_rml->getDocument() : nullptr;
        if(!document)
            return;

        m_status = juceRmlUi::helper::findChild(document, "ramDiffStatus", false);
        m_hex = juceRmlUi::helper::findChild(document, "ramDiffHex", false);
        m_binary = juceRmlUi::helper::findChild(document, "ramDiffBinary", false);
        auto* canvasElem = juceRmlUi::helper::findChild(document, "ramDiffCanvas", false);
        m_canvas = dynamic_cast<juceRmlUi::ElemCanvas*>(canvasElem);
        if(!m_canvas)
        {
            if(auto* host = juceRmlUi::helper::findChild(document, "ramDiffDumpHost", false))
                m_canvas = juceRmlUi::ElemCanvas::create(host);
        }
        if(m_canvas)
        {
            m_canvas->setPixelAligned(true);
            m_canvas->setClearEveryFrame(false);
            m_canvas->setRepaintGraphicsCallback([this](juce::Image& _image, juce::Graphics& _g)
            {
                m_dump.paint(_image, _g);
            });
            juceRmlUi::EventListener::Add(m_canvas, Rml::EventId::Mousescroll,
                [this](Rml::Event& _event)
                {
                    const auto delta = juceRmlUi::helper::getMouseWheelDelta(_event);
                    m_dump.setScrollRow(m_dump.scrollRow + static_cast<int>(delta.y * 12.0f),
                        visibleRows());
                    repaintDump();
                });
        }

        auto* scrollElem = juceRmlUi::helper::findChild(document, "ramDiffScroll", false);
        m_scroll = dynamic_cast<juceRmlUi::ElemCanvas*>(scrollElem);
        if(m_scroll)
        {
            m_scroll->setPixelAligned(true);
            m_scroll->setClearEveryFrame(false);
            m_scroll->setRepaintGraphicsCallback([this](juce::Image& _image, juce::Graphics& _g)
            {
                m_dump.paintScrollbar(_image, _g, visibleRows());
            });
            const auto scrollToMouse = [this](Rml::Event& _event)
            {
                if(!m_scroll)
                    return;
                const auto mouse = juceRmlUi::helper::getMousePos(_event);
                const auto origin = m_scroll->GetAbsoluteOffset(Rml::BoxArea::Border);
                const auto height = std::max(1.0f, m_scroll->GetBox().GetSize(Rml::BoxArea::Border).y);
                const auto t = std::clamp((mouse.y - origin.y) / height, 0.0f, 1.0f);
                m_dump.setScrollRow(static_cast<int>(t * static_cast<float>(m_dump.maxScroll(visibleRows()))),
                    visibleRows());
                repaintDump();
            };
            juceRmlUi::EventListener::Add(m_scroll, Rml::EventId::Mousedown,
                [this, scrollToMouse](Rml::Event& _event)
                {
                    if(juceRmlUi::helper::getMouseButton(_event) != juceRmlUi::MouseButton::Left)
                        return;
                    m_scrollDragging = true;
                    scrollToMouse(_event);
                });
            juceRmlUi::EventListener::Add(m_scroll, Rml::EventId::Mousemove,
                [this, scrollToMouse](Rml::Event& _event)
                {
                    if(m_scrollDragging)
                        scrollToMouse(_event);
                });
            juceRmlUi::EventListener::Add(document, Rml::EventId::Mousemove,
                [this, scrollToMouse](Rml::Event& _event)
                {
                    if(m_scrollDragging)
                        scrollToMouse(_event);
                });
            juceRmlUi::EventListener::Add(document, Rml::EventId::Mouseup,
                [this](Rml::Event&) { m_scrollDragging = false; });
            juceRmlUi::EventListener::Add(m_scroll, Rml::EventId::Mousescroll,
                [this](Rml::Event& _event)
                {
                    const auto delta = juceRmlUi::helper::getMouseWheelDelta(_event);
                    m_dump.setScrollRow(m_dump.scrollRow + static_cast<int>(delta.y * 12.0f),
                        visibleRows());
                    repaintDump();
                });
        }

        const auto binary = m_editor.getProcessor().getConfig().getBoolValue(g_binaryConfigKey, false);
        m_dump.binary = binary;
        juceRmlUi::ElemButton::setChecked(m_hex, !binary);
        juceRmlUi::ElemButton::setChecked(m_binary, binary);

        bindMode(m_hex, false);
        bindMode(m_binary, true);
        bindRegion("ramDiffGotoPatch", md::ramDiff::RegionKind::Patch);
        bindRegion("ramDiffGotoMain", md::ramDiff::RegionKind::Main);
        bindRegion("ramDiffGotoSram", md::ramDiff::RegionKind::Sram);
        bindRegion("ramDiffGotoLoader", md::ramDiff::RegionKind::Loader);
        bindStep("ramDiffPrevChange", false);
        bindStep("ramDiffNextChange", true);
        bindAnnotate();

        auto& config = m_editor.getProcessor().getConfig();
        m_autoscroll = config.getBoolValue("ramDiffAutoscroll", true);
        if(auto* autoBtn = juceRmlUi::helper::findChild(document, "ramDiffAutoscroll", false))
        {
            juceRmlUi::ElemButton::setChecked(autoBtn, m_autoscroll);
            juceRmlUi::EventListener::AddClick(autoBtn, [this, autoBtn]
            {
                m_autoscroll = juceRmlUi::ElemButton::isChecked(autoBtn);
                auto& cfg = m_editor.getProcessor().getConfig();
                cfg.setValue("ramDiffAutoscroll", m_autoscroll);
                cfg.saveIfNeeded();
            });
        }

        m_dump.fadeMilliseconds = 1000.0 * std::clamp(
            config.getIntValue("ramDiffFadeSeconds", 5), 1, 30);
        m_fadeSlider = juceRmlUi::helper::findChildT<Rml::ElementFormControlInput>(
            document, "ramDiffFade", false);
        m_fadeValue = juceRmlUi::helper::findChild(document, "ramDiffFadeValue", false);
        if(m_fadeSlider)
        {
            m_fadeSlider->SetValue(std::to_string(static_cast<int>(
                m_dump.fadeMilliseconds / 1000.0)));
            updateFadeLabel();
            juceRmlUi::EventListener::Add(m_fadeSlider, Rml::EventId::Change, [this](Rml::Event&)
            {
                if(!m_fadeSlider)
                    return;
                const auto seconds = std::clamp(std::atoi(m_fadeSlider->GetValue().c_str()), 1, 30);
                m_dump.fadeMilliseconds = 1000.0 * seconds;
                auto& cfg = m_editor.getProcessor().getConfig();
                cfg.setValue("ramDiffFadeSeconds", seconds);
                cfg.saveIfNeeded();
                updateFadeLabel();
            });
        }

        m_dump.annotate = static_cast<md::ramLabels::Annotate>(std::clamp(
            config.getIntValue(g_annotateConfigKey, 1), 0, 5));
        updateAnnotateButtons();

        const auto region = static_cast<md::ramDiff::RegionKind>(std::clamp(
            config.getIntValue("ramDiffRegion", 0), 0, 3));
        selectRegion(region, false);
    }

    void bindMode(Rml::Element* _button, const bool _binary)
    {
        if(!_button)
            return;
        juceRmlUi::EventListener::AddClick(_button, [this, _binary]
        {
            m_dump.binary = _binary;
            juceRmlUi::ElemButton::setChecked(m_hex, !_binary);
            juceRmlUi::ElemButton::setChecked(m_binary, _binary);
            auto& config = m_editor.getProcessor().getConfig();
            config.setValue(g_binaryConfigKey, _binary);
            config.saveIfNeeded();
            repaintDump();
        });
    }

    void bindAnnotate()
    {
        const auto bind = [this](const char* const _id, const md::ramLabels::Annotate _mode)
        {
            auto* document = m_rml ? m_rml->getDocument() : nullptr;
            if(!document)
                return;
            auto* button = juceRmlUi::helper::findChild(document, _id, false);
            if(!button)
                return;
            juceRmlUi::EventListener::AddClick(button, [this, _mode]
            {
                m_dump.annotate = _mode;
                auto& config = m_editor.getProcessor().getConfig();
                config.setValue(g_annotateConfigKey, static_cast<int>(_mode));
                config.saveIfNeeded();
                updateAnnotateButtons();
                scrollToAnnotateBase();
                updateStatus(true);
                repaintDump();
            });
        };
        bind("ramDiffAnnotateOff", md::ramLabels::Annotate::Off);
        bind("ramDiffAnnotateAll", md::ramLabels::Annotate::All);
        bind("ramDiffAnnotatePattern", md::ramLabels::Annotate::Pattern);
        bind("ramDiffAnnotateKit", md::ramLabels::Annotate::Kit);
        bind("ramDiffAnnotateSong", md::ramLabels::Annotate::Song);
        bind("ramDiffAnnotateGlobal", md::ramLabels::Annotate::Global);
    }

    const Controller* controller() const
    {
        return dynamic_cast<const Controller*>(&m_editor.getProcessor().getController());
    }

    void currentSlots(uint8_t& _pattern, uint8_t& _kit, uint8_t& _song, uint8_t& _global) const
    {
        _pattern = _kit = _song = _global = 0xff;
        if(const auto* ctrl = controller())
        {
            _pattern = ctrl->getCurrentPattern();
            _kit = ctrl->getCurrentKit();
            _song = ctrl->getCurrentSong();
            _global = ctrl->getCurrentGlobal();
        }
    }

    void scrollToAnnotateBase()
    {
        if(m_dump.annotate == md::ramLabels::Annotate::Off)
            return;
        uint8_t pattern = 0xff, kit = 0xff, song = 0xff, global = 0xff;
        currentSlots(pattern, kit, song, global);
        const auto base = m_dump.labelBase(pattern, kit, song, global);
        if(base < m_dump.base)
            return;
        if(base >= m_dump.base + static_cast<uint32_t>(m_dump.bytes.size()))
            return;
        m_dump.setScrollRow(m_dump.rowForAddress(base), visibleRows());
    }

    void updateAnnotateButtons()
    {
        auto* document = m_rml ? m_rml->getDocument() : nullptr;
        if(!document)
            return;
        const auto set = [this, document](const char* _id, const md::ramLabels::Annotate _mode)
        {
            if(auto* button = juceRmlUi::helper::findChild(document, _id, false))
                juceRmlUi::ElemButton::setChecked(button, m_dump.annotate == _mode);
        };
        set("ramDiffAnnotateOff", md::ramLabels::Annotate::Off);
        set("ramDiffAnnotateAll", md::ramLabels::Annotate::All);
        set("ramDiffAnnotatePattern", md::ramLabels::Annotate::Pattern);
        set("ramDiffAnnotateKit", md::ramLabels::Annotate::Kit);
        set("ramDiffAnnotateSong", md::ramLabels::Annotate::Song);
        set("ramDiffAnnotateGlobal", md::ramLabels::Annotate::Global);
    }

    const char* annotateName() const
    {
        switch(m_dump.annotate)
        {
        case md::ramLabels::Annotate::All: return "all map";
        case md::ramLabels::Annotate::Kit: return "kit map";
        case md::ramLabels::Annotate::Pattern: return "pattern map";
        case md::ramLabels::Annotate::Song: return "song map";
        case md::ramLabels::Annotate::Global: return "global map";
        case md::ramLabels::Annotate::Off: return "no map";
        }
        return "no map";
    }

    void bindRegion(const char* const _id, const md::ramDiff::RegionKind _kind)
    {
        auto* document = m_rml->getDocument();
        if(!document)
            return;
        auto* button = juceRmlUi::helper::findChild(document, _id, false);
        if(!button)
            return;
        juceRmlUi::EventListener::AddClick(button, [this, _kind]
        {
            selectRegion(_kind, true);
        });
    }

    void selectRegion(const md::ramDiff::RegionKind _kind, const bool _save)
    {
        m_dump.selectRegion(_kind);
        updateRegionButtons();
        if(_save)
        {
            auto& config = m_editor.getProcessor().getConfig();
            config.setValue("ramDiffRegion", static_cast<int>(_kind));
            config.saveIfNeeded();
        }
        pollSnapshot(true);
    }

    void updateRegionButtons()
    {
        auto* document = m_rml ? m_rml->getDocument() : nullptr;
        if(!document)
            return;
        const auto set = [this, document](const char* _id, const md::ramDiff::RegionKind _kind)
        {
            if(auto* button = juceRmlUi::helper::findChild(document, _id, false))
                juceRmlUi::ElemButton::setChecked(button, m_dump.kind == _kind);
        };
        set("ramDiffGotoPatch", md::ramDiff::RegionKind::Patch);
        set("ramDiffGotoMain", md::ramDiff::RegionKind::Main);
        set("ramDiffGotoSram", md::ramDiff::RegionKind::Sram);
        set("ramDiffGotoLoader", md::ramDiff::RegionKind::Loader);
    }

    void updateFadeLabel()
    {
        if(m_fadeValue)
            m_fadeValue->SetInnerRML(std::to_string(static_cast<int>(
                m_dump.fadeMilliseconds / 1000.0)) + "s");
    }

    void pollSnapshot(const bool _force)
    {
        std::vector<uint8_t> next;
        if(!m_editor.getRamDiffProbe().captureRamRegion(m_dump.kind, next))
            return;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto changed = m_dump.refresh(next, now, m_dump.fadeMilliseconds);
        if(changed && m_autoscrollArmed)
        {
            const auto first = m_dump.firstChangeRow();
            if(first >= 0)
                m_dump.setScrollRow(first, visibleRows());
            m_autoscrollArmed = false;
        }
        if(changed || _force)
        {
            updateStatus(changed);
            repaintDump();
        }
    }

    void updateStatus(const bool _changed)
    {
        if(!m_status)
            return;
        std::string text;
        if(_changed && !m_actionLabel.empty())
        {
            text = "Last change: " + m_actionLabel;
            text += "  ·  " + std::to_string(m_dump.highlights.size()) + " bytes";
            if(!m_dump.highlights.empty())
            {
                uint32_t firstAddr = UINT32_MAX;
                for(const auto& entry : m_dump.highlights)
                    firstAddr = std::min(firstAddr, entry.first);
                if(const auto found = md::ramLabels::hit(m_dump.labelMap, m_dump.kind, firstAddr,
                    m_dump.annotate, m_dump.labelBase()))
                {
                    text += "  ·  ";
                    text += found->text;
                }
            }
        }
        else
        {
            text = std::string("Live ") + md::ramDiff::regionName(m_dump.kind)
                + "  ·  " + std::to_string(m_dump.bytes.size()) + " bytes";
        }
        text += "  ·  ";
        text += annotateName();
        uint8_t pattern = 0xff, kit = 0xff, song = 0xff, global = 0xff;
        currentSlots(pattern, kit, song, global);
        text += "  ·  ";
        text += pattern == 0xff ? std::string("A??") : md::ramLabels::patternSlotName(pattern);
        text += " ";
        text += kit == 0xff ? std::string("kit ??") : md::ramLabels::kitSlotName(kit);
        text += " ";
        text += song == 0xff ? std::string("song ??") : md::ramLabels::songSlotName(song);
        text += " ";
        text += global == 0xff ? std::string("global ?") : md::ramLabels::globalSlotName(global);
        if(m_dump.annotate != md::ramLabels::Annotate::Off)
        {
            char addr[16];
            std::snprintf(addr, sizeof(addr), " @ 0x%08x",
                m_dump.labelBase(pattern, kit, song, global));
            text += addr;
        }
        m_status->SetInnerRML(text);
    }

    void bindStep(const char* const _id, const bool _forward)
    {
        auto* document = m_rml->getDocument();
        if(!document)
            return;
        auto* button = juceRmlUi::helper::findChild(document, _id, false);
        if(!button)
            return;
        juceRmlUi::EventListener::AddClick(button, [this, _forward]
        {
            const auto current = m_dump.scrollRow;
            const auto row = _forward
                ? m_dump.nextChangeRow(current)
                : m_dump.previousChangeRow(current);
            if(row >= 0)
                m_dump.setScrollRow(row, visibleRows());
            repaintDump();
        });
    }

    int visibleRows() const
    {
        if(!m_canvas)
            return 1;
        return std::max(1, m_canvas->getPaintSize().y / g_rowHeight);
    }

    void repaintDump()
    {
        if(m_canvas)
            m_canvas->repaint();
        if(m_scroll)
            m_scroll->repaint();
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if(now - m_lastSnapshotMilliseconds >= 200.0)
        {
            m_lastSnapshotMilliseconds = now;
            pollSnapshot(false);
        }
        if(m_dump.tickHighlights(now))
            repaintDump();
    }

    bool restoreBounds()
    {
        const auto stored = m_editor.getProcessor().getConfig().getValue(g_boundsConfigKey);
        if(stored.isEmpty())
        {
            setSize(960, 640);
            return false;
        }
        juce::StringArray parts;
        parts.addTokens(stored, ",", "");
        if(parts.size() != 4)
        {
            setSize(960, 640);
            return false;
        }
        setBounds(parts[0].getIntValue(), parts[1].getIntValue(),
            std::max(520, parts[2].getIntValue()), std::max(320, parts[3].getIntValue()));
        return true;
    }

    void saveBounds() const
    {
        const auto bounds = getBounds();
        auto& config = m_editor.getProcessor().getConfig();
        config.setValue(g_boundsConfigKey, juce::String(bounds.getX()) + ","
            + juce::String(bounds.getY()) + "," + juce::String(bounds.getWidth()) + ","
            + juce::String(bounds.getHeight()));
        config.saveIfNeeded();
    }

    Editor& m_editor;
    EditorResources m_resources;
    juceRmlUi::RmlInterfaces m_interfaces;
    std::unique_ptr<juceRmlUi::RmlComponent> m_rml;
    juceRmlUi::ElemCanvas* m_canvas = nullptr;
    juceRmlUi::ElemCanvas* m_scroll = nullptr;
    bool m_scrollDragging = false;
    Rml::Element* m_status = nullptr;
    Rml::Element* m_hex = nullptr;
    Rml::Element* m_binary = nullptr;
    DumpState m_dump;
    std::string m_actionLabel;
    bool m_autoscroll = true;
    bool m_autoscrollArmed = false;
    double m_lastSnapshotMilliseconds = 0;
    Rml::ElementFormControlInput* m_fadeSlider = nullptr;
    Rml::Element* m_fadeValue = nullptr;
};

RamDiffOverlay::RamDiffOverlay(Editor& _editor)
    : m_editor(_editor)
    , m_window(std::make_unique<Window>(_editor))
{
}

RamDiffOverlay::~RamDiffOverlay() = default;

void RamDiffOverlay::noteAction(const std::string& _label)
{
    if(m_window)
        m_window->noteAction(_label);
}

} // namespace mdJucePlugin

#endif