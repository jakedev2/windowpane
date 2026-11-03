#include <Geode/Geode.hpp>

// Geode mods must NOT be built in the Debug configuration: the debug C++ runtime lays out std::string, vectors, ...
// differently from the Geode loader's release runtime, so any string that crosses between the two (e.g. the JSON text
// from matjson) crashes with "Debug Assertion Failed ... xmemory line 209".
//   cmake -B build -A x64
//   cmake --build build --config RelWithDebInfo
#if defined(_MSC_VER) && defined(_DEBUG)
#error "Build Windowpane with: cmake --build build --config RelWithDebInfo   (not Debug)"
#endif

#include "core/Cfg.hpp"
#include "core/Session.hpp"
#include "import/MacroParser.hpp"
#include "sim/Analyzer.hpp"
#include "ui/Ui.hpp"

using namespace geode::prelude;

namespace wp {

// Optional frame-interval override (settings "Apply display FPS"). The previous interval is stored so
// leaving the level puts the game's own setting back.
void applyFrameLimiter(bool inLevel) {
    static double original = 0.0;
    auto* dir = CCDirector::get();
    if (!dir) return;
    if (inLevel && cfg().frameLimiter) {
        if (original <= 0.0) original = dir->getAnimationInterval();
        dir->setAnimationInterval(1.0 / cfg().displayFpsValue());
    } else if (original > 0.0) {
        dir->setAnimationInterval(original);
        original = 0.0;
    }
}

}  // namespace wp

$on_mod(Loaded) {
    wp::registerBuiltInParsers();
    wp::Cfg::get();

    // Settings update live: reload the typed copy; the HUD notices the bumped revision on its next frame.
    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        wp::cfg().reload();
        wp::applyFrameLimiter(wp::Session::get().pl() != nullptr);
    });

    listenForKeybindSettingPresses("key-toggle-sidebar", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        auto* mod = Mod::get();
        mod->setSettingValue<bool>("sidebar-enabled", !mod->getSettingValue<bool>("sidebar-enabled"));
    });

    listenForKeybindSettingPresses("key-analyze", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat || !wp::Session::get().pl()) return;
        auto& an = wp::sim::Analyzer::get();
        if (an.active()) { an.cancel("Cancelled"); return; }
        if (!wp::Session::get().macro()) { wp::ui::openControlPopup(); return; }
        wp::ui::openProgressPopup();
    });

    ButtonSettingPressedEventV3(Mod::get(), "btn-save-json").listen([](std::string_view) { wp::ui::saveJsonNow(); }).leak();
    ButtonSettingPressedEventV3(Mod::get(), "btn-copy-summary").listen([](std::string_view) { wp::ui::copySummary(); }).leak();
    ButtonSettingPressedEventV3(Mod::get(), "btn-open-folder").listen([](std::string_view) {
        auto& S = wp::Session::get();
        std::filesystem::path dir = Mod::get()->getSaveDir() / "results";
        if (auto m = S.macro(); m && !m->path.empty()) dir = std::filesystem::u8path(m->path).parent_path();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        file::openFolder(dir);
    }).leak();
}
