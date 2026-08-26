#pragma once

namespace mdJucePlugin
{
	// The macOS Metal child view can disagree with the JUCE component's pointer
	// geometry. Default the directly-hosted standalone products to the composited
	// software path, while leaving an explicit user preference, a composite-host
	// override (MachineRack), and plug-in hosts authoritative.
	constexpr bool shouldPersistStandaloneSoftwareRendererDefault(
		const bool _isMacOS, const bool _isStandalone,
		const bool _hasSessionRendererOverride,
		const bool _hasPersistedRendererPreference)
	{
		return _isMacOS && _isStandalone
			&& !_hasSessionRendererOverride
			&& !_hasPersistedRendererPreference;
	}
}
