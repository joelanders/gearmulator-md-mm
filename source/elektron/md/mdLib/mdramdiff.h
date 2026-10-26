#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "mdmemorymap.h"

namespace md::ramDiff
{

inline constexpr const char* g_editorConfigKey = "ramDiffProbe";
inline constexpr const char* g_regionMaskConfigKey = "ramDiffRegions";
inline constexpr bool g_editorDefaultEnabled = false;
inline constexpr size_t g_bytesPerRow = 16;
inline constexpr size_t g_maxPrintedRows = 64;
inline constexpr size_t g_rangeIoMaxBytes = 0x2000;

enum class RegionKind : uint8_t
{
    Patch,
    Main,
    Sram,
    Loader,
};

inline constexpr uint8_t g_regionPatch  = 1u << 0;
inline constexpr uint8_t g_regionMain   = 1u << 1;
inline constexpr uint8_t g_regionSram   = 1u << 2;
inline constexpr uint8_t g_regionLoader = 1u << 3;
inline constexpr uint8_t g_regionAll    = g_regionPatch | g_regionMain | g_regionSram | g_regionLoader;

inline constexpr uint8_t regionFlag(const RegionKind _kind)
{
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(_kind));
}

inline const char* regionName(const RegionKind _kind)
{
    switch(_kind)
    {
    case RegionKind::Patch: return "patch";
    case RegionKind::Main: return "main";
    case RegionKind::Sram: return "sram";
    case RegionKind::Loader: return "loader";
    }
    return "";
}

inline memorymap::Range regionRange(const RegionKind _kind)
{
    switch(_kind)
    {
    case RegionKind::Patch: return memorymap::g_patchBootstrap;
    case RegionKind::Main: return memorymap::g_mainRam;
    case RegionKind::Sram: return memorymap::g_internalSram;
    case RegionKind::Loader: return memorymap::g_loaderRam;
    }
    return memorymap::g_patchBootstrap;
}

inline bool ramRangeValid(const RegionKind _kind, const uint32_t _address, const size_t _size)
{
    if(_size == 0 || _size > g_rangeIoMaxBytes)
        return false;
    const auto range = regionRange(_kind);
    if(_address < range.begin)
        return false;
    const auto offset = static_cast<uint64_t>(_address) - range.begin;
    if(offset >= range.size())
        return false;
    return _size <= range.size() - static_cast<size_t>(offset);
}

struct Region
{
    RegionKind kind = RegionKind::Patch;
    const char* name = "";
    uint32_t base = 0;
    std::vector<uint8_t> bytes;
};

struct Image
{
    Region patch;
    Region main;
    Region sram;
    Region loader;
};

struct Row
{
    RegionKind kind = RegionKind::Patch;
    const char* region = "";
    uint32_t address = 0;
    std::array<uint8_t, g_bytesPerRow> before{};
    std::array<uint8_t, g_bytesPerRow> after{};
    uint16_t presentMask = 0;
    uint16_t changedMask = 0;
};

struct Report
{
    std::vector<Row> rows;
    size_t changedBytes = 0;
    bool sizeMismatch = false;
};

inline void copyBytes(std::vector<uint8_t>& _destination, const std::vector<uint8_t>& _source)
{
    if(_destination.size() != _source.size())
        _destination.resize(_source.size());
    if(!_source.empty())
        std::memcpy(_destination.data(), _source.data(), _source.size());
}

inline void prepareRegion(Region& _region, const RegionKind _kind, const memorymap::Range _range)
{
    _region.kind = _kind;
    _region.name = regionName(_kind);
    _region.base = _range.begin;
    if(_region.bytes.size() != _range.size())
        _region.bytes.resize(_range.size());
}

inline void prepareImage(Image& _image)
{
    prepareRegion(_image.patch, RegionKind::Patch, memorymap::g_patchBootstrap);
    prepareRegion(_image.main, RegionKind::Main, memorymap::g_mainRam);
    prepareRegion(_image.sram, RegionKind::Sram, memorymap::g_internalSram);
    prepareRegion(_image.loader, RegionKind::Loader, memorymap::g_loaderRam);
}

template<typename Fn>
void forEachDisplayRegion(Image& _image, Fn&& _fn)
{
    _fn(_image.patch);
    _fn(_image.main);
    _fn(_image.loader);
    _fn(_image.sram);
}

template<typename Fn>
void forEachDisplayRegion(const Image& _image, Fn&& _fn)
{
    _fn(_image.patch);
    _fn(_image.main);
    _fn(_image.loader);
    _fn(_image.sram);
}

inline void diffRegion(Report& _report, const Region& _before, const Region& _after)
{
    if(_before.bytes.size() != _after.bytes.size())
        _report.sizeMismatch = true;

    const auto size = std::min(_before.bytes.size(), _after.bytes.size());
    for(size_t offset = 0; offset < size; offset += g_bytesPerRow)
    {
        Row row;
        row.kind = _after.name[0] ? _after.kind : _before.kind;
        row.region = _after.name[0] ? _after.name : _before.name;
        row.address = _before.base + static_cast<uint32_t>(offset);
        for(size_t byte = 0; byte < g_bytesPerRow && offset + byte < size; ++byte)
        {
            const auto before = _before.bytes[offset + byte];
            const auto after = _after.bytes[offset + byte];
            row.before[byte] = before;
            row.after[byte] = after;
            row.presentMask = static_cast<uint16_t>(row.presentMask | (1u << byte));
            if(before == after)
                continue;
            row.changedMask = static_cast<uint16_t>(row.changedMask | (1u << byte));
            ++_report.changedBytes;
        }
        if(row.changedMask)
            _report.rows.push_back(row);
    }
}

