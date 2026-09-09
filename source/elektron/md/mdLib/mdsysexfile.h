#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mdtypes.h"

namespace md
{
	inline constexpr size_t g_midiSysexTransferMaxBytes = 8u * 1024u * 1024u;

	enum class MidiSysexStreamValidation : uint8_t
	{
		Valid, Empty, TooLarge, InvalidFraming, InvalidDataByte,
		ChecksumMismatch, UnsupportedMessage, WrongModel, FirmwareUpdate,
		InvalidSampleHeader, InvalidSampleSequence, UnsupportedSampleExtension,
		TransportMessage, OtherManufacturer, UnknownElektronCommand
	};

	enum class MidiSysexMessageKind : uint8_t
	{
		UserDump, DigiPro, SampleName, SdsHeader, SdsPacket
	};

	struct MidiSysexMessage
	{
		size_t offset = 0;
		size_t size = 0;
		MidiSysexMessageKind kind = MidiSysexMessageKind::UserDump;
		bool lastSamplePacket = false;
	};

	inline const char* midiSysexValidationMessage(const MidiSysexStreamValidation _result)
	{
		switch(_result)
		{
		case MidiSysexStreamValidation::Valid: return "Ready to send.";
		case MidiSysexStreamValidation::Empty: return "The selected file is empty.";
		case MidiSysexStreamValidation::TooLarge: return "The selected file exceeds the 8 MiB safety limit.";
		case MidiSysexStreamValidation::InvalidFraming: return "The file contains incomplete or incorrectly framed SysEx messages.";
		case MidiSysexStreamValidation::InvalidDataByte: return "A SysEx message contains a non-7-bit data byte.";
		case MidiSysexStreamValidation::ChecksumMismatch: return "A SysEx message has an invalid checksum or declared length.";
		case MidiSysexStreamValidation::WrongModel: return "This file contains data for a different machine model. SDS samples require Machinedrum UW.";
		case MidiSysexStreamValidation::FirmwareUpdate: return "OS updates require a separate update workflow; this command imports user data only.";
		case MidiSysexStreamValidation::InvalidSampleHeader: return "An SDS sample header has an invalid size, resolution, period, or loop range.";
		case MidiSysexStreamValidation::InvalidSampleSequence: return "An SDS sample is incomplete, has out-of-order packets, or is interrupted by another message.";
		case MidiSysexStreamValidation::UnsupportedSampleExtension: return "This file uses a Sample Dump Extension that is not supported by this importer.";
		case MidiSysexStreamValidation::TransportMessage: return "This file contains handshake or TurboMIDI negotiation messages, not importable user data.";
		case MidiSysexStreamValidation::OtherManufacturer: return "This file contains SysEx for an unsupported manufacturer.";
		case MidiSysexStreamValidation::UnknownElektronCommand: return "This file contains an unrecognized Elektron command. It has not been sent.";
		case MidiSysexStreamValidation::UnsupportedMessage: return "This file contains a control or request message rather than importable user data.";
		}
		return "The file could not be prepared.";
	}

