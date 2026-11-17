#pragma once
#include "mdLib/mddevice.h"
#include "mdLib/mdromloader.h"
#include "synthLib/plugin.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using Bytes = std::vector<uint8_t>;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void save(const std::string& path, const Bytes& data)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(data.data()), data.size());
    require(bool(stream), "fixture write failed");
}

// Elektron MM SysEx v0.6: first undo 7-bit encoding, then run-length packing.
Bytes unpack(const Bytes& message)
{
    require(message.size() >= 15, "short dump");
    Bytes packed, result;
    for (size_t i = 10; i < message.size() - 5;) {
        auto mask = message[i++];
        for (int bit = 6; bit >= 0 && i < message.size() - 5; --bit)
            packed.push_back(message[i++] | (((mask >> bit) & 1) << 7));
    }
    for (size_t i = 0; i < packed.size(); ++i) {
        if (!(packed[i] & 128)) result.push_back(packed[i]);
        else {
            const auto count = packed[i] & 127;
            require(count && i + 1 < packed.size(), "invalid run length");
            result.insert(result.end(), count, packed[++i]);
        }
    }
    return result;
}

Bytes pack(const Bytes& header, const Bytes& data)
{
    Bytes packed, wire(header.begin(), header.begin() + 10);
    for (size_t i = 0; i < data.size();) {
        size_t count = 1;
        while (i + count < data.size() && count < 127 && data[i + count] == data[i]) ++count;
        if (count > 1 || (data[i] & 128)) packed.push_back(128 | count);
        packed.push_back(data[i]);
        i += count;
    }
    for (size_t i = 0; i < packed.size();) {
        const auto maskAt = wire.size();
        wire.push_back(0);
        for (int bit = 6; bit >= 0 && i < packed.size(); --bit, ++i) {
            wire[maskAt] |= (packed[i] >> 7) << bit;
            wire.push_back(packed[i] & 127);
        }
    }
    unsigned checksum = 0;
    for (size_t i = 9; i < wire.size(); ++i) checksum += wire[i];
    const auto length = wire.size() - 5; // final message length minus ten
    wire.insert(wire.end(), {uint8_t((checksum >> 7) & 127), uint8_t(checksum & 127),
                            uint8_t(length >> 7), uint8_t(length & 127), 0xf7});
    require(unpack(wire) == data, "packing did not round trip");
    return wire;
}

void advance(md::Hardware& hardware, uint32_t count)
{
    std::array<std::array<float, 256>, 2> scratch{};
    synthLib::TAudioOutputs outputs{};
    outputs[0] = scratch[0].data(); outputs[1] = scratch[1].data();
    while (count) {
        const auto chunk = std::min(count, 256u);
        hardware.processAudio(outputs, chunk, 0);
        count -= chunk;
    }
}

void send(md::Hardware& hardware, const Bytes& bytes)
{
    synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
    event.sysex.assign(bytes.begin(), bytes.end());
    require(hardware.sendMidi(event), "MIDI send rejected");
}

Bytes request(md::Hardware& hardware, uint8_t command)
{
    std::vector<synthLib::SMidiEvent> events;
    hardware.readMidiOut(events);
    send(hardware, {0xf0,0,0x20,0x3c,3,0,uint8_t(command+1),0,0xf7});
    for (unsigned i = 0; i < 3 * 44100 / 256; ++i) {
        advance(hardware, 256);
        events.clear(); hardware.readMidiOut(events);
        for (const auto& e : events)
            if (e.sysex.size() >= 15 && e.sysex[6] == command && e.sysex[9] == 0)
                return Bytes(e.sysex.begin(), e.sysex.end());
    }
    throw std::runtime_error("firmware dump response timed out");
}

void tap(md::Hardware& hardware, md::PanelControl control)
{
    auto packet = md::panelPacket(md::MachineModel::Monomachine, control);
    require(packet.has_value(), "missing panel mapping");
    require(hardware.trySendPanelEvent(packet->row, packet->mask), "panel press rejected");
    advance(hardware, 2048);
    require(hardware.trySendPanelEvent(packet->row, 0), "panel release rejected");
    advance(hardware, 4096);
}

void enterReceive(md::Hardware& hardware)
{
    md::PanelRowState rows;
    auto function = md::panelPacket(md::MachineModel::Monomachine, md::PanelControl::Function).value();
    auto kit = md::panelPacket(md::MachineModel::Monomachine, md::PanelControl::Kit).value();
    for (auto packet : {rows.press(function), rows.press(kit), rows.release(kit), rows.release(function)}) {
        require(hardware.trySendPanelEvent(packet.row, packet.mask), "menu chord rejected");
        advance(hardware, 1024);
    }
    advance(hardware, 6400);
    // Same GLOBAL > FILE > SYSEX RECV > ORIGINAL workflow as the existing
    // firmware SysEx test. Ordinary playback mode does not import these dumps.
    for(auto control : {md::PanelControl::Enter, md::PanelControl::Down, md::PanelControl::Down,
        md::PanelControl::Right, md::PanelControl::Down, md::PanelControl::Enter,
        md::PanelControl::Right, md::PanelControl::Enter}) tap(hardware, control);
}
