#pragma once

#include "juce_core/juce_core.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace mdJucePlugin::storageImage
{
	// Reads an unwrapped Monomachine battery-backed storage image. The image is
	// intentionally kept separate from JUCE/plugin state and must be exactly the
	// physical 1 MiB patch-RAM width.
	bool readExact(const juce::File& _source, std::vector<uint8_t>& _bytes,
		juce::String& _error);

	// Replaces _target through a same-directory temporary file. A failed write or
	// promotion leaves the previous recovery image in place.
	bool writeExactAtomically(const juce::File& _target,
		const std::vector<uint8_t>& _bytes, juce::String& _error);

	// Installs the current live storage as recovery before invoking _commit. If
	// commit declines, the prior recovery contents (or prior absence) are restored.
	// The callback is the only portion that may take a device/audio lock; all file
	// I/O is complete before it is entered and resumes only after it returns.
	bool installRecoveryThenCommit(const juce::File& _target,
		const std::vector<uint8_t>& _currentBytes,
		const std::function<bool()>& _commit, juce::String& _error);

	// Reads/writes the self-describing sparse MDPD image used by project state and
	// explicit +Drive import/export. The 512 MiB host cap covers all documented UW
	// Snapshot/sample-bank content while bounding allocations from hostile files.
	bool readPlusDrive(const juce::File& _source, std::vector<uint8_t>& _bytes,
		juce::String& _error);
	bool writePlusDriveAtomically(const juce::File& _target,
		const std::vector<uint8_t>& _bytes, juce::String& _error);
}
