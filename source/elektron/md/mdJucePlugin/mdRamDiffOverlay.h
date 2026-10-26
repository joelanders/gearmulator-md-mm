#pragma once

#include "mdLib/mdramdiff.h"

#include <memory>
#include <string>

namespace mdJucePlugin
{
	class Editor;

	class RamDiffOverlay
	{
	public:
		explicit RamDiffOverlay(Editor& _editor);
		~RamDiffOverlay();

		RamDiffOverlay(RamDiffOverlay&&) = delete;
		RamDiffOverlay(const RamDiffOverlay&) = delete;
		RamDiffOverlay& operator=(RamDiffOverlay&&) = delete;
		RamDiffOverlay& operator=(const RamDiffOverlay&) = delete;

		void noteAction(const std::string& _label);

	private:
		class Window;
		Editor& m_editor;
		std::unique_ptr<Window> m_window;
	};
}