inline Report diff(const Image& _before, const Image& _after, const uint8_t _mask = g_regionAll)
{
    Report report;
    if(_mask & g_regionPatch)
        diffRegion(report, _before.patch, _after.patch);
    if(_mask & g_regionMain)
        diffRegion(report, _before.main, _after.main);
    if(_mask & g_regionSram)
        diffRegion(report, _before.sram, _after.sram);
    if(_mask & g_regionLoader)
        diffRegion(report, _before.loader, _after.loader);
    return report;
}

inline Report filter(const Report& _report, const uint8_t _mask)
{
    Report report;
    report.sizeMismatch = _report.sizeMismatch;
    report.rows.reserve(_report.rows.size());
    for(const auto& row : _report.rows)
    {
        if((regionFlag(row.kind) & _mask) == 0)
            continue;
        report.rows.push_back(row);
        for(size_t byte = 0; byte < g_bytesPerRow; ++byte)
        {
            if(row.changedMask & (1u << byte))
                ++report.changedBytes;
        }
    }
    return report;
}

inline void appendRowBytes(std::ostringstream& _out, const std::array<uint8_t, g_bytesPerRow>& _bytes,
    const uint16_t _presentMask, const uint16_t _changedMask)
{
    bool first = true;
    for(size_t byte = 0; byte < g_bytesPerRow; ++byte)
    {
        if((_presentMask & (1u << byte)) == 0)
            continue;
        if(!first)
            _out << ' ';
        first = false;
        const auto changed = (_changedMask & (1u << byte)) != 0;
        if(changed)
            _out << '[';
        _out << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<unsigned>(_bytes[byte]);
        if(changed)
            _out << ']';
    }
}

inline std::string formatHex(const Report& _report, const std::string& _label,
    const size_t _maxRows = g_maxPrintedRows)
{
    std::ostringstream out;
    out << "RAM diff [" << _label << "] " << std::dec << _report.changedBytes
        << " byte" << (_report.changedBytes == 1 ? "" : "s")
        << " in " << _report.rows.size()
        << " row" << (_report.rows.size() == 1 ? "" : "s");
    if(_report.sizeMismatch)
        out << " (region size mismatch)";
    if(_report.rows.empty())
    {
        out << "\n  no changes";
        return out.str();
    }

    const auto printed = std::min(_report.rows.size(), _maxRows);
    for(size_t i = 0; i < printed; ++i)
    {
        const auto& row = _report.rows[i];
        out << "\n  " << row.region << " 0x" << std::hex << std::setfill('0')
            << std::setw(8) << row.address << "\n    ";
        appendRowBytes(out, row.before, row.presentMask, row.changedMask);
        out << "\n    ";
        appendRowBytes(out, row.after, row.presentMask, row.changedMask);
    }
    if(_report.rows.size() > _maxRows)
        out << "\n  ... " << std::dec << (_report.rows.size() - _maxRows) << " more rows";
    return out.str();
}

inline void appendRmlBytes(std::ostringstream& _out, const std::array<uint8_t, g_bytesPerRow>& _bytes,
    const uint16_t _presentMask, const uint16_t _changedMask, const char* const _changedClass)
{
    for(size_t byte = 0; byte < g_bytesPerRow; ++byte)
    {
        if((_presentMask & (1u << byte)) == 0)
            continue;
        if(byte)
            _out << ' ';
        const auto changed = (_changedMask & (1u << byte)) != 0;
        _out << "<span class=\"ramDiffByte";
        if(changed)
            _out << ' ' << _changedClass;
        _out << "\">" << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<unsigned>(_bytes[byte]) << "</span>";
    }
}

inline std::string formatRml(const Report& _report, const std::string& _label,
    const size_t _maxRows = g_maxPrintedRows)
{
    std::ostringstream out;
    out << "<div class=\"ramDiffSummary\">RAM diff [" << _label << "] "
        << std::dec << _report.changedBytes
        << " byte" << (_report.changedBytes == 1 ? "" : "s")
        << " in " << _report.rows.size()
        << " row" << (_report.rows.size() == 1 ? "" : "s");
    if(_report.sizeMismatch)
        out << " (region size mismatch)";
    out << "</div>";
    if(_report.rows.empty())
    {
        out << "<div class=\"ramDiffEmpty\">no changes</div>";
        return out.str();
    }

    const auto printed = std::min(_report.rows.size(), _maxRows);
    for(size_t i = 0; i < printed; ++i)
    {
        const auto& row = _report.rows[i];
        out << "<div class=\"ramDiffBlock\"><div class=\"ramDiffAddr\">"
            << row.region << " 0x" << std::hex << std::setfill('0') << std::setw(8)
            << row.address << "</div><div class=\"ramDiffLine ramDiffOriginal\">";
        appendRmlBytes(out, row.before, row.presentMask, row.changedMask, "ramDiffChangedBefore");
        out << "</div><div class=\"ramDiffLine ramDiffUpdated\">";
        appendRmlBytes(out, row.after, row.presentMask, row.changedMask, "ramDiffChangedAfter");
        out << "</div></div>";
    }
    if(_report.rows.size() > _maxRows)
        out << "<div class=\"ramDiffEmpty\">... " << std::dec
            << (_report.rows.size() - _maxRows) << " more rows</div>";
    return out.str();
}

}