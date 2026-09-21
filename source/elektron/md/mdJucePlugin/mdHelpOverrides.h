#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mdJucePlugin
{
	// Optional replacements for the hover tooltip text, for tweaking wording without rebuilding.
	//
	// The text built into mdParameterHelp.h, mdMonomachineHelp.h and mdMachinedrumHelp.h is the
	// default. A plain-text file of "key = text" lines replaces individual strings: keys it does not
	// mention keep their built-in text, and without the file nothing changes. The file is re-read
	// when it changes, so an edit shows up the next time a tooltip updates.
	//
	// writeDefaults() writes every key with its built-in text. That file is the list of what can
	// be overridden: copy lines from it into the override file and edit them there.
	class HelpOverrides
	{
	public:
		using TextFunc = std::function<void(const std::string& _key, const char* const& _field)>;

		// Calls _func for every overridable string. _field refers to the table member itself, so
		// its address identifies the string even where several rows share the same text.
		static void forEachText(const TextFunc& _func);

		// Every key with its built-in text, in the override file's format.
		static std::string defaultsText();

		// Writes defaultsText() to _path unless the file already holds exactly that.
		static bool writeDefaults(const std::string& _path);

		void setFile(std::string _path);

		// Re-reads the file when its contents changed, checking at most twice a second. Returns true when the
		// texts changed. std::filesystem would be simpler, but it needs macOS 10.15 and this targets 10.13.
		bool refresh();

		// Reads the file now. A missing file leaves no overrides.
		void load();

	private:
		void apply(const std::string& _text);

	public:

		// The replacement for a table string, or the string itself. Pass the table member, e.g.
		// help(entry->description), not a copy of the pointer.
		const char* operator()(const char* const& _field) const;
		// A temporary pointer has no stable address to look up, so it could never be overridden.
		const char* operator()(const char* const&& _field) const = delete;

		// Keys in the file that name no tooltip string, typically typos. Refreshed on every load.
		const std::vector<std::string>& unknownKeys() const { return m_unknownKeys; }

		size_t size() const { return m_texts.size(); }

	private:
		std::string m_path;
		std::unordered_map<const void*, std::string> m_texts;
		std::vector<std::string> m_unknownKeys;
		std::string m_loadedText;	// the file as last read, to notice an edit
		std::chrono::steady_clock::time_point m_lastCheck{};
	};
}
