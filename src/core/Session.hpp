#pragma once
#include <chrono>
// Process-wide state of the mod: which macro is loaded, the latest analysis result, per-input bucket
// assignment, live counters for the sidebar and the live input recorder.

#include "Cfg.hpp"
#include "Types.hpp"

#include <Geode/Geode.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wp {

class Session {
public:
    static Session& get();

    // ---- level binding -------------------------------------------------------------------------
    PlayLayer* pl() const { return m_pl; }
    void attach(PlayLayer* pl);     // PlayLayer finished setup
    void detach();                  // PlayLayer is going away

    // ---- macro / result ------------------------------------------------------------------------
    std::shared_ptr<Macro const> macro() const { return m_macro; }
    std::shared_ptr<AnalysisResult const> result() const { return m_result; }

    // Returns "" on success, otherwise an error to show the user. Warnings go to `warnings`.
    std::string loadMacro(std::filesystem::path const& path, std::vector<std::string>& warnings, MacroType type = MacroType::Auto);
    void setMacro(std::shared_ptr<Macro const> m);
    // Starts analysing the loaded macro in the open level. Returns "" or an error.
    std::string startAnalysis();
    void onAnalysisFinished(std::shared_ptr<AnalysisResult> r);

    // ---- derived data for the UI ----------------------------------------------------------------
    // Rebuilds bucket assignment if the result or the bucket config changed.
    void refresh();
    int bucketOf(size_t input) const { return input < m_bucket.size() ? m_bucket[input] : -1; }
    // number of inputs whose tick is <= `tick` (binary search)
    size_t inputsUpTo(int64_t tick) const;
    std::vector<int> const& countsAt(size_t inputsShown);   // per cfg().buckets index
    std::vector<int> const& totals() const { return m_totals; }
    bool hasResult() const { return m_result && !m_result->inputs.empty(); }
    double sigmaPerSec() const { return m_result ? m_result->sigmaPerSec : 0.0; }

    // ---- live recording -------------------------------------------------------------------------
    // Recording only works in practice mode. When the level respawns from a practice checkpoint the inputs
    // made after that checkpoint are dropped, so the macro is pieced together from the good parts of each attempt.
    bool recording() const { return m_recording; }
    // Returns "" or an error to show (not in practice mode, platformer, ...).
    // FPS option (physics tick rate): cycles 240 -> 320 -> 480 -> 720 -> 960 -> 240.
    // A recording is only allowed when the game really runs at that rate: the physics steps of the running game are counted
    // (noteStep, once per step in normal play) and a recording whose measured rate does not match is cancelled at once.
    int fps() const;
    void cycleFps();
    void noteStep(float timeWarp);
    double measuredTps() const { return m_measTps; }   // steps per real second, 0 = not measured yet
    bool macroFpsMismatch() const;                     // a macro is loaded and was recorded at another FPS than the one set
    // Exact checkpoint respawn while recording (setting "respawn at practice checkpoints"): after a death the level is restarted
    // from the very beginning and the clicks recorded so far are fast-forwarded to the checkpoint's tick with the analyzer's
    // stepper, so the attempt continues from a state that a replay from tick 0 reproduces exactly (the game's own checkpoint
    // load does not restore everything, which made recordings joined from checkpoints die in the hard parts when replayed).
    bool ffActive() const { return m_ffActive; }
    // Takes every practice checkpoint out of the level THE GAME'S WAY (PlayLayer::removeAllCheckpoints), which also removes the
    // checkpoints' level objects from the object sections. Emptying m_checkpointArray directly leaves those objects behind, pointing
    // at checkpoints that no longer exist, and the next visibility update crashed on them.
    static void clearPracticeCheckpoints(PlayLayer* pl);
    int64_t lastCheckpointTick(PlayLayer* pl) const;
    void beginExactRespawn(PlayLayer* pl, int64_t target);   // call after beginLevelReset, before the game's own reset
    bool ffUpdate(PlayLayer* pl);                            // drives the fast-forward; true = the game's own update is swallowed
    std::string startRecording();
    // Ends the recording; returns "" and stores the macro, or an error.
    std::string stopRecording();
    void recordInput(int button, bool down, bool p2, int64_t tick);
    // The tick of the command pass that is running (the value of m_currentProgress when it STARTED). Clicks are
    // stamped with it, which is exactly the tick playback / analysis applies them at, whatever the game does to
    // m_currentProgress during the pass.
    void enterPass(int64_t tick) { m_inPass = true; m_passTick = tick; }
    void leavePass() { m_inPass = false; }
    int64_t stampFor(int64_t liveProgress) const { return m_inPass ? m_passTick : liveProgress; }
    // Called around PlayLayer::resetLevel. Drops the inputs that come after the checkpoint we respawn at
    // (everything, for a full restart) and remembers which buttons were held at that point.
    void beginLevelReset(PlayLayer* pl);
    void endLevelReset(PlayLayer* pl);
    // Called once per game update: closes / opens a hold that carried over a respawn.
    void recordCarryCheck(GJBaseGameLayer* gl);

