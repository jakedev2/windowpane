#pragma once
#include <array>
#include "../core/Types.hpp"
#include "Snapshot.hpp"
#include "Stepper.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wp::sim {

struct AnalysisParams {
    int maxShift = 16;
    int maxTrials = 80;
    int horizonInputs = 2;
    int horizonTail = 12;
    int horizonMin = 30;
    int horizonMax = 480;
    int convergeTicks = 8;
    bool selfTest = true;
    bool longSelfTest = true;
    bool localStart = true;      // a snapshot that does not restore bit-exactly is still used: trials start just before the click with the
                                 // player's position / velocity / rotation set from the first pass (instead of replaying from far back)
    bool cbf = false;
    int cbfSubdivisions = 4;
    int speedX = 40;
    bool autoThrottle = true;
    int minFps = 10;
    bool mute = true;
    bool keepHold = true;        // a shifted click drags its release along (same hold length)
    int releaseMode = 0;         // 0 auto (where it matters), 1 always, 2 never
    bool autoTiming = true;      // imported macros: try other frame interpretations when the replay dies early
    bool exact = true;           // replay every trial from the start of the level (no snapshots in the middle)
    bool autoSubtick = false;    // half-tick measurement when the game counts several progress steps per tick
    int rejoinExtra = 480;       // keep checking this many ticks past the horizon while a shifted run has not rejoined the original
    int flyStrict = 1;           // 0-3: extra strictness for clicks in ship / wave / swing (longer survival check)
    bool recheck = true;         // at the very end, measure every click with a window of 1-3 ticks once more, strictly
    bool verify = true;          // re-run the outermost surviving shift of each side to the end, with no early exit
    bool repair = true;          // a macro that dies early is repaired: clicks a few ticks off are moved until it survives
    int repairMaxShift = 12;     // how far (ticks) a click may be moved while repairing
    bool fastSearch = true;      // shifts 1..8 are tried one by one as before; beyond that the search gallops and bisects (far fewer trials for wide windows)
};

struct AnalysisProgress {
    bool active = false;
    std::string phase;           // "Recording the original run", "Measuring input 12 / 340", ...
    double fraction = 0.0;       // 0..1
    double speedX = 0.0;         // simulated ticks per wall second / 240
    double trialsPerSec = 0.0;
    double throttle = 1.0;       // 1 = full requested speed
    int64_t trials = 0;
    int doneInputs = 0;
    int totalInputs = 0;
    double elapsedSec = 0.0;
    double etaSec = 0.0;
    bool cbfActive = false;
};

// Re-simulates a macro in the open PlayLayer to measure every input's timing window.
//
// Pass 1 (baseline):   play the macro once from 0%, recording a per-tick picture of both players and the
//                      position of every input.
// Pass 2 (measure):    play it again. Shortly before each group of inputs take a snapshot, then for each
//                      input restore the snapshot and replay with that input moved by -1, -2, ... and
//                      +1, +2, ... ticks until the run dies (or the shift cap is reached). Trials stop
//                      early once the shifted run has rejoined the original one.
//
// All work happens in small slices called once per frame from the GJBaseGameLayer::update hook, so the
// UI stays responsive and the frame time is bounded.
class Analyzer {
public:
    static Analyzer& get();

    bool active() const { return m_phase != Phase::Idle; }

    // Begins an analysis of `macro` in `pl`. Returns false and fills `error` if it cannot start.
    bool start(PlayLayer* pl, std::shared_ptr<Macro const> macro, AnalysisParams params, std::string& error);

    // Stops at the next slice. Safe to call from UI code at any time.
    void cancel(std::string const& reason = "Cancelled");

    // Called every frame by the update hook while active().
    void runSlice(PlayLayer* pl, double realDt);

    AnalysisProgress progress() const;

    // Invoked once when the analysis ends (finished, cancelled or failed). The result is never null.
    std::function<void(std::shared_ptr<AnalysisResult>)> onFinished;

    // The level was closed under us: drop everything without touching the game.
    void abandon();

private:
    enum class Phase { Idle, Init, Baseline, Group, Reset, Measure, Repair };

    struct Plan {                // per input pre-computation
        int64_t tick = 0;
        int earlier = 0;         // max shift earlier
        int later = 0;           // max shift later
        int64_t snapTick = 0;    // tick - earlier
        int64_t horizonEnd = 0;  // the stretch that must be survived
        int64_t hardEnd = 0;     // the run keeps going until here (>= horizonEnd) while it has not rejoined the original
        bool reachable = true;
        size_t rel = static_cast<size_t>(-1);   // index of this press's release (npos if none)
        std::string note;
    };
    struct Group { size_t first, last; int64_t snapTick; };

