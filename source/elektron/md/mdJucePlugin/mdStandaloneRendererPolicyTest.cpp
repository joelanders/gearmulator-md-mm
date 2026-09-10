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
	using mdJucePlugin::shouldMigrateStandaloneRendererDefaultToAuto;

	expect(shouldMigrateStandaloneRendererDefaultToAuto(true, true, false, false),
		"unmigrated macOS standalone did not select automatic rendering");
	expect(!shouldMigrateStandaloneRendererDefaultToAuto(false, true, false, false),
		"non-macOS standalone attempted the macOS migration");
	expect(!shouldMigrateStandaloneRendererDefaultToAuto(true, false, false, false),
		"plug-in instance attempted the standalone migration");
	expect(!shouldMigrateStandaloneRendererDefaultToAuto(true, true, true, false),
		"session renderer override was not authoritative");
	expect(!shouldMigrateStandaloneRendererDefaultToAuto(true, true, false, true),
		"completed migration ran a second time");

	std::cout << "MD/MM standalone renderer policy: PASS\n";
	return 0;
}
