#pragma once

namespace mdJucePlugin
{
	// Early MD/MM alpha builds persisted software rendering for every macOS
	// standalone instance. That avoids Metal-specific failures, but permanently
	// opts users out of the much cheaper renderer even when Metal works. Clear a
	// stored legacy `true` whenever it has no user-selection provenance. This is
	// deliberately repeatable: launching an older alpha can write the legacy
	// value again after a newer build has already migrated the configuration.
	constexpr bool shouldRemoveLegacyStandaloneSoftwareRenderer(
		const bool _isMacOS, const bool _isStandalone,
		const bool _hasSessionRendererOverride,
		const bool _userSelectedRenderer,
		const bool _hasPersistedRendererPreference,
		const bool _persistedSoftwareRenderer)
	{
		return _isMacOS && _isStandalone
			&& !_hasSessionRendererOverride
			&& !_userSelectedRenderer
			&& _hasPersistedRendererPreference
			&& _persistedSoftwareRenderer;
	}
}