    enum class Side { Earlier, Later, Done };
    struct Search {              // search state for the input being measured
        size_t input = 0;
        Side side = Side::Earlier;
        int step = 1;            // integer shift being tried on this side (>= 1)
        int passedEarlier = 0, passedLater = 0;   // furthest integer shift that survived
        double fracEarlier = 0, fracLater = 0;    // sub-tick refinement (CBF)
        bool refining = false;   // CBF: testing sub-tick offsets inside the failing integer tick
        int failAt = 0;          // fast search: smallest shift on this side known to die (0 = none yet)
        int stride = 0;          // fast search: current gallop stride
        int refineK = 1;
        bool cappedEarlier = false, cappedLater = false;
        int trials = 0;
        int verifySide = -1;     // boundary re-check: -1 not started, 0 earlier, 1 later, 2 done
        int verifyTries = 0;
        bool downgraded = false; // the re-check lowered the window
        // A search trial that already survived to hardEnd / level end is its own full-length edge verification.
        bool fullEarlier = false, fullLater = false;
        double fullEarlierDelta = 0.0, fullLaterDelta = 0.0;
        std::string trace;       // "-1o -2o -3x ..."
    };
    struct Trial {
        bool running = false;
        double delta = 0;
        uint32_t horizonEnd = 0;
        int64_t hardEnd = 0;         // see Plan::hardEnd
        bool fullRun = false;        // verification trial: no early exit, always runs to hardEnd
        bool completedFull = false;  // survived to hardEnd / level end rather than exiting through convergence
        bool deepBlocked = false;    // visible state rejoined, hidden state did not (at least once)
        size_t cursor = 0;
        int64_t lastDiffTick = 0;    // after this tick the shifted run differs only through physics
        int64_t firstDiffTick = 0;   // before this tick the shifted run is identical to the original
        int converge = 0;
        int64_t deathTick = -1;      // tick the trial died at (diagnostics: shown in the trace)
        std::vector<InputEvent> events;
    };

    // one unit of work: returns ticks simulated (>= 0), or -1 to end this frame's slice
    int64_t unitInit(PlayLayer* pl);
    int64_t unitBaseline(PlayLayer* pl);
    int64_t unitGroup(PlayLayer* pl);
    int64_t unitMeasure(PlayLayer* pl);
    // resetLevel=false: leave the level where it is (used after an exception, when the game's own state may be half-updated and a
    // level reset on top of it can crash the game).
    void finish(PlayLayer* pl, bool cancelled, std::string const& error, bool resetLevel = true);
    void abortAfterException(PlayLayer* pl, std::string const& what, char const* type);

    // helpers
    // ---- automatic macro timing (imported macros) ----
    bool retryTiming(PlayLayer* pl);
    void applyTiming(size_t idx);
    std::shared_ptr<Macro const> m_origMacro;
    std::vector<std::pair<double, double>> m_timingCands;   // (scale, offset in ticks)
    size_t m_timingIdx = 0;
    bool m_timingFinal = false;
    std::array<int64_t, 12> m_timingReach{};
    size_t m_warnMark = 0;
    bool m_cbfRequested = false;
    bool runControl(PlayLayer* pl, std::string& note);   // unmodified replay from the start vs the first pass
    mutable double m_shownFraction = 0.0;   // last progress fraction reported (see progress())
    bool m_exact = false;
    // Imported (non-Windowpane) macros only: a group of inputs whose mid-level snapshots do not restore exactly is measured by
    // replaying every trial from a genuine reset of the level (like exact mode), instead of restoring the start-of-level
    // checkpoint, which is not the same as a new attempt and made every shifted trial die (every window came out as 1 frame).
    bool m_confirmPending = false, m_confirming = false;   // a death at a 1-2 tick shift is replayed once before it counts
    double m_confirmDelta = 0.0;
    int64_t m_confirmFlips = 0;
    // final strict re-check of the tight clicks (window 1-3), see startRecheck()
    bool m_rc = false, m_rcDone = false, m_rcPrevValid = false;
    std::vector<size_t> m_rcList;
    size_t m_rcPos = 0, m_rcPrev = 0;
    double m_rcOld = 0.0;
    int m_rcChanged = 0;
    bool startRecheck(PlayLayer* pl);
    bool m_advNoCompare = false, m_driftNoted = false;   // drift of a replay from the start: keep measuring, do not compare further
    int m_groupRetry = 0, m_detRetry = 0;
    // A one-off hiccup (a reset that is slow to start, one update that does not advance) must not end an analysis that has been
    // running for many minutes: the trial / reset is simply run again, up to 5 times in a row.
    int m_transientRetries = 0;
    bool m_faultRetry = false;
    bool retryReset(PlayLayer* pl, std::string const& why);
    bool m_groupExact = false;
    int m_stFailStreak = 0;          // consecutive groups whose snapshot self-test failed
    bool m_stSkipNoted = false;
    bool exactNow() const { return m_exact || m_groupExact; }
    bool m_controlDone = false;
    // Control trial (exact mode): the UNMODIFIED macro is replayed through exactly the same reset / step / input path as every
    // shifted trial and must survive exactly as long as the first pass. If it does not, nothing measured would be trustworthy.
    bool m_isControl = false;
    int64_t m_controlLimit = 0;
    std::string m_controlDiff;
    bool prepareControl();
    int64_t stepControl(PlayLayer* pl);
    void endControl(bool ok, std::string const& note);
    // Hidden-state convergence is switched off when two identical replays do not agree on it (the hash would then only
    // ever prevent early exits, or worse, not mean anything).
    bool m_deepUsable = true;
    int64_t m_deepBlocked = 0;       // trials that rejoined visibly but not in the hidden state (ran on to hardEnd)
    // The original run is recorded once before the sub-tick probe decides how clicks are delivered. If the mode turns out
    // different from the one the baseline used (macro with sub-tick clicks), the baseline is recorded again.
    bool m_baseCbf = false, m_baseHalf = false, m_rebaselined = false;
    bool m_freshDone = false;   // Init: the tick-rate calibration restored a checkpoint, so the level was reset once more
    bool nextVerifyDelta(double& delta);

