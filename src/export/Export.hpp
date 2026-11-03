#pragma once
#include "../core/Types.hpp"

#include <filesystem>
#include <string>

namespace wp {

// Writes <macro>.windows.json next to the macro (or into the mod's save directory for live recordings).
// Returns "" on success, otherwise an error message. `outPath` receives the file written.
std::string exportJson(Macro const& macro, AnalysisResult const& result, std::filesystem::path& outPath);
std::string exportJsonText(Macro const& macro, AnalysisResult const& result);
// The "Save macro" file (.wpmacro): the macro, and its analysis when there is one. Loading it restores both.
std::string exportWpMacroText(Macro const& macro, AnalysisResult const* result);
// Short plain-text summary (bucket counts, totals, sigma) for the clipboard.
std::string exportSummary(Macro const& macro, AnalysisResult const& result);

}  // namespace wp
