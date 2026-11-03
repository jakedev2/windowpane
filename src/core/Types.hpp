#pragma once
// Plain data types shared by every part of Windowpane. Nothing in here touches the game.

#include <cstdint>
#include <memory>
#include <limits>
#include <string>
#include <vector>

namespace wp {

// The physics tick rate everything is measured at (the FPS option on the main page: 240 / 320 / 480 / 720 / 960). A tick is
// 1/tps() seconds, frame windows are counted in those ticks, and a macro is only analysed at the rate it was recorded at.
inline double& tpsRef() { static double v = 240.0; return v; }
inline double tps() { return tpsRef(); }
inline bool validTps(int v) { return v == 240 || v == 320 || v == 480 || v == 720 || v == 960; }
inline int nextTps(int v) {
    switch (v) { case 240: return 320; case 320: return 480; case 480: return 720; case 720: return 960; default: return 240; }
}

// Some setups advance GJGameState::m_currentProgress by more than 1 for every 1/240 s of game time (two counts per
// physics step, ...). A "tick" in Windowpane is always 1/240 s, so ticks = progress / progressPerTick. The factor is
// measured by the calibration (Stepper.cpp), saved, and is 1 on a plain game.
inline uint32_t& progressPerTickRef() { static uint32_t v = 1; return v; }
inline uint32_t progressPerTick() { return progressPerTickRef() < 1 ? 1u : progressPerTickRef(); }
inline uint32_t ticksOf(uint32_t progress) { return progress / progressPerTick(); }

enum class Button : uint8_t { Jump = 1, Left = 2, Right = 3 };

// Which bot a macro file comes from, as chosen on the import screen. Auto = work it out from the file.
enum class MacroType : uint8_t { Auto, XdBot, ZBot, Eclipse, Windowpane, MegaHack };

// One press or release. `tick` follows the GDR / bot convention: it is the value of
// GJGameState::m_currentProgress at the moment the input is applied, i.e. the input takes effect
// in the physics step that advances the level from `tick` to `tick + 1`.
struct InputEvent {
    int64_t tick = 0;
    double sub = 0.0;            // sub-tick offset in [0,1) for click-between-frames macros
    Button button = Button::Jump;
    bool p2 = false;
    bool down = false;           // true = press, false = release
    int64_t raw = 0;             // the frame number exactly as it was in the file (before any conversion)
};

struct Macro {
    std::string name;            // file name without extension
    std::string path;            // full path ("" for a live recording)
    std::string format;          // "GDR2", "GDR1 (msgpack)", "GDR1 (json)", "recording"
    std::string botName;
    double sourceTps = 240.0;   // the tick rate the macro was recorded at; its ticks are in 1/sourceTps s
    bool platformer = false;
    bool hasSubTick = false;
    bool imported = false;       // read from another bot's file (GDR): its frame numbering may need correcting
    MacroType type = MacroType::Auto;   // what the user said the file is (import screen)
    double timingScale = 1.0;    // applied correction: tick = raw * timingScale + timingOffset
    double timingOffset = 0.0;
    uint32_t levelId = 0;
    std::string levelName;
    int64_t lastTick = 0;
    std::vector<InputEvent> inputs;   // sorted by tick; stable for equal ticks
    std::vector<uint64_t> deaths;

    bool empty() const { return inputs.empty(); }
};

enum class InputStatus : uint8_t {
    Pending,       // not measured yet
    Measured,      // both sides found a failing shift inside the search range
    Capped,        // at least one side never failed inside the max shift (window >= reported)
    BaselineFail,  // the unshifted input already dies: macro does not reproduce here
    Unreliable,    // snapshot self-test failed, window not trusted
    Aborted,       // trial budget / loop guard tripped
    NotApplicable  // a release where the release timing does not matter (cube, ball, ufo, spider)
};

struct InputResult {
    int index = 0;                 // index into Macro::inputs
    InputStatus status = InputStatus::Pending;
    double window = 0.0;           // number of timing positions, >= 1 (fractional in CBF mode)
    double earlier = 0.0;          // how far it can move earlier
    double later = 0.0;            // how far it can move later
    int trials = 0;
    float x = 0.f, y = 0.f;        // position of the acting player at the input
    float percent = 0.f;
    float speed = 0.f;             // speed portal state at the input (player speed multiplier)
    std::string mode;              // game mode of the player at the input (cube, ship, ball, ufo, wave, robot, spider, swing), for diagnosing the windows per mode
    double timeSec = 0.0;
    std::string note;              // diagnostic text for Unreliable / BaselineFail
    std::string trace;             // every shift that was tried: "-1o -2o -3x +1o +2x" (o = survived, x = died)
    bool edited = false;           // the window was changed by hand (Edit windows)
    double analyzedWindow = 0.0;   // what the analysis measured, kept so an edit can be reset
};

struct AnalysisResult {
    std::string macroName;
    std::string macroPath;
    uint32_t levelId = 0;
    std::string levelName;
    bool cbf = false;
    bool cbfSupported = false;
    int maxShift = 0;
    std::vector<InputResult> inputs;
    int64_t totalTrials = 0;
    int64_t totalTicks = 0;
    double elapsedSec = 0.0;
    double sigmaPerSec = 0.0;      // see core/Sigma.hpp
    double sigmaHours = 24.0;
    bool complete = false;
    bool cancelled = false;
    std::string error;
    std::vector<std::string> warnings;
    std::shared_ptr<struct Macro const> scaledMacro;   // set when the macro's timing had to be corrected or it was repaired
    int repairedInputs = 0;        // clicks moved by the automatic repair
    double timingScale = 1.0;
    double timingOffset = 0.0;
};

// A sidebar row. `lo..hi` is inclusive; hi == kOpen means "lo and up".
constexpr int kOpen = std::numeric_limits<int>::max();

struct Rgb {
    uint8_t r = 255, g = 255, b = 255;
};

struct Bucket {
    int lo = 1, hi = 1;
    std::string label;
    Rgb labelColor, countColor, markerColor;
    bool extended = false;
    bool contains(double window) const {
        // fractional windows (CBF) are floored into their bucket: 3.75 counts as "3"
        const int w = static_cast<int>(window + 1e-9);
        return w >= lo && w <= hi;
    }
};

}  // namespace wp