    // ---- macro repair ----
    // A macro that dies long before it ends (typically a recording stitched together from practice-mode attempts, whose
    // clicks can be a few ticks off) is repaired before it is measured: starting at the death, the nearest clicks are
    // moved by a few ticks, each candidate replayed from a genuine reset, until the macro survives. Of the shifts that work
    // the middle one is taken so the click does not end up on the very edge of its window.
    struct Repair {
        std::vector<InputEvent> events;
        int64_t reach = 0;                 // tick at which the working macro dies
        bool ended = false;
        std::vector<size_t> cand;          // candidate inputs (nearest to the death first)
        size_t ci = 0;
        int si = 0;                        // index into the shift order -1, +1, -2, +2, ...
        int bestCand = -1, bestShift = 0;
        int64_t bestReach = 0;
        bool bestEnded = false;
        bool refining = false;
        int refDir = 0, refNext = 0;
        size_t refCand = 0;
        std::vector<std::pair<int, std::pair<int64_t, bool>>> passed;   // refining: shift -> (reach, ended)
        int rounds = 0, trials = 0, applied = 0;
        bool resetting = false;
        int resetFrames = 0;
        bool running = false;
        std::vector<InputEvent> trialEv;
        size_t cursor = 0;
        size_t curCandIdx = 0;
        int curShift = 0;
        std::vector<std::string> notes;
    };
    Repair m_rp;
    bool m_repairDone = false;
    bool m_autoExact = false;   // save states drifted: this analysis was restarted in exact-replay mode
    bool startRepair(PlayLayer* pl);
    void repairBuildCandidates();
    bool repairMakeTrial(size_t idx, int shift, std::vector<InputEvent>& out) const;
    int64_t unitRepair(PlayLayer* pl);
    void repairTrialDone(int64_t reachTick, bool ended);
    void repairApply(size_t candIdx, int shift, int64_t reach, bool ended);
    void repairNextRound();
    void finishRepair(PlayLayer* pl);
    bool m_warnedControl = false;
    bool calibrate(PlayLayer* pl);   // finds the dt / hook setting that gives exactly one step per update
    void buildPlans();
    void buildGroups();
    void beginInput();
    bool nextDelta(double& delta);
    void onTrialResult(bool survived);
    void endSide();
    bool beginTrial(PlayLayer* pl, double delta);
    bool prepareTrial(PlayLayer* pl, double delta);   // builds the shifted event list; the level is at its start / snapshot
    int64_t unitReset(PlayLayer* pl);                 // exact mode: waits for a fresh reset of the level to start
    int m_resetFrames = 0;
    double m_pendingDelta = 0.0;
    int64_t stepTrial(PlayLayer* pl);
    void finishInput(InputStatus forced, std::string const& note);
    bool runSelfTest(PlayLayer* pl, std::string& note, int ticks);
    void startBroken(std::string const& why, double resumeDelta);                  // snapshot start rejected: this group is measured from the level start
    bool forcePlayerState(PlayLayer* pl, uint32_t tick);                          // sets x / y / yVelocity / rotation of both players from m_rec[tick]
    bool runLocalCheck(PlayLayer* pl, std::string& note, int ticks);               // restore + force, then the unmodified macro must stay on the first pass
    void runCbfProbe(PlayLayer* pl, size_t pressIndex);
    void fail(std::string const& msg);
    void markUnreliable(size_t first, size_t last, std::string const& note);
    size_t firstEventAtOrAfter(int64_t tick) const;
    void queueDue(Stepper& st, std::vector<InputEvent> const& ev, size_t& cursor, int64_t tick);

