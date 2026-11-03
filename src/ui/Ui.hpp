#pragma once
#include <Geode/Geode.hpp>
#include <functional>
#include "../core/Types.hpp"

namespace wp::ui {

// Called by the hooks.
void attach(PlayLayer* pl);        // PlayLayer finished loading: create HUD + marker layer
void frame(PlayLayer* pl);         // once per rendered game frame (not during analysis)
void detach(PlayLayer* pl);        // level closing
bool shouldShow(PlayLayer* pl);    // current mode/visibility gate shared by HUD and markers

// Pops
void openControlPopup();
void openProgressPopup();
void openEditWindowsPopup();   // Popups.cpp
void pickMacro(std::function<void()> done = {});   // import screen (which bot?) -> file dialog -> load -> toast; `done` runs afterwards
void pickMacroOfType(MacroType type, std::function<void()> done = {});   // the file dialog for an already chosen type
void copySummary();
void saveJsonNow();
cocos2d::CCNode* makePauseButtonNode();   // the pause-menu button in the Windowpane 2.0 style

// Individual pieces (implemented in Hud.cpp / Markers.cpp)
void hudAttach(PlayLayer* pl);
void hudFrame(PlayLayer* pl);
void hudDetach();
void markersAttach(PlayLayer* pl);
void markersFrame(PlayLayer* pl);
void markersDetach();

}  // namespace wp::ui
