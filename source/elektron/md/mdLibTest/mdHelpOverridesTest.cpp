// Tooltip text overrides: every built-in string has one unique key, the generated defaults file
// reads back to exactly the built-in text, and an override replaces one string and nothing else.
//
//   mdHelpOverridesTest            run the checks
//   mdHelpOverridesTest --dump     print the defaults file to stdout

#include "../mdJucePlugin/mdHelpOverrides.h"
#include "../mdJucePlugin/mdMonomachineHelp.h"
#include "../mdJucePlugin/mdParameterHelp.h"

#include "baseLib/configFile.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace
{
	int g_failures = 0;

	void check(const bool _ok, const std::string& _what)
	{
		if(_ok)
			return;
		std::cerr << "FAIL: " << _what << '\n';
		++g_failures;
	}

	void writeFile(const std::filesystem::path& _path, const std::string& _text)
	{
		std::ofstream out(_path, std::ios::binary | std::ios::trunc);
		out << _text;
	}
}

int main(const int _argc, char** _argv)
{
	using mdJucePlugin::HelpOverrides;

	if(_argc > 1 && std::strcmp(_argv[1], "--dump") == 0)
	{
		std::cout << HelpOverrides::defaultsText();
		return 0;
	}

	// Keys are unique, every string is non-empty and fits on one line of the file format.
	std::set<std::string> keys;
	std::set<const void*> fields;
	size_t count = 0;
	HelpOverrides::forEachText([&](const std::string& _key, const char* const& _field)
	{
		++count;
		check(keys.insert(_key).second, "duplicate key " + _key);
		check(fields.insert(&_field).second, "string listed twice under " + _key);
		check(_field && *_field, "empty text for " + _key);
		if(!_field)
			return;
		const std::string text(_field);
		check(text.find('\n') == std::string::npos && text.find('\r') == std::string::npos, "line break in " + _key);
		check(text.front() != ' ' && text.back() != ' ', "leading or trailing space in " + _key + " (the file format trims it)");
		check(_key.find('=') == std::string::npos && _key.front() != '#' && _key.front() != ';', "unusable key " + _key);
	});
	check(count > 500, "expected several hundred strings, got " + std::to_string(count));

	// The defaults file reads back to exactly the built-in text.
	const auto dir = std::filesystem::temp_directory_path() / "mdHelpOverridesTest";
	std::filesystem::create_directories(dir);
	const auto defaults = dir / "tooltips-defaults.txt";
	check(HelpOverrides::writeDefaults(defaults), "writing the defaults file");
	{
		const baseLib::ConfigFile file(defaults.string());
		check(file.getArgsWithValues().size() == count, "defaults file holds every key");
		HelpOverrides::forEachText([&](const std::string& _key, const char* const& _field)
		{
			check(file.get(_key) == _field, "defaults file round trip for " + _key);
		});
	}

	// No file: every string is its default.
	const auto overridePath = dir / "tooltips.txt";
	std::filesystem::remove(overridePath);
	HelpOverrides help;
	help.setFile(overridePath);
	const auto& atk = mdJucePlugin::parameterHelp::g_monomachineAmplification[0];
	check(help(atk.description) == atk.description, "missing file leaves defaults");

	// One override replaces that string only. text::tune is shared by several rows; overriding one
	// of those rows must not change the others.
	const mdJucePlugin::monomachineHelp::Parameter* sineTune = nullptr;
	const mdJucePlugin::monomachineHelp::Parameter* noiseTune = nullptr;
	for(const auto& p : mdJucePlugin::monomachineHelp::g_parameters)
	{
		if(std::strcmp(p.label, "TUNE") != 0)
			continue;
		if(p.machine == 1)
			sineTune = &p;
		else if(p.machine == 2)
			noiseTune = &p;
	}
	check(sineTune && noiseTune && sineTune->entry.description == noiseTune->entry.description,
		"GND-SIN and GND-NOIS share the TUNE text");
	writeFile(overridePath,
		"# a comment\n"
		"mm.amp.ATK.description = Custom attack text, with = signs = kept.\n"
		"mm.machine.GND-SIN.TUNE.description = Sine tuning.\n"
		"mm.amp.NOPE.description = typo\n");
	help.load();
	check(std::string(help(atk.description)) == "Custom attack text, with = signs = kept.", "override applied");
	check(help(atk.name) == atk.name, "other field of the same row unchanged");
	check(sineTune && std::string(help(sineTune->entry.description)) == "Sine tuning.", "shared text overridden on one row");
	check(noiseTune && help(noiseTune->entry.description) == noiseTune->entry.description, "other rows sharing that text unchanged");
	check(help.unknownKeys().size() == 1 && help.unknownKeys()[0] == "mm.amp.NOPE.description", "unknown key reported");
	check(help.size() == 2, "two overrides held");

	// Deleting the file restores the defaults.
	std::filesystem::remove(overridePath);
	help.load();
	check(help(atk.description) == atk.description && help.size() == 0, "deleted file restores defaults");

	std::filesystem::remove_all(dir);
	if(g_failures)
	{
		std::cerr << g_failures << " failure(s)\n";
		return 1;
	}
	std::cout << "mdHelpOverridesTest: " << count << " strings, all checks passed\n";
	return 0;
}