    Phase m_phase = Phase::Idle;
    std::shared_ptr<Macro const> m_macro;
    std::shared_ptr<AnalysisResult> m_res;
    AnalysisParams m_p;
    PlayLayer* m_pl = nullptr;   // not owned; cleared by abandon() / finish()
    std::string m_phaseText;
    std::string m_failText;
    bool m_cancelRequested = false;
    std::string m_cancelReason;
    bool m_cbfActive = false;
    bool m_cbfProbed = false;
    bool m_failed = false;

    // Init
    int m_initFrames = 0;
    bool m_hid = false;
    bool m_wasVisible = true;
    bool m_audioSaved = false;
    float m_savedMusic = 1.f, m_savedSfx = 1.f;

    // Baseline
    Snapshot m_s0;
    std::vector<TickState> m_rec;      // m_rec[t] = state when the level is at tick t (before step t)
    size_t m_baseCursor = 0;
    bool m_baseDied = false;
    bool m_baseEnded = false;
    uint32_t m_baseEndTick = 0;
    int64_t m_baseTickLimit = 0;

    // Measure
    std::vector<Plan> m_plans;
    std::vector<Group> m_groups;
    size_t m_gi = 0;                   // current group
    size_t m_ii = 0;                   // current input (index into macro inputs)
    int m_lastGoodRestarts = 0;
    Snapshot m_lastGood;               // beta.26: the last group snapshot that passed its self-test (restart point for failing groups)
    Snapshot m_snap;                   // snapshot of the current group (also "where the baseline is" between groups)
    // beta.28 "local start": m_snap did not pass the exact self-test, but restoring it and putting both players back to the position /
    // velocity / rotation of the first pass (m_rec[m_snap.tick]) replays the unmodified macro well enough (see runLocalCheck). Every
    // restore of such a snapshot is followed by forcePlayerState(), so a trial starts right before the click it measures instead of
    // replaying from the last exact snapshot or from the start of the level.
    bool m_snapLocal = false;
    int m_localStreak = 0;             // consecutive groups whose exact self-test failed (the exact test is then skipped for a while)
    int m_localGroups = 0, m_localInputs = 0;
    bool m_forcePending = false;       // force the player state right before the first step of the trial (see stepTrial)
    // 2.5.1 start validation: a snapshot start that cannot reproduce the first pass must never be reported as "1 frame".
    bool m_zeroProbe = false;          // the running trial is the unshifted probe of a group
    double m_probeDelta = 0.0;         // the shifted trial that triggered the probe (re-run after it passes)
    bool m_groupZeroOk = false;        // this group's start survived an unshifted trial (or the group is measured from the level start)
    int m_badStarts = 0;               // groups whose snapshot start was rejected and measured by replaying from the start of the level
    bool m_localOff = false;           // local start switched off for the rest of the analysis after repeated bad starts
    bool m_haveSnap = false;
    bool m_groupReliable = true;
    std::string m_groupNote;
    bool m_advRestored = false;
    bool m_advFromStart = false;
    bool m_advResetting = false;       // a genuine level reset for the start of a group's advance is in progress
    int m_advResetFrames = 0;
    size_t m_advCursor = 0;
    int m_advMismatches = 0;

    Search m_s;
    Trial m_t;
    bool m_inputActive = false;

    // stats
    std::chrono::steady_clock::time_point m_t0;
    int64_t m_ticksTotal = 0;
    int64_t m_trialsTotal = 0;
    double m_speedX = 0, m_trialsPerSec = 0, m_throttle = 1.0;
    double m_frameEma = 0.0;
    double m_lastSliceSec = 0.0;   // wall time the previous analysis slice itself took (excluded from the throttle's frame-overhead check)
    int m_doneInputs = 0;
    int m_toMeasure = 0;
};

}  // namespace wp::sim