	// Pure, bounded file parsing, before acquiring the device lock. The optional
	// descriptors preserve protocol boundaries without copying individual messages.
	inline MidiSysexStreamValidation parseMidiSysexFile(const std::vector<uint8_t>& _bytes,
		const MachineModel _model, std::vector<MidiSysexMessage>* _messages = nullptr)
	{
		using Result = MidiSysexStreamValidation;
		if(_messages) _messages->clear();
		if(_bytes.empty()) return Result::Empty;
		if(_bytes.size() > g_midiSysexTransferMaxBytes) return Result::TooLarge;
		const uint8_t product = _model == MachineModel::Monomachine ? 3 : 2;
		size_t remainingPackets = 0, samplePackets = 0;
		uint8_t sampleDevice = 0;
		bool sampleNamed = false;
		for(size_t offset = 0; offset < _bytes.size();)
		{
			if(_bytes[offset] != 0xf0) return Result::InvalidFraming;
			const auto end = std::find(_bytes.begin() + offset + 1, _bytes.end(), uint8_t{0xf7});
			if(end == _bytes.end()) return Result::InvalidFraming;
			const auto size = size_t(end - _bytes.begin()) + 1 - offset;
			if(size < 3) return Result::InvalidFraming;
			const auto* b = _bytes.data() + offset;
			if(std::any_of(b + 1, b + size - 1, [](uint8_t v) { return v > 0x7f; }))
				return Result::InvalidDataByte;
			MidiSysexMessage message{offset, size};
			if(b[1] == 0x7e)
			{
				if(size < 5) return Result::InvalidFraming;
				const auto command = b[3];
				if(command >= 0x7b && command <= 0x7f) return Result::TransportMessage;
				if(command == 5) return Result::UnsupportedSampleExtension;
				if(command != 1 && command != 2) return Result::UnsupportedMessage;
				if(_model != MachineModel::Machinedrum) return Result::WrongModel;
				if(command == 1)
				{
					if(remainingPackets) return Result::InvalidSampleSequence;
					if(size != 21 || b[6] < 8 || b[6] > 28) return Result::InvalidSampleHeader;
					const auto value = [b](size_t i) { return uint32_t(b[i]) | (uint32_t(b[i+1]) << 7) | (uint32_t(b[i+2]) << 14); };
					const auto words = value(10);
					if(!value(7) || !words || (b[19] != 0 && b[19] != 1 && b[19] != 0x7f)
						|| (b[19] != 0x7f && (value(13) > value(16) || value(16) > words)))
						return Result::InvalidSampleHeader;
					const size_t wordsPerPacket = 120 / ((b[6] + 6) / 7);
					remainingPackets = (words + wordsPerPacket - 1) / wordsPerPacket;
					samplePackets = 0;
					sampleDevice = b[2];
					sampleNamed = false;
					message.kind = MidiSysexMessageKind::SdsHeader;
				}
				else
				{
					if(!remainingPackets || size != 127 || b[2] != sampleDevice
						|| b[4] != (samplePackets & 0x7f)) return Result::InvalidSampleSequence;
					uint8_t checksum = 0;
					for(size_t i = 1; i < 125; ++i) checksum ^= b[i];
					if(checksum != b[125]) return Result::ChecksumMismatch;
					++samplePackets;
					message.lastSamplePacket = --remainingPackets == 0;
					message.kind = MidiSysexMessageKind::SdsPacket;
				}
			}
			else if(b[1] == 0x7f) return Result::UnsupportedMessage;
			else if(b[1] == 0 && size >= 5 && b[2] == 0x20 && b[3] == 0x3c)
			{
				if(size < 8) return Result::InvalidFraming;
				if(b[4] == 0) return Result::TransportMessage;
				if(b[4] != product) return Result::WrongModel;
				// Preserve the old sender's tolerance of the unused/base-channel
				// byte. Canonical dumps use zero; it is not part of the checksum.
				const auto command = b[6];
				if(command == 0x7e || command == 0x7f) return Result::FirmwareUpdate;
				const bool name = product == 2 && command == 0x73;
				if(remainingPackets && (!name || samplePackets || sampleNamed))
					return Result::InvalidSampleSequence;
				if(name)
				{
					if(size != 13 || (!remainingPackets && b[7] > 47)) return Result::InvalidSampleHeader;
					sampleNamed = true;
					message.kind = MidiSysexMessageKind::SampleName;
				}
				else
				{
					const bool digiPro = product == 3 && command == 0x5d;
					if(command != 0x50 && command != 0x52 && command != 0x67 && command != 0x69 && !digiPro)
						return (command >= 0x50 && command <= 0x74) ? Result::UnsupportedMessage : Result::UnknownElektronCommand;
					if(size < 15 || size - 10 > 0x3fff) return Result::ChecksumMismatch;
					uint32_t sum = 0;
					for(size_t i = digiPro ? 10 : 9; i < size - 5; ++i) sum += b[i];
					if((sum & 0x3fff) != uint32_t((b[size-5] << 7) | b[size-4])
						|| size - 10 != size_t((b[size-3] << 7) | b[size-2])) return Result::ChecksumMismatch;
					message.kind = digiPro ? MidiSysexMessageKind::DigiPro : MidiSysexMessageKind::UserDump;
				}
			}
			else return Result::OtherManufacturer;
			if(_messages) _messages->push_back(message);
			offset += size;
		}
		return remainingPackets ? Result::InvalidSampleSequence : Result::Valid;
	}

	inline MidiSysexStreamValidation validateMidiSysexStream(const std::vector<uint8_t>& _bytes,
		const MachineModel _model)
	{
		return parseMidiSysexFile(_bytes, _model);
	}
}
