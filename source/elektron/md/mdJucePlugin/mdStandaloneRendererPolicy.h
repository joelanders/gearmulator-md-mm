#pragma once

namespace mdJucePlugin
{
	// Early MD/MM alpha builds persisted software rendering for every macOS
	// standalone instance. That avoids Metal-specific failures, but permanently
	// opts users out of the much cheaper renderer even when Metal works. Clear
	// that old default once and return to the renderer's automatic selection.
	// A software preference chosen after this migration remains authoritative.
	constexpr bool shouldMigrateStandaloneRendererDefaultToAuto(
		const bool _isMacOS, const bool _isStandalone,
		const bool _hasSessionRendererOverride,
		const bool _migrationComplete)
	{
		return _isMacOS && _isStandalone
			&& !_hasSessionRendererOverride
			&& !_migrationComplete;
	}
}
