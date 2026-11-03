#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wp::ui {

// Native file dialogs (Windows only; elsewhere the callback gets nullopt).
//
// The dialog runs on its own worker thread and the result is handed back on the game's main thread, so:
//  - the button click that opened it returns immediately (a blocking dialog inside the click handler made the
//    game re-fire the click and open the dialog again and again),
//  - only one dialog can be open at a time, and a click right after one closed is ignored.
// Returns false (and does not call `done`) when a dialog is already open / was closed a moment ago.
// `done` receives nullopt when the user cancelled.
// It lives in its own small source file so the rest of the UI does not have to pull in the async/coroutine
// machinery of Geode's file picker on Windows (the MSVC compiler crashed on that translation unit).
using PickDone = std::function<void(std::optional<std::filesystem::path>)>;

bool nativePickFileAsync(std::string const& description, std::vector<std::string> const& patterns, PickDone done);

// `extension` has no dot, e.g. "wpmacro".
bool nativeSaveFileAsync(std::string const& defaultName, std::string const& extension, std::string const& description, PickDone done);

}  // namespace wp::ui
