#include "mdStandaloneRendererPolicy.h"

#include <cstdlib>
#include <iostream>

namespace
{
	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "mdStandaloneRendererPolicyTest: " << _message << '\n';
		std::exit(1);
	}
}

int main()
{
	using mdJucePlugin::shouldRemoveLegacyStandaloneSoftwareRenderer;

	expect(shouldRemoveLegacyStandaloneSoftwareRenderer(true, true, false, false, true, true),
		"legacy macOS standalone software default was not removed");
	expect(!shouldRemoveLegacyStandaloneSoftwareRenderer(true, true, false, false, false, false),
		"missing renderer preference was treated as a legacy software default");
	expect(!shouldRemoveLegacyStandaloneSoftwareRenderer(true, true, false, false, true, false),
		"persisted automatic renderer preference was removed");
	expect(!shouldRemoveLegacyStandaloneSoftwareRenderer(true, true, false, true, true, true),
		"explicit user software preference was removed");
	expect(!shouldRemoveLegacyStandaloneSoftwareRenderer(false, true, false, false, true, true),
		"non-macOS standalone attempted the macOS migration");
	expect(!shouldRemoveLegacyStandaloneSoftwareRenderer(true, false, false, false, true, true),
		"plug-in instance attempted the standalone migration");
	expect(!shouldRemoveLegacyStandaloneSoftwareRenderer(true, true, true, false, true, true),
		"session renderer override was not authoritative");

	std::cout << "MD/MM standalone renderer policy: PASS\n";
	return 0;
}