    // ---- playback: the loaded macro plays itself while you play the level, so the frame windows can be watched ----
    bool playbackOn() const;
    bool togglePlayback();
    // Called before every command pass of the live level (not while analysing).
    void injectPlayback(GJBaseGameLayer* gl);
    void beginUpdatePasses() { m_livePass = 0; }   // called at the start of every game update
    bool botAttempt() const { return m_botAttempt; }
    // Plays frame-N.wav for the input with this macro index (N = its measured window, 9 and above use frame-9).
    void playFrameSound(size_t eventIndex);
    void playFrameSoundN(int n);

    // ---- hand editing of the measured windows, and saving the macro with its analysis -----------------
    std::vector<size_t> editableInputs() const;          // inputs that have a measured window
    bool editWindow(size_t input, double value);         // sets the window; false if the input cannot be edited
    bool resetWindow(size_t input);                      // back to what the analysis measured
    std::string saveMacroFile(std::filesystem::path const& path);   // "" or an error   // plays frame-N.wav (1..9) now; also used as a test
    void clearBotAttempt() { m_botAttempt = false; }

    // ---- recording speed (1x / 0.5x / 0.25x / 0.1x) ----------------------------------------------
    double recordSpeed() const;
    double cycleRecordSpeed();     // 1 -> 0.5 -> 0.25 -> 0.1 -> 1; returns the new value
    void updateRecordSpeed();      // applies / releases the slow-down; call every game update
    void releaseRecordSpeed();     // puts the normal speed back (menus, quitting, analysis)

    // ---- export ---------------------------------------------------------------------------------
    std::string saveJson(std::filesystem::path& outPath);   // "" on success
    std::string summaryText() const;

    std::string lastMessage;   // last status line (shown in the control popup)
    std::string lastReport;    // why nothing could be measured (empty when the analysis produced numbers)

private:
    PlayLayer* m_pl = nullptr;
    std::shared_ptr<Macro const> m_macro;
    std::shared_ptr<AnalysisResult> m_result;
    uint64_t m_cfgRev = ~0ull;
    const AnalysisResult* m_bucketFor = nullptr;
    std::vector<int> m_bucket;
    std::vector<int> m_totals;
    std::vector<int> m_counts;
    size_t m_countsShown = ~size_t(0);
    bool m_recording = false;
    std::vector<InputEvent> m_rec;
    bool m_held[2][4] = {};        // [player 2][button] as the recording sees it
    bool m_tickCalibrated = false;
    // physics step counting (see noteStep)
    std::chrono::steady_clock::time_point m_stepStart{}, m_stepLast{};
    int m_stepCount = 0;
    double m_measTps = 0.0;
    bool m_verifyTps = false;      // a recording just started: check the game's tick rate on the first measurement
    void cancelRecordingForTps(double measured);

    // exact respawn state (see ffActive)
    bool m_ffActive = false;
    int m_ffPhase = 0;                 // 0 = waiting for the restarted level, 1 = fast-forwarding
    int m_ffFrames = 0;
    int64_t m_ffTarget = 0;
    size_t m_ffCursor = 0;
    std::vector<InputEvent> m_ffEvents;
    geode::Ref<cocos2d::CCArray> m_ffSaved;       // the real checkpoint list while the game sees an empty one
    bool m_ffHideAll = false;
    CheckpointObject* m_ffCurrent = nullptr;      // PlayLayer::m_currentCheckpoint as it was before the respawn
    cocos2d::CCArray* m_ffTemp = nullptr;
    geode::Ref<cocos2d::CCLabelBMFont> m_ffLabel;
    bool m_ffHidden = false;
    std::string m_ffShown;
    void ffRestore(PlayLayer* pl);                // gives the checkpoint list back, shows the level, removes the label
    void ffAbort(PlayLayer* pl, std::string const& why);
    bool m_inPass = false;
    int64_t m_passTick = 0;
    bool m_inReset = false;        // inside PlayLayer::resetLevel: button events there are not real clicks
    bool m_carryCheck = false;
    int64_t m_carryTick = 0;
    const AnalysisResult* m_soundFor = nullptr;
    std::vector<float> m_soundWindow;   // per macro input: measured window in frames, 0 = none
    int m_livePass = 0;
    size_t m_pbCursor = 0;
    int64_t m_pbLast = -1;
    int64_t m_pbPassTick = -1;   // tick the pass counter below belongs to
    int m_pbPassCount = 0;       // command passes seen so far on that tick
    bool m_pbResync = true;
    bool m_botAttempt = false;
    bool m_slowActive = false;
    float m_slowSaved = 1.f;
};

}  // namespace wp
