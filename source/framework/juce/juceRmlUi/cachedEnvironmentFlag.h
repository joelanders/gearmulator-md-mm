#pragma once

#include <cstdlib>

namespace juceRmlUi
{
	class CachedEnvironmentFlag
	{
	public:
		using Lookup = const char* (*)(const char*);

		explicit CachedEnvironmentFlag(const char* _name)
			: CachedEnvironmentFlag(_name, lookupEnvironment)
		{
		}

		CachedEnvironmentFlag(const char* _name, const Lookup _lookup)
			: m_enabled(parse(_lookup(_name)))
		{
		}

		bool isEnabled() const noexcept { return m_enabled; }

	private:
		static const char* lookupEnvironment(const char* const _name)
		{
			return std::getenv(_name);
		}

		static bool parse(const char* const _value)
		{
			return _value && std::atoi(_value) != 0;
		}

		const bool m_enabled;
	};
}
