#include "Analyzer.hpp"
#include "../import/MacroParser.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <iterator>
#include <map>
#include <new>
#include <typeinfo>

using namespace geode::prelude;

namespace wp::sim {

namespace {
constexpr int kGroupSpan = 24;          // ticks of extra replay tolerated to share one snapshot
constexpr int kSelfTestTicks = 16;
constexpr int kLinearHead = 8;          // fast search: shifts up to this are tried one by one, only larger ones gallop / bisect
constexpr double kDriftTol = 1e-3;      // position / velocity tolerance for replay verification
constexpr double kConvTol = 1e-4;       // tolerance for "the shifted run is identical to the original again"
constexpr double kLocalTol = 0.1;       // local start: how far the replay from a forced snapshot may drift from the first pass (position / velocity)
constexpr int64_t kBaseTail = 240;      // keep recording this long after the last input

using Clock = std::chrono::steady_clock;

// Why a snapshot / local start / group start was rejected, kept in the results file (first 25 only) so an export says where and why the
// restore stops matching the first pass, without needing the Geode log.
void diagNote(std::shared_ptr<AnalysisResult> const& res, std::string text) {
    if (!res) return;
    int n = 0;
    for (auto const& w : res->warnings) if (w.rfind("Diag: ", 0) == 0) ++n;
    if (n < 25) res->warnings.push_back("Diag: " + std::move(text));
}

double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

struct EventLess {
    bool operator()(InputEvent const& a, InputEvent const& b) const {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.sub < b.sub;
    }
};
}  // namespace

Analyzer& Analyzer::get() {
    static Analyzer a;
    return a;
}

// --------------------------------------------------------------------------------------------------
// lifecycle
// --------------------------------------------------------------------------------------------------

bool Analyzer::start(PlayLayer* pl, std::shared_ptr<Macro const> macro, AnalysisParams p, std::string& error) {
    if (active()) { error = "An analysis is already running."; return false; }
    if (!pl || !pl->m_player1 || !pl->m_objectLayer) { error = "Open a level first."; return false; }
    if (!macro || macro->inputs.empty()) { error = "There is no macro loaded."; return false; }

    p.maxShift = std::clamp(p.maxShift, 1, 64);
    p.maxTrials = std::clamp(p.maxTrials, 2, 1000);
    p.horizonInputs = std::clamp(p.horizonInputs, 1, 16);
    p.horizonTail = std::clamp(p.horizonTail, 0, 1000);
    p.horizonMin = std::clamp(p.horizonMin, 2, 2000);
    p.horizonMax = std::clamp(p.horizonMax, p.horizonMin, 10000);
    p.convergeTicks = std::clamp(p.convergeTicks, 2, 240);
    p.cbfSubdivisions = std::clamp(p.cbfSubdivisions, 2, 32);
    p.speedX = std::clamp(p.speedX, 1, 2000);
    p.minFps = std::clamp(p.minFps, 5, 60);
    p.rejoinExtra = std::clamp(p.rejoinExtra, 0, 4800);
    p.repairMaxShift = std::clamp(p.repairMaxShift, 1, 60);

    m_pl = pl;
    m_macro = std::move(macro);
    m_origMacro = m_macro;
    m_p = p;
    m_cbfRequested = p.cbf;
    m_timingCands.clear();
    m_timingIdx = 0;
    m_timingFinal = false;
    m_timingReach.fill(0);
    if (p.autoTiming && m_macro->imported && m_macro->timingScale == 1.0 && m_macro->timingOffset == 0.0) {
        // Other bots count frames differently: maybe per progress step (half a tick here), maybe off by one.
        const double s2 = progressPerTick() > 1 ? 1.0 / progressPerTick() : 0.5;
        // Bots count frames differently (counted per progress step, or off by one or two ticks), so the readings are tried
        // in turn and the one that survives longest is kept. Which one is tried first depends on what the user said the
        // file is: macros from a game that counts two progress steps per tick (the usual Eclipse case) start with the
        // half-tick readings, everything else starts with the plain reading and small whole-tick offsets.
        if (m_macro->type == MacroType::Eclipse) {
            m_timingCands = {{1.0, 0.0}, {s2, 0.0}, {s2, 0.5}, {s2, -0.5}, {1.0, 1.0}, {1.0, -1.0}, {1.0, 2.0}, {1.0, -2.0}};
        } else {
            m_timingCands = {{1.0, 0.0}, {1.0, 1.0}, {1.0, -1.0}, {s2, 0.0}, {1.0, 2.0}, {1.0, -2.0}, {s2, 0.5}, {s2, -0.5}};
        }
    }
    m_res = std::make_shared<AnalysisResult>();
    m_res->macroName = m_macro->name;
    m_res->macroPath = m_macro->path;
    m_res->levelId = m_macro->levelId;
    m_res->levelName = m_macro->levelName;
    m_res->cbf = p.cbf;
    m_res->maxShift = p.maxShift;
    m_res->inputs.assign(m_macro->inputs.size(), InputResult{});
    for (size_t i = 0; i < m_res->inputs.size(); ++i) m_res->inputs[i].index = static_cast<int>(i);

    SimState::get().dtScale = 1.f;
    SimState::get().forceDelta = true;
    SimState::get().halfPassMode = false;
    SimState::get().secondPass.clear();
    m_warnedControl = false;
    m_isControl = false;
    m_controlDone = false;
    m_controlDiff.clear();
    m_deepUsable = true;
    m_deepBlocked = 0;
    m_rebaselined = false;
    m_baseCbf = m_baseHalf = false;
    m_freshDone = false;
    m_repairDone = false;
    m_autoExact = false;
    m_groupExact = false;
    m_advNoCompare = m_driftNoted = false;
    m_groupRetry = m_detRetry = 0;
    m_rc = m_rcDone = m_rcPrevValid = false;
    m_transientRetries = 0;
    m_faultRetry = false;
    m_rcList.clear();
    m_confirmPending = m_confirming = false;
    m_confirmFlips = 0;
    m_stFailStreak = 0;
    m_stSkipNoted = false;
    m_advResetting = false;
    m_advRestored = false;
    m_advFromStart = false;
    m_rp = Repair{};
    m_phase = Phase::Init;
    m_shownFraction = 0.0;
    m_phaseText = "Starting";
    m_cancelRequested = false;
    m_cancelReason.clear();
    m_failed = false;
    m_failText.clear();
    m_cbfActive = false;
    m_cbfProbed = false;
    m_initFrames = 0;
    m_hid = false;
    m_audioSaved = false;
    m_s0 = {};
    m_snap = {};
    m_lastGood = {};
    m_snapLocal = false;
    m_localStreak = m_localGroups = m_localInputs = 0;
    m_forcePending = m_zeroProbe = m_groupZeroOk = m_localOff = false;
    m_badStarts = 0;
    m_haveSnap = false;
    m_rec.clear();
    m_baseCursor = 0;
    m_baseDied = m_baseEnded = false;
    m_baseEndTick = 0;
    m_plans.clear();
    m_groups.clear();
    m_gi = m_ii = 0;
    m_inputActive = false;
    m_t = Trial{};
    m_s = Search{};
    m_ticksTotal = m_trialsTotal = 0;
    m_speedX = m_trialsPerSec = 0;
    m_throttle = 1.0;
    m_frameEma = 0;
    m_lastSliceSec = 0;
    m_doneInputs = 0;
    m_toMeasure = 0;
    m_t0 = Clock::now();

    auto& S = SimState::get();
    S.clearOutcome();
    S.trialActive = true;
    S.muteAudio = p.mute;
    S.stepping = false;
    return true;
}

void Analyzer::cancel(std::string const& reason) {
    if (!active()) return;
    m_cancelRequested = true;
    if (m_cancelReason.empty()) m_cancelReason = reason;
}

void Analyzer::abandon() {
    auto& S = SimState::get();
    S.trialActive = false;
    S.muteAudio = false;
    S.stepping = false;
    if (!active()) return;
    // The PlayLayer is being torn down, so its nodes must not be touched. Only restore global audio.
    if (m_p.mute && m_audioSaved) {
        m_audioSaved = false;
        if (auto* fmod = FMODAudioEngine::sharedEngine()) {
            fmod->setBackgroundMusicVolume(m_savedMusic);
            fmod->setEffectsVolume(m_savedSfx);
        }
    }
    m_phase = Phase::Idle;
    m_pl = nullptr;
    m_s0 = {};
    m_snap = {};
    m_lastGood = {};
    m_rec.clear();
    if (m_res) {
        m_res->cancelled = true;
        m_res->error = "The level was closed.";
        m_res->elapsedSec = secondsSince(m_t0);
        auto cb = onFinished;
        auto res = m_res;
        if (cb) cb(res);
    }
}

void Analyzer::fail(std::string const& msg) {
    if (m_failed) return;
    m_failed = true;
    m_failText = msg;
    m_cancelRequested = true;
    log::error("[Windowpane] analysis failed: {}", msg);
}

void Analyzer::finish(PlayLayer* pl, bool cancelled, std::string const& error, bool resetLevel) {
    auto& S = SimState::get();
    S.trialActive = false;
    S.muteAudio = false;
    S.stepping = false;
    S.clearOutcome();
    S.dtScale = 1.f;
    S.forceDelta = true;
    S.halfPassMode = false;
    S.secondPass.clear();

    if (m_p.mute && m_audioSaved) {
        m_audioSaved = false;
        if (auto* fmod = FMODAudioEngine::sharedEngine()) {
            fmod->setBackgroundMusicVolume(m_savedMusic);
            fmod->setEffectsVolume(m_savedSfx);
        }
    }
    if (pl && m_hid && pl->m_objectLayer) pl->m_objectLayer->setVisible(m_wasVisible);
    m_hid = false;

    m_snap = {};
    m_s0 = {};
    m_rec.clear();
    m_rec.shrink_to_fit();
    m_haveSnap = false;
    m_t = Trial{};
    m_isControl = false;

    auto res = m_res;
    if (res) {
        size_t unrel = 0;
        for (auto const& r : res->inputs) if (r.status == InputStatus::Unreliable) ++unrel;
        if (unrel > 0)
            res->warnings.push_back(fmt::format(
                "{} input(s) could not be measured because replaying the macro does not reproduce the first pass (it drifts or dies at a "
                "different place). That is typical for a macro recorded with practice checkpoints, which joins attempts the game does not "
                "restore perfectly. Record it again with the setting \"Recording: use practice checkpoints\" turned off and analyse that.", unrel));
    }
    if (res) {
        size_t total = 0, covered = 0, unreliable = 0, aborted = 0, failed = 0, unreached = 0;
        for (auto const& r : res->inputs) {
            if (r.status == InputStatus::NotApplicable) continue;
            ++total;
            switch (r.status) {
                case InputStatus::Measured: case InputStatus::Capped: ++covered; break;
                case InputStatus::Unreliable: ++unreliable; break;
                case InputStatus::Aborted: ++aborted; break;
                case InputStatus::BaselineFail: ++failed; break;
                default: ++unreached; break;
            }
        }
        if (total > 0 && covered < total)
            res->warnings.push_back(fmt::format(
                "Coverage: {} of {} inputs measured. Not measured: {} unreliable replay, {} trial budget hit, {} die without any shift, {} "
                "not reachable (the replay of the macro ends or dies before them, or they come too late to judge).",
                covered, total, unreliable, aborted, failed, unreached));
    }
    if (res && m_macro && res->inputs.size() <= m_macro->inputs.size()) {
        size_t pr = 0, prTight = 0, rl = 0, rlTight = 0;
        for (auto const& r : res->inputs) {
            if ((r.status != InputStatus::Measured && r.status != InputStatus::Capped) || r.index < 0 ||
                static_cast<size_t>(r.index) >= m_macro->inputs.size()) continue;
            const bool down = m_macro->inputs[static_cast<size_t>(r.index)].down;
            (down ? pr : rl)++;
            if (r.window <= 2.0 + 1e-6) (down ? prTight : rlTight)++;
        }
        res->warnings.push_back(fmt::format("Measured {} clicks ({} with a window of 2 or less) and {} releases ({} with a window of 2 or less).", pr, prTight, rl, rlTight));
    }
    if (res && m_res && m_plans.size() == res->inputs.size()) {
        // Every window exactly one frame although shifting was allowed: that is what a broken replay looks like, not a real result.
        size_t meas = 0, ones = 0;
        for (size_t q = 0; q < res->inputs.size(); ++q) {
            auto const& r = res->inputs[q];
            if (r.status != InputStatus::Measured || m_plans[q].earlier + m_plans[q].later == 0) continue;
            ++meas;
            if (r.earlier <= 0.0 && r.later <= 0.0) ++ones;
        }
        if (meas >= 5 && ones * 10 >= meas * 9)
            res->warnings.push_back(fmt::format(
                "{} of {} measured inputs came out as exactly 1 frame (every shift died, even +-1). That is almost never real: another mod is "
                "probably changing the physics or the death check during the analysis (noclip / safe mode, speedhack, TPS bypass, a physics "
                "fix). Turn those off and analyse again, and send the Geode log lines starting with [Windowpane] input.", ones, meas));
    }
    if (res && m_badStarts > 0)
        res->warnings.push_back(fmt::format(
            "{} group(s) started from a snapshot that did not reproduce the first pass (the unmodified macro died from it, or a trial died before "
            "its shift). Those inputs were measured by replaying from the start of the level instead, so they are exact but slower.", m_badStarts));
    if (res && m_localGroups > 0)
        res->warnings.push_back(fmt::format(
            "{} input(s) in {} group(s) were measured from a snapshot that does not restore bit-exactly: each trial started a few ticks "
            "before the click with the player's position, velocity and rotation set from the first pass. The unmodified macro was checked to "
            "replay from that start, so the windows are valid, but a very tight one can be off by a tick. Turn off \"Start trials just "
            "before the click\" for the slower, strictly exact fallback.", m_localInputs, m_localGroups));
    if (res && m_confirmFlips > 0)
        res->warnings.push_back(fmt::format(
            "{} trial(s) died once and survived when repeated with the identical shift, so the replay is not fully deterministic here. "
            "They were counted as survived. If this number is large, another mod is changing the physics during the analysis.", m_confirmFlips));
    if (res && m_deepBlocked > 0)
        res->warnings.push_back(fmt::format(
            "{} trial(s) matched the original run in position and velocity but not in the hidden player state, so they were run to the end of "
            "the check window instead of stopping early. That is slower, not less accurate.", m_deepBlocked));
    if (res && !error.empty() && error != "Cancelled")
        res->warnings.push_back("The analysis stopped before the end: " + error);
    else if (res && cancelled && m_macro && m_doneInputs < m_toMeasure)
        res->warnings.push_back(fmt::format("The analysis was cancelled after {} of {} inputs (Esc, closing the progress window, or the analyze key was pressed).", m_doneInputs, m_toMeasure));
    if (res) {
        res->totalTrials = m_trialsTotal;
        res->totalTicks = m_ticksTotal;
        res->elapsedSec = secondsSince(m_t0);
        res->cancelled = cancelled;
        res->error = error;
        res->cbfSupported = m_cbfActive;
        res->complete = !cancelled && error.empty();
    }

    m_phase = Phase::Idle;
    m_pl = nullptr;

    // Put the level back at its start so the player is not left staring at a mid-simulation state.
    // (Skipped after an exception: the aborted update can leave the game's object sections half rewritten, and resetting on top of
    // that crashed inside GJBaseGameLayer::removeObjectFromSection.)
    if (pl) {
        pl->m_queuedButtons.clear();
        if (resetLevel) {
            // The last trial can leave a vehicle's sprite showing; clear them all so the next attempt starts with the plain icon.
            resetModeVisuals(pl->m_player1);
            resetModeVisuals(pl->m_player2);
            pl->resetLevelFromStart();
        }
    }
    auto cb = onFinished;
    if (cb && res) cb(res);
}

AnalysisProgress Analyzer::progress() const {
    AnalysisProgress p;
    p.active = active();
    p.phase = m_phaseText;
    p.speedX = m_speedX;
    p.trialsPerSec = m_trialsPerSec;
    p.throttle = m_throttle;
    p.trials = m_trialsTotal;
    p.doneInputs = m_doneInputs;
    p.totalInputs = m_toMeasure;
    p.cbfActive = m_cbfActive;
    p.elapsedSec = secondsSince(m_t0);
    switch (m_phase) {
        case Phase::Init: p.fraction = 0.0; break;
        case Phase::Baseline:
            p.fraction = m_baseTickLimit > 0 ? 0.08 * std::min(1.0, static_cast<double>(m_rec.size()) / static_cast<double>(m_baseTickLimit)) : 0.0;
            break;
        case Phase::Repair: p.fraction = 0.04; break;
        case Phase::Group:
        case Phase::Reset:     // exact trials pass through Reset between trials: it must not show 0%
        case Phase::Measure:
            p.fraction = 0.08 + 0.92 * (m_toMeasure > 0 ? static_cast<double>(m_doneInputs) / m_toMeasure : 1.0);
            break;
        default: p.fraction = m_shownFraction; break;
    }
    if (m_doneInputs > 0 && p.fraction < m_shownFraction) p.fraction = m_shownFraction;   // never jumps back once inputs are done
    m_shownFraction = p.fraction;
    if (p.fraction > 0.02 && p.active) p.etaSec = p.elapsedSec * (1.0 - p.fraction) / p.fraction;
    return p;
}

// --------------------------------------------------------------------------------------------------
// the slice loop
// --------------------------------------------------------------------------------------------------

void Analyzer::runSlice(PlayLayer* pl, double realDt) {
    if (!active()) return;
    if (pl != m_pl) { abandon(); return; }
    if (m_cancelRequested) {
        finish(pl, !m_failed, m_failed ? m_failText : m_cancelReason);
        return;
    }

    try {
        const double frameTarget = 1.0 / m_p.minFps;
        realDt = std::clamp(realDt, 1e-4, 1.0);

        // Auto-throttle: the previous frame includes our previous slice. If frames got long, back off hard;
        // while they are comfortably short, creep back up.
        // beta.23: only the game's own frame cost counts here. Our slice used to count too, so long levels (more to draw and update per
        // tick) pushed the throttle down to the floor and the speed fell to ~10x while short levels stayed at ~40x.
        const double overhead = std::max(0.0, realDt - m_lastSliceSec);
        m_frameEma = m_frameEma <= 0 ? overhead : m_frameEma * 0.8 + overhead * 0.2;
        if (m_p.autoThrottle) {
            if (overhead > frameTarget * 0.8) m_throttle = std::max(0.03, m_throttle * 0.8);
            else m_throttle = std::min(1.0, m_throttle * 1.08 + 0.01);
        } else {
            m_throttle = 1.0;
        }

        const double wantTicks = m_p.speedX * tps() * std::min(realDt, frameTarget) * m_throttle;
        const int64_t tickBudget = static_cast<int64_t>(std::clamp(wantTicks, 4.0, 200000.0));
        // Hard wall-clock ceiling for this slice so a frame can never exceed ~half the minimum-FPS budget.
        const double timeBudget = std::clamp(0.75 * frameTarget * (0.3 + 0.7 * m_throttle), 0.003, 0.25);

        const auto start = Clock::now();
        const int64_t trialsBefore = m_trialsTotal;
        int64_t ticks = 0;

        while (active() && !m_cancelRequested) {
            int64_t r = 0;
            switch (m_phase) {
                case Phase::Init: r = unitInit(pl); break;
                case Phase::Baseline: r = unitBaseline(pl); break;
                case Phase::Group: r = unitGroup(pl); break;
                case Phase::Reset: r = unitReset(pl); break;
                case Phase::Measure: r = unitMeasure(pl); break;
                case Phase::Repair: r = unitRepair(pl); break;
                default: r = -1; break;
            }
            if (r < 0) break;
            ticks += r;
            if (ticks >= tickBudget) break;
            if (std::chrono::duration<double>(Clock::now() - start).count() > timeBudget) break;
        }

        m_lastSliceSec = std::chrono::duration<double>(Clock::now() - start).count();
        m_ticksTotal += ticks;
        const double a = 0.15;
        m_speedX = m_speedX * (1 - a) + (static_cast<double>(ticks) / realDt / tps()) * a;
        m_trialsPerSec = m_trialsPerSec * (1 - a) + (static_cast<double>(m_trialsTotal - trialsBefore) / realDt) * a;

        if (active() && m_cancelRequested) finish(pl, !m_failed, m_failed ? m_failText : m_cancelReason);
    } catch (std::bad_alloc const&) {
        abortAfterException(pl, "ran out of memory", "std::bad_alloc");
    } catch (std::exception const& e) {
        abortAfterException(pl, std::string("internal error: ") + e.what(), typeid(e).name());
    } catch (...) {
        abortAfterException(pl, "unknown internal error", "unknown exception");
    }
}

// Ends the analysis after a C++ exception escaped a trial. This runs from the catch handlers above, so it must stay light: no level
// reset (see finish), and the game is left alone except for what finish() restores.
void Analyzer::abortAfterException(PlayLayer* pl, std::string const& what, char const* type) {
    log::error("[Windowpane] exception during analysis ({}): {} | phase={} input={}/{} trials={} rec={} ticks={}", type, what,
               static_cast<int>(m_phase), m_doneInputs, m_toMeasure, m_trialsTotal, m_rec.size(), m_ticksTotal);
    if (!active()) return;
    finish(pl, false, "The analysis stopped: " + what + ". Leave the level and open it again before analysing once more.", false);
}

// --------------------------------------------------------------------------------------------------
// phase: init
// --------------------------------------------------------------------------------------------------

int64_t Analyzer::unitInit(PlayLayer* pl) {
    if (m_initFrames == 0) {
        m_phaseText = "Resetting the level";
        if (m_p.mute) {
            if (auto* fmod = FMODAudioEngine::sharedEngine(); fmod && !m_audioSaved) {
                m_savedMusic = fmod->m_musicVolume;
                m_savedSfx = fmod->m_sfxVolume;
                m_audioSaved = true;
                fmod->setBackgroundMusicVolume(0.f);
                fmod->setEffectsVolume(0.f);
            }
        }
        // Nothing needs to be drawn while we simulate thousands of steps; skipping the draw of the object
        // layer is the biggest single saving.
        if (!m_hid) m_wasVisible = pl->m_objectLayer->isVisible();
        pl->m_objectLayer->setVisible(false);
        m_hid = true;
        pl->m_queuedButtons.clear();
        pl->resetLevelFromStart();
        ++m_initFrames;
        return -1;   // let the game process the reset for a frame
    }
    ++m_initFrames;
    if (!pl->m_started && m_initFrames == 4) pl->startGame();
    if (!pl->m_started) {
        if (m_initFrames > 150) fail("The level never started after a reset (GJBaseGameLayer::m_started stayed false).");
        return -1;
    }
    if (ticksOf(pl->m_gameState.m_currentProgress) != 0) {
        fail(fmt::format("After a reset the level is at tick {}, expected 0.", ticksOf(pl->m_gameState.m_currentProgress)));
        return -1;
    }
    pl->m_queuedButtons.clear();
    if (!m_s0.capture(pl)) { fail("Could not take a checkpoint of the start of the level."); return -1; }

    if (!m_freshDone) {
        if (!calibrate(pl)) return -1;
        // The calibration put the level back by loading the start-of-level checkpoint, which is not exactly the same as a
        // brand-new attempt. Every exact trial starts from a genuine reset, so the original run must too, or the two could
        // differ in tiny ways. Reset once more (the calibration is not repeated).
        m_freshDone = true;
        m_initFrames = 0;
        return -1;
    }
    m_freshDone = false;

    m_warnMark = m_res->warnings.size();
    m_rec.clear();
    m_baseTickLimit = m_macro->lastTick + kBaseTail;
    m_rec.reserve(static_cast<size_t>(std::min<int64_t>(m_baseTickLimit + 16, 8'000'000)));
    m_baseCursor = 0;
    m_baseCbf = m_cbfActive;
    m_baseHalf = SimState::get().halfPassMode;
    m_phase = Phase::Baseline;
    m_phaseText = "Recording the original run";
    return 0;
}

// --------------------------------------------------------------------------------------------------
// phase: baseline (pass 1)
// --------------------------------------------------------------------------------------------------

void Analyzer::queueDue(Stepper& st, std::vector<InputEvent> const& ev, size_t& cursor, int64_t tick) {
    while (cursor < ev.size() && ev[cursor].tick <= tick) {
        st.queueInput(ev[cursor], m_cbfActive);
        ++cursor;
    }
}

int64_t Analyzer::unitBaseline(PlayLayer* pl) {
    Stepper st(pl);
    auto const& ev = m_macro->inputs;
    const uint32_t tick = st.tick();

    // The previous step already stored this exact pre-step state as its post-step state. Read tick 0 once,
    // then reuse that entry instead of capturing and hashing the same player state twice per baseline tick.
    if (m_rec.size() <= tick) {
        m_rec.resize(static_cast<size_t>(tick) + 1);
        m_rec[tick] = readTickState(pl);
    }

    // Record where each input happens (before the step that applies it) and queue it.
    while (m_baseCursor < ev.size() && ev[m_baseCursor].tick <= tick) {
        auto const& e = ev[m_baseCursor];
        auto& r = m_res->inputs[m_baseCursor];
        PState const& ps = (e.p2 && m_rec[tick].dual) ? m_rec[tick].p2 : m_rec[tick].p1;
        r.x = ps.x;
        r.y = ps.y;
        r.percent = pl->getCurrentPercent();
        {
            auto* pp = (e.p2 && m_rec[tick].dual) ? pl->m_player2 : pl->m_player1;
            r.speed = pp ? pp->m_playerSpeed : 0.f;   // speed portal state the click happens in (0.7 / 0.9 / 1 / 1.1 / 1.3)
            if (pp) {
                r.mode = pp->m_isShip ? "ship" : pp->m_isBird ? "ufo" : pp->m_isBall ? "ball" : pp->m_isDart ? "wave" :
                         pp->m_isRobot ? "robot" : pp->m_isSpider ? "spider" : pp->m_isSwing ? "swing" : "cube";
                if (pp->m_vehicleSize < 0.9f) r.mode += " (mini)";
            }
        }
        r.timeSec = static_cast<double>(tick) * (1.0 / tps());
        st.queueInput(e, m_cbfActive);
        ++m_baseCursor;
    }

    std::string fault;
    const StepResult res = st.step(fault);
    if (res == StepResult::Fault) { fail(fault); return -1; }

    bool done = false;
    if (res == StepResult::Died) {
        m_baseDied = true;
        m_baseEndTick = tick;   // index of the fatal step
        done = true;
        m_res->warnings.push_back(fmt::format(
            "The macro dies at tick {} ({:.1f}%) when replayed unmodified. Inputs that need to survive past that point cannot be measured - "
            "check that the macro belongs to this level / game version and that no other mod (noclip, speedhack, TPS bypass) is interfering.",
            tick, pl->getCurrentPercent()));
        {
            auto& S = SimState::get();
            auto* po = (S.deathP2 && pl->m_player2) ? pl->m_player2 : pl->m_player1;
            m_res->warnings.push_back(fmt::format("Death details: killed by object id {} (type {}), player at x={:.1f} y={:.1f}, m_started={}, paused={}.",
                                                  S.deathObjectId, S.deathObjectType, po ? po->getPositionX() : 0.f, po ? po->getPositionY() : 0.f,
                                                  pl->m_started, pl->m_isPaused));
        }
    } else if (res == StepResult::Ended) {
        m_baseEnded = true;
        m_baseEndTick = tick + 1;
        done = true;
    } else if (static_cast<int64_t>(tick) + 1 >= m_baseTickLimit) {
        m_baseEndTick = tick + 1;
        done = true;
    }
    {
        // Normally one step; the first step of a level may be two (see Stepper::lastAdvance): every tick it covered
        // gets the state after the step, so later comparisons against the first pass stay consistent.
        const uint32_t adv = std::max<uint32_t>(1, st.lastAdvance());
        if (m_rec.size() <= static_cast<size_t>(tick) + adv) m_rec.resize(static_cast<size_t>(tick) + adv + 1);
        auto const after = readTickState(pl);
        for (uint32_t k = 1; k <= adv; ++k) m_rec[tick + k] = after;
        if (adv > 1 && m_baseCursor < ev.size() && ev[m_baseCursor].tick > static_cast<int64_t>(tick) &&
            ev[m_baseCursor].tick < static_cast<int64_t>(tick) + adv)
            m_res->warnings.push_back("The first input is on tick 1, which this game setup runs together with tick 0; that input is applied one tick late.");
    }

    if (done && !m_timingCands.empty() && !m_timingFinal) {
        // Does the recorded run survive as far as the macro goes? If not, the frame numbering is probably different.
        const bool bad = m_baseDied && static_cast<int64_t>(m_baseEndTick) + 8 < m_macro->lastTick;
        if (m_timingIdx < m_timingReach.size())
            m_timingReach[m_timingIdx] = m_baseEnded ? std::numeric_limits<int64_t>::max() / 2 : static_cast<int64_t>(m_baseEndTick);
        if (bad && retryTiming(pl)) return -1;
    }
    if (done && m_p.repair && !m_repairDone && m_baseDied && static_cast<int64_t>(m_baseEndTick) + 8 < m_macro->lastTick &&
        (m_timingCands.empty() || m_timingFinal)) {
        // The macro dies long before it ends, and no other reading of its timing helps: try to repair it.
        if (startRepair(pl)) return 1;
    }
    if (done) {
        buildPlans();
        buildGroups();
        m_toMeasure = 0;
        for (auto const& p : m_plans) if (p.reachable) ++m_toMeasure;
        m_gi = 0;
        m_advRestored = false;
        m_advFromStart = false;
        m_groupReliable = true;
        m_groupNote.clear();
        m_haveSnap = false;
        if (m_groups.empty()) {
            m_res->warnings.push_back("No input could be measured.");
            finish(pl, false, "");
            return -1;
        }
        m_phase = Phase::Group;
        m_phaseText = "Measuring";
    }
    return 1;
}


// --------------------------------------------------------------------------------------------------
// Calibration: measures how many progress counts one update(1/240 s) advances (see calibrateTickRate) before the
// real run, from the start-of-level snapshot. The level is put back to tick 0 afterwards.
// --------------------------------------------------------------------------------------------------
void Analyzer::applyTiming(size_t idx) {
    auto const [scale, offset] = m_timingCands[idx];
    m_timingIdx = idx;
    const bool half = scale < 0.999 && progressPerTick() > 1;
    m_macro = (scale == 1.0 && offset == 0.0) ? m_origMacro : rescaleMacro(*m_origMacro, scale, offset, half);
    m_res->inputs.assign(m_macro->inputs.size(), InputResult{});
    for (size_t i = 0; i < m_res->inputs.size(); ++i) m_res->inputs[i].index = static_cast<int>(i);
    m_res->timingScale = scale;
    m_res->timingOffset = offset;
    m_res->scaledMacro = (scale == 1.0 && offset == 0.0) ? nullptr : m_macro;
    m_p.cbf = m_cbfRequested;
    m_res->cbf = m_cbfRequested;
    m_cbfProbed = false;
    m_cbfActive = false;
    m_rebaselined = false;
    m_freshDone = false;
}

// The replay died long before the macro ends. Try the next way of reading its frame numbers (counted per progress
// step, or off by one); when all were tried, run the one that survived longest.
bool Analyzer::retryTiming(PlayLayer* pl) {
    (void)pl;
    size_t next = m_timingIdx + 1;
    if (next >= m_timingCands.size()) {
        size_t best = 0;
        for (size_t i = 0; i < m_timingCands.size() && i < m_timingReach.size(); ++i)
            if (m_timingReach[i] > m_timingReach[best]) best = i;
        m_timingFinal = true;
        if (best == m_timingIdx) return false;   // the current reading already was the best one
        next = best;
    }
    log::info("[Windowpane] macro timing: reading #{} (scale {:g}, offset {:g}) died at tick {} of {}; trying #{}", m_timingIdx, m_timingCands[m_timingIdx].first,
              m_timingCands[m_timingIdx].second, m_baseEndTick, m_macro->lastTick, next);
    m_res->warnings.resize(std::min(m_res->warnings.size(), m_warnMark));
    applyTiming(next);
    // start over: reset the level and record the original run again
    SimState::get().halfPassMode = false;
    SimState::get().secondPass.clear();
    m_rec.clear();
    m_baseCursor = 0;
    m_baseDied = false;
    m_baseEnded = false;
    m_baseEndTick = 0;
    m_initFrames = 0;
    m_phase = Phase::Init;
    m_phaseText = fmt::format("Trying another reading of the macro's timing ({}/{})", next + 1, m_timingCands.size());
    return true;
}

bool Analyzer::calibrate(PlayLayer* pl) {
    auto err = calibrateTickRate(pl, m_s0);
    if (!err.empty()) { fail(err); return false; }
    if (progressPerTick() > 1 && (m_p.autoSubtick || m_macro->hasSubTick) && !m_rebaselined) {
        // The game takes input at finer than whole-tick granularity here (several command passes per tick), so a click
        // can sit between two ticks. Measure windows in those steps (sub-tick positions) instead of whole ticks.
        m_p.cbf = true;
        m_p.cbfSubdivisions = 2;   // first / second command pass of a tick
        m_res->cbf = true;
        SimState::get().halfPassMode = true;
        SimState::get().secondPass.clear();
        log::info("[Windowpane] measuring in half ticks (second command pass)");
    }
    return true;
}

// --------------------------------------------------------------------------------------------------
// planning
// --------------------------------------------------------------------------------------------------

void Analyzer::buildPlans() {
    auto const& ev = m_macro->inputs;
    const size_t n = ev.size();
    m_plans.assign(n, Plan{});

    // previous / next event of the same stream (same player + button). A press and the release that
    // follows it are neighbours in one stream, which is what bounds a short hold: a release cannot
    // move before its own press.
    std::vector<int64_t> prevTick(n, -1), nextTick(n, -1);
    std::map<std::pair<bool, int>, size_t> last;
    for (size_t i = 0; i < n; ++i) {
        auto key = std::make_pair(ev[i].p2, static_cast<int>(ev[i].button));
        if (auto it = last.find(key); it != last.end()) {
            prevTick[i] = ev[it->second].tick;
            nextTick[it->second] = ev[i].tick;
        }
        last[key] = i;
    }

    // pair every press with its release (the next event of the same player / button)
    std::vector<size_t> relOf(n, static_cast<size_t>(-1));
    {
        std::map<std::pair<bool, int>, size_t> open;
        for (size_t i = 0; i < n; ++i) {
            auto key = std::make_pair(ev[i].p2, static_cast<int>(ev[i].button));
            if (ev[i].down) open[key] = i;
            else if (auto it = open.find(key); it != open.end()) { relOf[it->second] = i; open.erase(it); }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        Plan& P = m_plans[i];
        const int64_t t = ev[i].tick;
        P.tick = t;
        P.rel = relOf[i];
        const int64_t roomEarlier = (prevTick[i] >= 0 ? t - (prevTick[i] + 1) : t);
        int64_t roomLater = (nextTick[i] >= 0 ? (nextTick[i] - 1) - t : std::numeric_limits<int32_t>::max());
        if (m_p.keepHold && ev[i].down && P.rel != static_cast<size_t>(-1)) {
            // The click is moved together with its release, so only the NEXT press after the release limits it.
            const size_t r = P.rel;
            roomLater = (nextTick[r] >= 0 ? (nextTick[r] - 1) - ev[r].tick : std::numeric_limits<int32_t>::max());
        }
        P.earlier = static_cast<int>(std::clamp<int64_t>(std::min<int64_t>(m_p.maxShift, roomEarlier), 0, m_p.maxShift));
        P.later = static_cast<int>(std::clamp<int64_t>(std::min<int64_t>(m_p.maxShift, roomLater), 0, m_p.maxShift));
        P.snapTick = t - P.earlier;

        // The survival check is counted in ticks, but a tick covers more of the level the faster the player moves. At 4x speed a fixed
        // 60 tick check spans about twice the distance (and many more clicks, each with its own tolerance) than at 1x, which pushed
        // the windows of fast sections far too low. The tick parts of the check are therefore scaled by the player's speed (only ever
        // shortened, never lengthened).
        double speedScale = 1.0;
        if (t >= 0 && static_cast<size_t>(t) + 1 < m_rec.size()) {
            const size_t a = static_cast<size_t>(t), b = std::min(m_rec.size() - 1, a + 8);
            auto const& ta = m_rec[a];
            auto const& tb = m_rec[b];
            PState const& pa = (ev[i].p2 && ta.dual) ? ta.p2 : ta.p1;
            PState const& pb = (ev[i].p2 && tb.dual) ? tb.p2 : tb.p1;
            const double pxPerTick = std::abs(static_cast<double>(pb.x) - static_cast<double>(pa.x)) / static_cast<double>(b - a);
            if (pxPerTick > 1e-3) speedScale = std::clamp(311.58 / (pxPerTick * tps()), 0.4, 1.0);   // 311.58 px/s = normal speed
        }
        int horizonInputs = m_p.horizonInputs;
        int horizonTail = std::max(1, static_cast<int>(std::lround(m_p.horizonTail * speedScale)));
        int horizonMin = std::max(2, static_cast<int>(std::lround(m_p.horizonMin * speedScale)));
        int rejoinExtra = static_cast<int>(std::lround(m_p.rejoinExtra * speedScale));
        if (m_p.flyStrict > 0 && t >= 0 && static_cast<size_t>(t) < m_rec.size()) {
            auto const& ts = m_rec[static_cast<size_t>(t)];
            PState const& ps = (ev[i].p2 && ts.dual) ? ts.p2 : ts.p1;
            constexpr uint16_t kFlyModes = (1u << 0) | (1u << 3) | (1u << 6);   // ship, wave, swing
            // Ship / wave / swing keep an offset while the section lasts, so a few more clicks are checked after the shifted one. Only
            // the number of clicks grows: stretching the tick parts as well made every later click's own tolerance count against this one.
            if (ps.flags & kFlyModes) horizonInputs += std::min(m_p.flyStrict, 1);   // beta.21: at most one extra click, so later clicks' tight timing stops capping this one
        }
        const size_t j = std::min(n - 1, i + static_cast<size_t>(horizonInputs));
        int64_t h = ev[j].tick + horizonTail;
        h = std::max<int64_t>(h, t + horizonMin);
        h = std::max<int64_t>(h, t + P.later + horizonTail);
        h = std::min<int64_t>(h, t + m_p.horizonMax);
        P.horizonEnd = std::max<int64_t>(h, t + 1);

        // We can only judge survival as far as the recorded run goes. If the unmodified run itself ends (death or the end
        // of the recording) before the wanted horizon, shorten the horizon to the last tick it was alive instead of
        // dropping the input; the window is then reported with a note.
        const int64_t lastAlive = static_cast<int64_t>(m_baseEndTick) - (m_baseDied ? 1 : 0);
        bool clipped = false;
        if (!(m_baseEnded && t >= static_cast<int64_t>(m_baseEndTick)) && lastAlive < P.horizonEnd) {
            clipped = true;
            P.horizonEnd = lastAlive;
        }
        {
            // While a shifted run has not rejoined the original it keeps being checked up to here (the horizon above is the
            // part that must be survived; a run that is still off-course after it can die a little later).
            int64_t hard = P.horizonEnd + std::max(0, rejoinExtra);
            hard = std::min<int64_t>(hard, t + m_p.horizonMax);
            hard = std::min<int64_t>(hard, std::max<int64_t>(lastAlive, P.horizonEnd));
            P.hardEnd = std::max<int64_t>(hard, P.horizonEnd);
        }
        if (clipped && P.horizonEnd < t + 6) {
            P.reachable = false;
            P.note = fmt::format("the recorded run ends at tick {}, too soon after this input to see what happens", m_baseEndTick);
        } else if (m_baseEnded && t >= static_cast<int64_t>(m_baseEndTick)) {
            P.reachable = false;
            P.note = "the input happens after the level ended";
        } else if (!m_baseEnded && !m_baseDied && t >= static_cast<int64_t>(m_baseEndTick)) {
            P.reachable = false;
            P.note = "beyond the end of the recorded run";
        }
        if (P.reachable && !ev[i].down && m_p.releaseMode == 2) {
            P.reachable = false;
            auto& r = m_res->inputs[i];
            r.status = InputStatus::NotApplicable;
            r.note = "release timings are turned off";
            continue;
        }
        if (P.reachable && !ev[i].down && m_p.releaseMode == 0 && t >= 0 && static_cast<size_t>(t) < m_rec.size()) {
            // A release only has a timing window where holding does something: dash orbs, and the hold-controlled
            // vehicles (ship, wave, swing, robot). In cube / ball / ufo / spider the release is irrelevant.
            auto const& ts = m_rec[static_cast<size_t>(t)];
            PState const& ps = (ev[i].p2 && ts.dual) ? ts.p2 : ts.p1;
            constexpr uint16_t kHoldModes = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 9);   // ship, wave, robot, swing, dashing
            // Left / Right (platformer) releases always matter: they stop the movement.
            if (ev[i].button == Button::Jump && !(ps.flags & kHoldModes)) {
                P.reachable = false;
                auto& r = m_res->inputs[i];
                r.status = InputStatus::NotApplicable;
                r.note = "release timing does not matter in this mode";
                continue;
            }
        }
        if (!P.reachable) {
            auto& r = m_res->inputs[i];
            r.status = InputStatus::BaselineFail;
            r.note = P.note;
        } else if (clipped) {
            m_res->inputs[i].note = fmt::format("only checked until tick {} (the end of the recorded run), so the window can be too large", P.horizonEnd);
        }
    }
}

void Analyzer::buildGroups() {
    m_groups.clear();
    const size_t n = m_plans.size();
    // Exact mode: every trial is replayed from the very start of the level. No snapshot in the middle of the level is
    // involved, so a snapshot that does not restore perfectly can never spoil a result. One group, snapshot at tick 0.
    m_exact = (m_p.exact || m_autoExact) && m_macro->lastTick <= static_cast<int64_t>(30.0 * tps());   // up to 30 s of macro; longer ones use mid-level snapshots
    m_controlDone = false;
    if (m_exact) {
        size_t firstR = n, lastR = 0;
        for (size_t q = 0; q < n; ++q) {
            if (!m_plans[q].reachable) continue;
            m_plans[q].snapTick = 0;
            firstR = std::min(firstR, q);
            lastR = q;
        }
        if (firstR < n) m_groups.push_back(Group{firstR, lastR, 0});
        return;
    }
    int64_t prevSnap = 0;
    size_t i = 0;
    while (i < n) {
        if (!m_plans[i].reachable) { ++i; continue; }
        Group g{i, i, m_plans[i].snapTick};
        int64_t lo = m_plans[i].snapTick, hi = m_plans[i].snapTick;
        size_t k = i + 1;
        while (k < n) {
            if (!m_plans[k].reachable) { ++k; continue; }
            const int64_t nlo = std::min(lo, m_plans[k].snapTick), nhi = std::max(hi, m_plans[k].snapTick);
            if (nhi - nlo > kGroupSpan) break;
            lo = nlo;
            hi = nhi;
            g.last = k;
            ++k;
        }
        g.snapTick = std::max(lo, prevSnap);   // the baseline can only move forward
        // If clipping to prevSnap ever binds, the earlier search room of the affected inputs shrinks with it.
        for (size_t q = g.first; q <= g.last; ++q) {
            if (!m_plans[q].reachable) continue;
            const int64_t room = m_plans[q].tick - g.snapTick;
            if (room < m_plans[q].earlier) {
                m_plans[q].earlier = static_cast<int>(std::max<int64_t>(0, room));
                m_plans[q].snapTick = g.snapTick;
            }
        }
        prevSnap = g.snapTick;
        m_groups.push_back(g);
        i = g.last + 1;
    }
}

size_t Analyzer::firstEventAtOrAfter(int64_t tick) const {
    auto const& ev = m_macro->inputs;
    auto it = std::lower_bound(ev.begin(), ev.end(), tick, [](InputEvent const& e, int64_t t) { return e.tick < t; });
    return static_cast<size_t>(it - ev.begin());
}

// --------------------------------------------------------------------------------------------------
// phase: group (advance the baseline to the next snapshot point, snapshot, self-test)
// --------------------------------------------------------------------------------------------------

int64_t Analyzer::unitGroup(PlayLayer* pl) {
    if (m_gi >= m_groups.size()) { if (!startRecheck(pl)) finish(pl, false, ""); return m_rc ? 0 : -1; }
    Group const& g = m_groups[m_gi];
    auto const& ev = m_macro->inputs;

    const bool useStart = m_advFromStart || m_gi == 0 || !m_haveSnap;
    Snapshot const& from = useStart ? m_s0 : m_snap;

    if (!m_advRestored) {
        m_phaseText = fmt::format("Measuring input {} / {}", m_doneInputs + 1, m_toMeasure);
        if (useStart) {
            // Starting from the beginning of the level means a GENUINE new attempt, exactly like the first pass. Loading the
            // start-of-level checkpoint instead is not the same thing: for some levels the run from that restored state dies or
            // drifts (even before the first input) while the real one does not, and every input of the analysis was then reported
            // as "the replay diverged from the original run".
            if (!m_advResetting) {
                pl->m_queuedButtons.clear();
                pl->resetLevelFromStart();
                m_advResetting = true;
                m_advResetFrames = 1;
                return -1;
            }
            ++m_advResetFrames;
            if (!pl->m_started && m_advResetFrames >= 4 && m_advResetFrames % 2 == 0) pl->startGame();
            if (!pl->m_started) {
                if (m_advResetFrames > 150) {
                    if (m_transientRetries < 5) { ++m_transientRetries; log::warn("[Windowpane] group reset hiccup ({}/5)", m_transientRetries); m_advResetting = false; }
                    else fail("The level never started after a reset (GJBaseGameLayer::m_started stayed false).");
                }
                return -1;
            }
            if (ticksOf(pl->m_gameState.m_currentProgress) != 0) {
                if (m_transientRetries < 5) { ++m_transientRetries; log::warn("[Windowpane] group reset hiccup ({}/5): tick {}", m_transientRetries, ticksOf(pl->m_gameState.m_currentProgress)); m_advResetting = false; }
                else fail(fmt::format("After a reset the level is at tick {}, expected 0.", ticksOf(pl->m_gameState.m_currentProgress)));
                return -1;
            }
            pl->m_queuedButtons.clear();
            m_advResetting = false;
            m_advRestored = true;
            m_advCursor = 0;
            return 0;
        }
        if (!from.restore(pl)) { fail("Restoring a snapshot failed."); return -1; }
        if (m_snapLocal) forcePlayerState(pl, from.tick);   // beta.28: local start (see Analyzer.hpp)
        m_advRestored = true;
        m_advCursor = firstEventAtOrAfter(from.tick);
        return 0;
    }

    Stepper st(pl);
    const uint32_t tick = st.tick();

    if (static_cast<int64_t>(tick) < g.snapTick) {
        queueDue(st, ev, m_advCursor, tick);
        std::string fault;
        const StepResult r = st.step(fault);
        if (r == StepResult::Fault) {
            if (m_transientRetries < 5) {
                ++m_transientRetries;
                log::warn("[Windowpane] step fault while advancing to a group, starting that stretch again ({}/5): {}", m_transientRetries, fault);
                m_advRestored = false;
                m_advFromStart = true;
                m_advResetting = false;
                m_haveSnap = false;
                return 1;
            }
            fail(fault);
            return -1;
        }

        std::string drift;
        if (r == StepResult::Died) drift = fmt::format("died at tick {}", tick);
        else if (r == StepResult::Ended) drift = fmt::format("ended at tick {}", tick);
        else if (m_groupReliable && !m_advNoCompare && tick + 1 <= m_baseEndTick && tick + 1 < m_rec.size()) {
            auto d = compareTickState(readTickState(pl, false), m_rec[tick + 1], (m_snapLocal && !useStart) ? kLocalTol : kDriftTol);
            if (!d.empty()) drift = fmt::format("{} differs from the first pass at tick {}", d, tick + 1);
        }
        if (!drift.empty()) {
            if (!useStart && !m_snapLocal && !m_exact && !m_autoExact && m_macro->lastTick <= static_cast<int64_t>(30.0 * tps())) {
                // A save state of this level does not restore exactly (the restored run drifts from the first pass). Windows measured
                // from save states are then wrong in ways nothing flags (the edge of a window that "died" or "survived" only because of
                // the drift), including the inputs measured BEFORE the drift was noticed. For a macro this short a genuine replay of
                // every trial is affordable: start over with exact replays and throw the save-state results away.
                m_autoExact = true;
                log::warn("[Windowpane] save state drift ({}): restarting the analysis with exact replays", drift);
                m_res->warnings.resize(std::min(m_res->warnings.size(), m_warnMark));
                m_res->warnings.push_back(fmt::format("A save state of this level did not restore exactly ({}), so the analysis was restarted with exact replays (every trial replays the level from the start). Slower, but not affected by it.", drift));
                m_warnMark = m_res->warnings.size();
                m_res->inputs.assign(m_macro->inputs.size(), InputResult{});
                for (size_t q = 0; q < m_res->inputs.size(); ++q) m_res->inputs[q].index = static_cast<int>(q);
                m_rec.clear();
                m_baseCursor = 0;
                m_baseDied = m_baseEnded = false;
                m_baseEndTick = 0;
                m_haveSnap = false;
                m_snap = {};
                m_freshDone = false;
                m_initFrames = 0;
                m_doneInputs = 0;
                m_inputActive = false;
                m_t.running = false;
                m_phase = Phase::Init;
                m_phaseText = "Save states drifted: starting over with exact replays";
                return 1;
            }
            if (!useStart) {
                // The restored snapshot drifted. Repair: replay this stretch from the very start instead.
                m_res->warnings.push_back(fmt::format("Snapshot drift ({}); that stretch was replayed from the start of the level.", drift));
                log::warn("[Windowpane] snapshot drift: {}", drift);
                m_advFromStart = true;
                m_advRestored = false;
            } else {
                // A replay from the start does not reproduce pass 1.
                log::warn("[Windowpane] replay from start diverged: {}", drift);
                if (r == StepResult::Ok) {
                    // Only a small difference, the replay itself is alive: keep measuring (every trial of this group is replayed from the
                    // start the same way, and a trial that dies before its own shift is caught separately). Do not throw the inputs away.
                    if (!m_driftNoted) {
                        m_driftNoted = true;
                        m_res->warnings.push_back(fmt::format("A replay from the start of the level differs slightly from the first pass ({}). The inputs were still measured; if another mod changes the physics, tight windows can be off.", drift));
                    }
                    m_advNoCompare = true;
                    return 1;
                }
                if (m_groupRetry < 3) {
                    // The replay died / ended before this input although the first pass did not: run this stretch again from the start.
                    ++m_groupRetry;
                    m_advRestored = false;
                    m_advFromStart = true;
                    m_haveSnap = false;
                    return 1;
                }
                m_groupReliable = false;
                m_groupNote = "the replay diverged from the original run (" + drift + ")";
                if (r != StepResult::Ok) {
                    // the state is dead / finished, so there is nothing to snapshot; skip this group
                    markUnreliable(g.first, g.last, m_groupNote);
                    ++m_gi;
                    m_advRestored = false;
                    m_advFromStart = true;
                    m_haveSnap = false;
                    m_groupReliable = true;
                    return 1;
                }
            }
        }
        return 1;
    }

    // Reached the snapshot tick: capture, self-test.
    if (!m_snap.capture(pl)) { fail("Could not capture a snapshot."); return -1; }
    m_haveSnap = true;
    m_transientRetries = 0;
    m_snapLocal = false;

    bool selfTestFailed = false;
    // Imported macros: after several groups in a row whose snapshots were not exact, stop testing (each test costs two long
    // replays) and measure the remaining groups by replaying from the start of the level, which is always exact.
    const bool skipTest = m_macro->imported && m_stFailStreak >= 3 && m_s0.valid() && !m_exact;   // beta.25: back to 3, one failed test is often a one-off
    // beta.28: with local start on, the exact self-test (two long replays) is skipped after 3 failures in a row, because the cheaper local
    // check below decides anyway. It is tried again every 8th group, in case the level got easier to snapshot.
    const bool localSkip = m_p.localStart && !m_localOff && m_p.selfTest && !m_exact && m_localStreak >= 3 && (m_gi % 8) != 0;
    if (localSkip) {
        m_groupReliable = false;
        selfTestFailed = true;
        m_groupNote = "the exact snapshot self-test was skipped (it failed in the last groups)";
    } else
    if (skipTest) {
        if (!m_stSkipNoted) {
            m_stSkipNoted = true;
            m_res->warnings.push_back("Snapshots of this level did not restore exactly in several places in a row, so the rest of the inputs are measured by replaying every trial from the start of the level (slower, exact).");
        }
        m_groupReliable = false;
        selfTestFailed = true;
        m_groupNote = "snapshots of this level are not exact";
    } else
    if (m_groupReliable && m_p.selfTest && !m_exact) {   // exact mode never restores a snapshot
        std::string note;
        // A snapshot can restore almost exactly and still drift later (level state the restore does not bring back). A trial then dies
        // far from where it started, which the short test cannot see. Compare over the whole stretch a trial of this group can run.
        int ticks = kSelfTestTicks;
        if (m_p.longSelfTest) {
            int64_t end = g.snapTick;
            for (size_t q = g.first; q <= g.last; ++q) if (m_plans[q].reachable) end = std::max(end, m_plans[q].hardEnd);
            ticks = static_cast<int>(std::clamp<int64_t>(end - g.snapTick, kSelfTestTicks, 600));
        }
        if (!runSelfTest(pl, note, ticks)) {
            m_groupReliable = false;
            selfTestFailed = true;
            m_groupNote = "snapshot self-test failed: " + note;
            log::warn("[Windowpane] self-test failed at tick {}: {}", g.snapTick, note);
            diagNote(m_res, fmt::format("self-test failed at tick {} (inputs {}-{}): {}", g.snapTick, g.first + 1, g.last + 1, note));
            if (m_p.localStart) ++m_localStreak;
        } else {
            m_stFailStreak = 0;
            m_localStreak = 0;
        }
    }
    if (selfTestFailed && m_p.localStart && !m_localOff && !m_exact) {
        // beta.28 local start. The snapshot is not bit-exact, but what matters is whether a trial started from it behaves like the
        // first pass: restore it, set the players' position / velocity / rotation to what the first pass had, replay the unmodified macro
        // over the whole stretch a trial of this group can run. If that stays alive and on the first pass, the group is measured from
        // this snapshot (just before the click) instead of replaying from the last exact snapshot or from tick 0 for every trial.
        int64_t lend = g.snapTick;
        for (size_t q = g.first; q <= g.last; ++q) if (m_plans[q].reachable) lend = std::max(lend, m_plans[q].hardEnd);
        const int lticks = static_cast<int>(std::clamp<int64_t>(lend - g.snapTick, kSelfTestTicks, 600));
        std::string lnote;
        if (runLocalCheck(pl, lnote, lticks)) {
            m_snapLocal = true;
            m_groupReliable = true;
            selfTestFailed = false;
            m_groupNote.clear();
            ++m_localGroups;
            for (size_t q = g.first; q <= g.last; ++q) if (m_plans[q].reachable) ++m_localInputs;
            log::info("[Windowpane] local start at tick {} (inputs {}-{}): snapshot not bit-exact, forced player state replays like the first pass", g.snapTick, g.first + 1, g.last + 1);
        } else {
            log::warn("[Windowpane] local start failed at tick {}: {}", g.snapTick, lnote);
            diagNote(m_res, fmt::format("local start failed at tick {} (inputs {}-{}): {}", g.snapTick, g.first + 1, g.last + 1, lnote));
            m_groupNote += " | local start: " + lnote;
        }
    }
    if (!m_groupReliable && selfTestFailed && m_lastGood.valid() && m_lastGood.tick < static_cast<uint32_t>(std::max<int64_t>(0, g.snapTick))) {
        // beta.26: restart the trials of this group from the last snapshot that passed its self-test, instead of from tick 0. The
        // trials then replay only from there (the same kind of restore the normal groups use), which is much shorter on long levels.
        m_snap = m_lastGood;
        m_snapLocal = false;
        m_haveSnap = true;
        m_groupExact = false;
        m_groupReliable = true;
        m_groupNote.clear();
        ++m_lastGoodRestarts;
    } else if (!m_groupReliable && selfTestFailed && m_s0.valid()) {
        // Snapshots are not exact at this point of the level. Do not give up on these inputs: measure them by
        // replaying every trial from the very start of the level instead (slower, but needs no snapshot).
        if (!skipTest)
            m_res->warnings.push_back(fmt::format("Inputs {}-{}: snapshots were not exact here ({}); measured by replaying from the start of the level.",
                                                  g.first + 1, g.last + 1, m_groupNote));
        m_snap = m_s0;
        m_snapLocal = false;
        if (m_macro->imported) {
            // Imported macro: every trial of this group is a genuine new attempt (level reset + the macro from tick 0). Restoring
            // m_s0 is NOT equivalent (see the note in unitGroup / the changelog) and made every shifted trial die.
            m_groupExact = true;
            m_haveSnap = false;
            if (!skipTest) ++m_stFailStreak;
        }
        m_groupReliable = true;
        m_groupNote.clear();
    }
    if (!m_groupReliable) {
        markUnreliable(g.first, g.last, m_groupNote);
        ++m_gi;
        m_advRestored = false;
        m_advFromStart = true;     // the state we are standing on cannot be trusted as a starting point
        m_groupReliable = true;
        m_groupNote.clear();
        return 0;
    }

    if (m_haveSnap && !m_groupExact && !m_snapLocal) m_lastGood = m_snap;

    if (m_p.cbf && !m_cbfProbed) {
        for (size_t q = g.first; q <= g.last; ++q) {
            if (m_plans[q].reachable && ev[q].down) { runCbfProbe(pl, q); break; }
        }
    }

    if (m_macro->hasSubTick && !m_rebaselined &&
        (m_baseCbf != m_cbfActive || m_baseHalf != SimState::get().halfPassMode)) {
        // The original run was recorded before it was known how clicks between ticks are delivered. The trials deliver them
        // the other way, so their comparison with that recording would be meaningless: record it again the same way.
        m_rebaselined = true;
        log::info("[Windowpane] re-recording the original run in sub-tick mode (cbf {}, half pass {})", m_cbfActive, SimState::get().halfPassMode);
        m_res->warnings.resize(std::min(m_res->warnings.size(), m_warnMark));
        m_res->inputs.assign(m_macro->inputs.size(), InputResult{});
        for (size_t q = 0; q < m_res->inputs.size(); ++q) m_res->inputs[q].index = static_cast<int>(q);
        m_rec.clear();
        m_baseCursor = 0;
        m_baseDied = m_baseEnded = false;
        m_baseEndTick = 0;
        m_haveSnap = false;
        m_snap = {};
        m_freshDone = false;
        m_initFrames = 0;
        m_phase = Phase::Init;
        m_phaseText = "Recording the original run again (sub-tick clicks)";
        return 0;
    }

    m_phase = Phase::Measure;
    m_ii = g.first;
    m_inputActive = false;
    m_t.running = false;
    return 0;
}

void Analyzer::markUnreliable(size_t first, size_t last, std::string const& note) {
    for (size_t q = first; q <= last && q < m_plans.size(); ++q) {
        if (!m_plans[q].reachable) continue;
        auto& r = m_res->inputs[q];
        r.status = InputStatus::Unreliable;
        r.note = note;
        ++m_doneInputs;
    }
    m_res->warnings.push_back(fmt::format("Inputs {}-{} could not be measured reliably: {}", first + 1, last + 1, note));
}

// Replays the UNMODIFIED macro from the snapshot (tick 0 in exact mode) for as long as the first pass was alive and
// checks that it survives exactly as long. A macro that does not replay cannot be measured. Small state differences
// are only warned about (the shifted runs are judged by surviving, not by matching the first pass).
bool Analyzer::runControl(PlayLayer* pl, std::string& note) {
    if (!m_snap.restore(pl)) { note = "restore failed"; return false; }
    Stepper st(pl);
    auto const& ev = m_macro->inputs;
    size_t cur = firstEventAtOrAfter(m_snap.tick);
    const int64_t limit = std::min<int64_t>(static_cast<int64_t>(m_baseEndTick), m_macro->lastTick + 120);
    std::string firstDiff;
    while (static_cast<int64_t>(st.tick()) < limit) {
        queueDue(st, ev, cur, st.tick());
        std::string fault;
        const StepResult r = st.step(fault);
        if (r == StepResult::Fault) { note = fault; return false; }
        const size_t idx = st.tick();
        if (r == StepResult::Died) {
            const bool expected = m_baseDied && static_cast<int64_t>(idx) + 1 >= static_cast<int64_t>(m_baseEndTick);
            if (!expected) {
                note = fmt::format("it dies at tick {} but the first pass survived until tick {}", idx, m_baseEndTick);
                return false;
            }
            break;
        }
        if (r == StepResult::Ended) break;
        if (firstDiff.empty() && idx < m_rec.size() && idx <= m_baseEndTick) {
            auto d = compareTickState(readTickState(pl, false), m_rec[idx], kDriftTol);
            if (!d.empty()) firstDiff = fmt::format("{} differs from the first pass at tick {}", d, idx);
        }
    }
    if (!firstDiff.empty()) {
        m_res->warnings.push_back("The replay is not bit-identical to the first pass (" + firstDiff + "); it survived, so the results are still valid.");
        log::warn("[Windowpane] control replay drift: {}", firstDiff);
    }
    return true;
}

bool Analyzer::runSelfTest(PlayLayer* pl, std::string& note, int ticks) {
    Stepper st(pl);
    auto const& ev = m_macro->inputs;
    const uint32_t base = m_snap.tick;
    const size_t cur0 = firstEventAtOrAfter(base);

    struct Run {
        std::vector<TickState> states;
        PlayerExtra e1, e2;
    };
    auto runOnce = [&](Run& out) -> bool {
        size_t cur = cur0;
        out.states.clear();
        for (int k = 0; k < ticks; ++k) {
            queueDue(st, ev, cur, st.tick());
            std::string fault;
            const StepResult r = st.step(fault);
            if (r == StepResult::Fault) { note = fault; return false; }
            out.states.push_back(readTickState(pl, false));
            if (r != StepResult::Ok) break;
        }
        out.e1.capture(pl->m_player1);
        out.e2.capture(pl->m_player2);
        return true;
    };

    Run a, b;
    if (!runOnce(a)) return false;                     // continues straight from the snapshot point
    if (!m_snap.restore(pl)) { note = "restore failed"; return false; }
    if (!runOnce(b)) return false;                     // same ticks after a restore

    if (a.states.size() != b.states.size()) {
        note = fmt::format("one run lasted {} ticks and the other {}", a.states.size(), b.states.size());
        return false;
    }
    for (size_t k = 0; k < a.states.size(); ++k) {
        auto d = compareTickState(a.states[k], b.states[k], 1e-5);
        if (!d.empty()) {
            note = fmt::format("after restoring, tick {} differs: {}", base + k + 1, d);
            auto fields = diffExtra(a.e1, b.e1);
            if (!fields.empty()) note += " [player 1 state that drifted: " + fields + "]";
            return false;
        }
    }
    // The continuing run should also match pass 1.
    for (size_t k = 0; k < a.states.size(); ++k) {
        const size_t idx = base + k + 1;
        if (idx < m_rec.size() && idx <= m_baseEndTick) {
            auto d = compareTickState(a.states[k], m_rec[idx], kDriftTol);
            if (!d.empty()) { note = fmt::format("pass 2 differs from pass 1 at tick {}: {}", idx, d); return false; }
        }
    }
    auto fields = diffExtra(a.e1, b.e1);
    if (!fields.empty()) {
        // visible state matched but hidden state did not: worth a warning, not a failure
        log::warn("[Windowpane] hidden player state differs after restore at tick {}: {}", base, fields);
    }
    return true;
}

// Puts both players back to the position / y velocity / rotation the first pass had at `tick` (m_rec[tick] is the state before step
// `tick`, which is exactly what a snapshot taken at that tick holds). Used right after restoring a snapshot that does not restore
// bit-exactly: the level state comes from the checkpoint, the part of the player that decides the next ticks comes from the recording.
bool Analyzer::forcePlayerState(PlayLayer* pl, uint32_t tick) {
    if (!pl || !pl->m_player1 || tick >= m_rec.size()) return false;
    auto const& ts = m_rec[tick];
    auto apply = [](PlayerObject* p, PState const& st) {
        if (!p) return;
        const cocos2d::CCPoint pt(st.x, st.y);
        p->setPosition(pt);
        p->m_position = pt;
        p->m_yVelocity = st.yVel;
        p->setRotation(st.rot);
    };
    apply(pl->m_player1, ts.p1);
    if (ts.dual && pl->m_player2) apply(pl->m_player2, ts.p2);
    return true;
}

// Local start check: restore the current group's snapshot, force the players to the first pass's state, replay the UNMODIFIED macro
// for `ticks` ticks. It must stay alive as long as the first pass did and stay within kLocalTol of it. If it does, every trial of the
// group can start from this snapshot (just before the click) and mean the same thing as in the first pass.
bool Analyzer::runLocalCheck(PlayLayer* pl, std::string& note, int ticks) {
    if (!m_snap.restore(pl)) { note = "restore failed"; return false; }
    if (!forcePlayerState(pl, m_snap.tick)) { note = "the first pass has no state at this tick"; return false; }
    Stepper st(pl);
    auto const& ev = m_macro->inputs;
    size_t cur = firstEventAtOrAfter(m_snap.tick);
    for (int k = 0; k < ticks; ++k) {
        queueDue(st, ev, cur, st.tick());
        std::string fault;
        const StepResult r = st.step(fault);
        if (r == StepResult::Fault) { note = fault; return false; }
        const size_t idx = st.tick();
        if (r == StepResult::Died) {
            const bool expected = m_baseDied && static_cast<int64_t>(idx) + 1 >= static_cast<int64_t>(m_baseEndTick);
            if (!expected) {
                note = fmt::format("it dies at tick {} but the first pass survived until tick {}", idx, m_baseEndTick);
                return false;
            }
            break;
        }
        if (r == StepResult::Ended) break;
        if (idx < m_rec.size() && idx <= m_baseEndTick) {
            auto d = compareTickState(readTickState(pl, false), m_rec[idx], kLocalTol);
            if (!d.empty()) { note = fmt::format("{} differs from the first pass at tick {}", d, idx); return false; }
        }
    }
    return true;
}

void Analyzer::runCbfProbe(PlayLayer* pl, size_t pressIndex) {
    m_cbfProbed = true;
    auto& S = SimState::get();
    auto const& all = m_macro->inputs;
    Plan const& P = m_plans[pressIndex];
    const size_t first = firstEventAtOrAfter(m_snap.tick);
    const int64_t t = P.tick;
    const bool half = S.halfPassMode;
    const int nSub = half ? 2 : 3;
    double y[3] = {0, 0, 0};
    const double subs[3] = {0.0, 0.5, 0.99};
    bool ok = pressIndex >= first && pressIndex < all.size();
    S.secondPassMissed = false;
    m_cbfActive = true;   // queueDue must send sub-tick inputs during the probe
    Stepper st(pl);
    for (int s = 0; s < nSub && ok; ++s) {
        if (!m_snap.restore(pl)) { ok = false; break; }
        if (m_snapLocal) forcePlayerState(pl, m_snap.tick);
        auto endIt = std::upper_bound(all.begin(), all.end(), t + 12, [](int64_t v, InputEvent const& e) { return v < e.tick; });
        const size_t endIdx = static_cast<size_t>(endIt - all.begin());
        if (endIdx <= pressIndex || endIdx < first) { ok = false; break; }
        std::vector<InputEvent> evs(all.begin() + static_cast<std::ptrdiff_t>(first), all.begin() + static_cast<std::ptrdiff_t>(endIdx));
        const size_t local = pressIndex - first;
        if (local >= evs.size()) { ok = false; break; }
        evs[local].sub = subs[s];
        size_t cur = 0;
        while (static_cast<int64_t>(st.tick()) < t + 8) {
            queueDue(st, evs, cur, st.tick());
            std::string fault;
            if (st.step(fault) != StepResult::Ok) { ok = false; break; }
        }
        PlayerObject* p = (all[pressIndex].p2 && pl->m_gameState.m_isDualMode) ? pl->m_player2 : pl->m_player1;
        y[s] = p->getPositionY();
    }
    S.secondPass.clear();
    bool monotone;
    if (half) {
        // a click in the second pass instead of the first must land somewhere different, and the second pass must exist
        monotone = ok && !S.secondPassMissed && std::abs(y[0] - y[1]) > 1e-4;
    } else {
        monotone = ok && std::abs(y[0] - y[1]) > 1e-4 && std::abs(y[1] - y[2]) > 1e-4 &&
                   ((y[0] > y[1] && y[1] > y[2]) || (y[0] < y[1] && y[1] < y[2]));
    }
    // the probe leaves the world in a trial state; the next trial restores anyway
    m_cbfActive = monotone;
    m_res->cbfSupported = monotone;
    if (!monotone) {
        S.halfPassMode = false;
        m_res->warnings.push_back(
            half ? "Half-tick measurement was needed, but a click in the second command pass of a tick changed nothing here. "
                   "Windows are reported in whole ticks."
                 : "CBF mode was requested, but this game build does not apply click timestamps inside a physics step "
                   "(a click moved by half a tick changed nothing). Windows are reported in whole ticks.");
    } else {
        log::info("[Windowpane] sub-tick probe passed: y = {:.4f} / {:.4f} / {:.4f}", y[0], y[1], y[2]);
    }
}

// --------------------------------------------------------------------------------------------------
// phase: measure
// --------------------------------------------------------------------------------------------------

bool Analyzer::startRecheck(PlayLayer* pl) {
    (void)pl;
    if (m_rc || m_rcDone || !m_p.recheck || m_cancelRequested || !m_s0.valid() || m_groups.empty()) return false;
    m_rcDone = true;
    m_rcList.clear();
    for (size_t i = 0; i < m_res->inputs.size() && i < m_plans.size(); ++i) {
        auto const& r = m_res->inputs[i];
        if (!m_plans[i].reachable) continue;
        // tight clicks (window 1-3), and every input the first pass could not settle (unreliable replay, trial budget hit)
        if ((r.status == InputStatus::Measured && r.window <= 3.0 + 1e-6) || r.status == InputStatus::Unreliable ||
            r.status == InputStatus::Aborted)
            m_rcList.push_back(i);
    }
    if (m_rcList.empty()) return false;
    // Every tight click (window 1-3) is measured once more at the very end: each trial is a genuine new attempt from the start of
    // the level, runs to the end with no early exit, and every death is replayed before it counts.
    m_rc = true;
    m_rcPos = 0;
    m_rcPrevValid = false;
    m_rcChanged = 0;
    m_groupExact = true;
    m_haveSnap = false;
    m_snap = m_s0;
    m_gi = 0;
    m_toMeasure += static_cast<int>(m_rcList.size());
    m_inputActive = false;
    m_t.running = false;
    m_phase = Phase::Measure;
    return true;
}

int64_t Analyzer::unitMeasure(PlayLayer* pl) {
    if (m_rc && !m_inputActive && !(m_isControl && m_t.running)) {
        if (m_rcPrevValid) {
            m_rcPrevValid = false;
            auto& r = m_res->inputs[m_rcPrev];
            if (std::abs(r.window - m_rcOld) > 1e-6) {
                ++m_rcChanged;
                r.note += std::string(r.note.empty() ? "" : " | ") + fmt::format("final re-check: window was {:.0f}", m_rcOld);
            }
        }
        if (m_rcPos >= m_rcList.size()) {
            m_res->warnings.push_back(fmt::format("Re-checked {} tight click(s) (window 1-3) at the end: {} changed.", m_rcList.size(), m_rcChanged));
            finish(pl, false, "");
            return -1;
        }
        m_ii = m_rcList[m_rcPos++];
        m_rcPrev = m_ii;
        m_rcPrevValid = true;
        m_rcOld = m_res->inputs[m_ii].window;
        beginInput();
        m_inputActive = true;
        m_phaseText = fmt::format("Re-checking tight click {} / {}", m_rcPos, m_rcList.size());
    }
    if (m_gi >= m_groups.size() && !m_rc) { if (!startRecheck(pl)) finish(pl, false, ""); return m_rc ? 0 : -1; }
    Group const& g = m_groups[m_gi];

    if (m_isControl && m_t.running) return stepControl(pl);

    if (!m_inputActive) {
        while (m_ii <= g.last && !m_plans[m_ii].reachable) ++m_ii;
        if (m_ii > g.last) {
            // next group
            ++m_gi;
            m_advRestored = false;
            m_advFromStart = false;
            m_groupExact = false;
            m_groupZeroOk = false;
            m_zeroProbe = false;
            m_advNoCompare = false;
            m_groupRetry = 0;
            m_groupReliable = true;
            m_groupNote.clear();
            if (m_gi >= m_groups.size()) { if (!startRecheck(pl)) finish(pl, false, ""); return m_rc ? 0 : -1; }
            m_phase = Phase::Group;
            return 0;
        }
        if (m_exact && !m_controlDone && !m_isControl) {
            // Before measuring anything: the unmodified macro, through the very same reset / step path as every trial.
            m_isControl = true;
            m_phaseText = "Checking that the unmodified macro replays";
            if (!beginTrial(pl, 0.0)) return -1;
            return 0;
        }
        beginInput();
        m_inputActive = true;
        m_phaseText = fmt::format("Measuring input {} / {}", m_doneInputs + 1, m_toMeasure);
    }

    if (!m_t.running) {
        double delta = 0;
        if (m_confirmPending) {
            m_confirmPending = false;
            m_confirming = !m_faultRetry;   // a rerun after a fault is not a death confirmation
            m_faultRetry = false;
            if (!beginTrial(pl, m_confirmDelta)) return -1;
            return 0;
        }
        if (!nextDelta(delta)) {
            finishInput(InputStatus::Pending, "");
            return 0;
        }
        if (!beginTrial(pl, delta)) return -1;
        return 0;   // the restore is heavy: let the budget check run
    }
    return stepTrial(pl);
}

void Analyzer::beginInput() {
    m_confirmPending = m_confirming = false;
    m_detRetry = 0;
    m_s = Search{};
    m_s.input = m_ii;
    m_s.side = m_plans[m_ii].earlier > 0 ? Side::Earlier : (m_plans[m_ii].later > 0 ? Side::Later : Side::Done);
}

void Analyzer::endSide() {
    m_s.step = 1;
    m_s.failAt = 0;
    m_s.stride = 0;
    m_s.refining = false;
    m_s.refineK = 1;
    if (m_s.side == Side::Earlier) m_s.side = m_plans[m_s.input].later > 0 ? Side::Later : Side::Done;
    else m_s.side = Side::Done;
}

bool Analyzer::nextDelta(double& delta) {
    Plan const& P = m_plans[m_s.input];
    const int N = m_cbfActive ? m_p.cbfSubdivisions : 1;
    for (int guard = 0; guard < 8; ++guard) {
        if (m_s.side == Side::Done) return m_p.verify ? nextVerifyDelta(delta) : false;
        const bool earlier = m_s.side == Side::Earlier;
        const int limit = earlier ? P.earlier : P.later;
        const int passed = earlier ? m_s.passedEarlier : m_s.passedLater;
        const double dir = earlier ? -1.0 : 1.0;

        if (m_s.trials >= m_p.maxTrials) {
            (earlier ? m_s.cappedEarlier : m_s.cappedLater) = true;   // ran out of trial budget
            endSide();
            continue;
        }
        if (!m_s.refining) {
            if (m_s.step > limit) {
                // Survived every shift we were allowed to try. If that limit is the user's cap (not a
                // neighbouring input) the true window is at least this big.
                if (limit >= m_p.maxShift) (earlier ? m_s.cappedEarlier : m_s.cappedLater) = true;
                endSide();
                continue;
            }
            delta = dir * m_s.step;
            return true;
        }
        if (m_s.refineK >= N) { endSide(); continue; }
        delta = dir * (passed + static_cast<double>(m_s.refineK) / N);
        return true;
    }
    return false;
}

// Boundary re-check. The search above may stop a trial early when the shifted run looks like the original again. The two
// outermost shifts that were accepted are therefore run once more to the very end with no early exit; if one of them
// dies after all, the window is lowered by one step and that side is checked again.
bool Analyzer::nextVerifyDelta(double& delta) {
    if (m_s.verifySide < 0) { m_s.verifySide = 0; m_s.verifyTries = 0; }
    while (m_s.verifySide < 2) {
        const bool earlier = m_s.verifySide == 0;
        int& passed = earlier ? m_s.passedEarlier : m_s.passedLater;
        double& frac = earlier ? m_s.fracEarlier : m_s.fracLater;
        if (m_s.verifyTries >= 16) { passed = 0; frac = 0; }
        const double reach = passed + frac;
        if (reach <= 1e-9) { ++m_s.verifySide; m_s.verifyTries = 0; continue; }
        delta = (earlier ? -1.0 : 1.0) * reach;
        const bool alreadyFull = earlier
            ? (m_s.fullEarlier && std::abs(m_s.fullEarlierDelta - delta) <= 1e-9)
            : (m_s.fullLater && std::abs(m_s.fullLaterDelta - delta) <= 1e-9);
        if (alreadyFull) {
            // This exact shift already survived without a convergence early-exit. Replaying it would test the same
            // trajectory twice; only converged trials need the separate no-early-exit verification.
            ++m_s.verifySide;
            m_s.verifyTries = 0;
            continue;
        }
        return true;
    }
    return false;
}

void Analyzer::onTrialResult(bool survived) {
    if (m_zeroProbe) {
        // the unshifted probe of this group ended the way the first pass did (survived, or died where the first pass died), so the start
        // reproduces the first pass and the shifted trial's death is real: run that trial again
        (void)survived;
        m_zeroProbe = false;
        m_groupZeroOk = true;
        m_confirmPending = true;
        m_confirmDelta = m_probeDelta;
        m_faultRetry = true;
        return;
    }
    if (m_confirming) {
        m_confirming = false;
        if (survived) ++m_confirmFlips;   // died the first time, survived the repeat
    }
    if (survived && m_t.completedFull && m_s.side != Side::Done) {
        // Keep evidence for the eventual boundary check. A later early-exit trial simply leaves the previous
        // full-length delta in place and therefore still gets its own verification.
        if (m_s.side == Side::Earlier) {
            m_s.fullEarlier = true;
            m_s.fullEarlierDelta = m_t.delta;
        } else {
            m_s.fullLater = true;
            m_s.fullLaterDelta = m_t.delta;
        }
    }
    {
        if (!m_s.trace.empty()) m_s.trace += ' ';
        if (m_s.side == Side::Done) m_s.trace += 'v';   // boundary re-check
        m_s.trace += fmt::format("{:+g}{}", m_t.delta, survived ? 'o' : 'x');
        // where the shifted run died, in ticks after the click (the click itself is at 0): a small number means the click's own jump /
        // flight killed it, a large one means a later input that stayed where it was
        if (!survived && m_t.deathTick >= 0)
            m_s.trace += fmt::format("@{}", m_t.deathTick - static_cast<int64_t>(m_macro->inputs[m_s.input].tick));
    }
    if (m_s.side == Side::Done) {
        if (survived) {
            ++m_s.verifySide;
            m_s.verifyTries = 0;
        } else {
            const bool ve = m_s.verifySide == 0;
            int& vp = ve ? m_s.passedEarlier : m_s.passedLater;
            double& vf = ve ? m_s.fracEarlier : m_s.fracLater;
            const int vN = m_cbfActive ? m_p.cbfSubdivisions : 1;
            if (vf > 1e-9) vf = std::max(0.0, vf - 1.0 / vN);
            else if (vp > 0) --vp;
            m_s.downgraded = true;
            ++m_s.verifyTries;
        }
        return;
    }
    const bool earlier = m_s.side == Side::Earlier;
    int& passed = earlier ? m_s.passedEarlier : m_s.passedLater;
    double& frac = earlier ? m_s.fracEarlier : m_s.fracLater;
    const int N = m_cbfActive ? m_p.cbfSubdivisions : 1;
    if (!m_s.refining) {
        // Integer search on this side. `passed` is the furthest shift that survived. With fast search the first kLinearHead shifts are
        // still tried one by one (so every window up to that size is found exactly as before); past that the shift gallops outwards
        // (+4, +8, +16, ...) until one dies and the gap between the last survivor and the first death is bisected. A 24-tick window
        // then costs about 10 trials per side instead of 25.
        bool boundary = false;   // passed + 1 is known to die: the integer search of this side is finished
        const int limit = earlier ? m_plans[m_s.input].earlier : m_plans[m_s.input].later;
        if (survived) {
            passed = m_s.step;
            if (m_s.failAt > 0) {
                if (m_s.failAt - passed <= 1) boundary = true;
                else m_s.step = passed + (m_s.failAt - passed) / 2;
            } else if (!m_p.fastSearch || passed < kLinearHead || passed >= limit) {
                ++m_s.step;
            } else {
                m_s.stride = m_s.stride > 0 ? m_s.stride * 2 : 4;
                m_s.step = std::min(limit, passed + m_s.stride);
            }
        } else if (!m_p.fastSearch || m_s.step - passed <= 1) {
            boundary = true;
        } else {
            m_s.failAt = m_s.step;
            m_s.step = passed + (m_s.failAt - passed) / 2;
        }
        if (boundary) {
            if (m_cbfActive && N > 1) {
                m_s.refining = true;
                m_s.refineK = 1;
            } else {
                endSide();
            }
        }
    } else {
        if (survived) {
            frac = static_cast<double>(m_s.refineK) / N;
            ++m_s.refineK;
        } else {
            endSide();
        }
    }
}

bool Analyzer::beginTrial(PlayLayer* pl, double delta) {
    ++m_s.trials;
    ++m_trialsTotal;
    if (exactNow()) {
        // A genuine new attempt: reset the level exactly the way the first pass started, no checkpoint involved.
        pl->m_queuedButtons.clear();
        pl->resetLevelFromStart();
        m_resetFrames = 1;
        m_pendingDelta = delta;
        m_phase = Phase::Reset;
        return true;
    }
    if (!m_snap.restore(pl)) { fail("Restoring a snapshot failed."); return false; }
    if (m_snapLocal) forcePlayerState(pl, m_snap.tick);   // beta.28: start just before the click, player state from the first pass
    m_forcePending = m_snapLocal;                         // ... and once more right before the first step (a frame passes in between)
    return prepareTrial(pl, delta);
}

// Runs the level reset of an exact trial again after a transient failure; ends the analysis only when it keeps failing.
bool Analyzer::retryReset(PlayLayer* pl, std::string const& why) {
    if (m_transientRetries < 5) {
        ++m_transientRetries;
        log::warn("[Windowpane] reset hiccup, trying again ({}/5): {}", m_transientRetries, why);
        pl->m_queuedButtons.clear();
        pl->resetLevelFromStart();
        m_resetFrames = 1;
        return true;
    }
    fail(why);
    return false;
}

int64_t Analyzer::unitReset(PlayLayer* pl) {
    ++m_resetFrames;
    if (!pl->m_started && m_resetFrames >= 2 && m_resetFrames % 2 == 0) pl->startGame();   // asked again every 2nd frame until the level runs
    if (!pl->m_started) {
        if (m_resetFrames > 150) retryReset(pl, "The level never started after a reset (GJBaseGameLayer::m_started stayed false).");
        return -1;
    }
    if (ticksOf(pl->m_gameState.m_currentProgress) != 0) {
        retryReset(pl, fmt::format("After a reset the level is at tick {}, expected 0.", ticksOf(pl->m_gameState.m_currentProgress)));
        return -1;
    }
    m_transientRetries = 0;
    pl->m_queuedButtons.clear();
    m_phase = Phase::Measure;
    if (!prepareTrial(pl, m_pendingDelta)) return -1;
    return 0;
}

bool Analyzer::prepareTrial(PlayLayer* pl, double delta) {
    (void)pl;
    if (m_isControl) return prepareControl();
    auto const& all = m_macro->inputs;
    const size_t i = m_s.input;
    Plan const& P = m_plans[i];
    const size_t first = firstEventAtOrAfter(m_snap.tick);
    // The trial may run on past the horizon (see Plan::hardEnd), so every input up to the end of that stretch must be in it.
    const int64_t evEnd = std::max<int64_t>(P.horizonEnd, P.hardEnd);
    auto endIt = std::upper_bound(all.begin(), all.end(), evEnd + 1,
                                  [](int64_t v, InputEvent const& e) { return v < e.tick; });
    const size_t endIdx = static_cast<size_t>(endIt - all.begin());
    if (i < first || i >= endIdx) { fail("Internal error: trial window does not contain the input being measured."); return false; }

    m_t.events.assign(all.begin() + first, all.begin() + endIdx);
    const size_t local = i - first;
    InputEvent moved = m_t.events[local];
    // The shift is applied to the click's real position, which includes its own sub-tick part (half-tick / CBF macros).
    // Dropping that part would move the click by a different amount than the trial claims.
    const double pos = static_cast<double>(moved.tick) + moved.sub + delta;
    const int64_t tk = static_cast<int64_t>(std::floor(pos + 1e-9));
    moved.tick = tk;
    moved.sub = std::max(0.0, pos - static_cast<double>(tk));
    if (moved.sub < 1e-9) moved.sub = 0.0;
    // keep the hold length: the paired release moves with the click
    bool movedRel = false;
    InputEvent rel;
    int64_t relOrig = 0;
    if (m_p.keepHold && all[i].down && P.rel != static_cast<size_t>(-1) && P.rel >= first && P.rel < endIdx) {
        const size_t lr = P.rel - first;
        rel = m_t.events[lr];
        relOrig = rel.tick;
        const double rpos = static_cast<double>(rel.tick) + rel.sub + delta;
        const int64_t rtk = static_cast<int64_t>(std::floor(rpos + 1e-9));
        rel.tick = rtk;
        rel.sub = std::max(0.0, rpos - static_cast<double>(rtk));
        if (rel.sub < 1e-9) rel.sub = 0.0;
        movedRel = true;
        // erase the higher index first so the lower one stays valid
        const size_t hi = std::max(local, lr), lo = std::min(local, lr);
        m_t.events.erase(m_t.events.begin() + static_cast<std::ptrdiff_t>(hi));
        m_t.events.erase(m_t.events.begin() + static_cast<std::ptrdiff_t>(lo));
        m_t.events.insert(std::upper_bound(m_t.events.begin(), m_t.events.end(), moved, EventLess{}), moved);
        m_t.events.insert(std::upper_bound(m_t.events.begin(), m_t.events.end(), rel, EventLess{}), rel);
    } else {
        m_t.events.erase(m_t.events.begin() + static_cast<std::ptrdiff_t>(local));
        m_t.events.insert(std::upper_bound(m_t.events.begin(), m_t.events.end(), moved, EventLess{}), moved);
    }

    m_t.delta = delta;
    m_t.horizonEnd = static_cast<uint32_t>(P.horizonEnd);
    m_t.hardEnd = std::max<int64_t>(P.hardEnd, P.horizonEnd);
    m_t.fullRun = m_s.side == Side::Done || m_rc;   // boundary re-check (and the final tight-click re-check): no early exit
    m_t.completedFull = false;
    m_t.deepBlocked = false;
    m_t.cursor = 0;
    m_t.converge = 0;
    m_t.lastDiffTick = std::max<int64_t>(all[i].tick, tk) + 1;
    m_t.firstDiffTick = std::min<int64_t>(all[i].tick, tk);
    if (movedRel) {
        m_t.lastDiffTick = std::max<int64_t>(m_t.lastDiffTick, std::max<int64_t>(relOrig, rel.tick) + 1);
        m_t.firstDiffTick = std::min<int64_t>(m_t.firstDiffTick, std::min<int64_t>(relOrig, rel.tick));
    }
    m_t.running = true;
    m_t.deathTick = -1;
    return true;
}

// The snapshot this group's trials start from cannot reproduce the first pass (the unmodified macro dies from it, or a trial dies before
// anything was shifted). Every shifted trial would die for that reason and the window would come out as 1 frame. Measure the group by
// replaying from the very start of the level instead (slow but exact) and run the trial that was interrupted again.
void Analyzer::startBroken(std::string const& why, double resumeDelta) {
    ++m_badStarts;
    if (m_badStarts >= 2) m_localOff = true;
    log::warn("[Windowpane] snapshot start rejected at input {} ({}): measuring this group by replaying from the start of the level", m_s.input + 1, why);
    diagNote(m_res, fmt::format("snapshot start rejected at input {} (tick {}): {}", m_s.input + 1, m_macro->inputs[m_s.input].tick, why));
    m_zeroProbe = false;
    m_forcePending = false;
    if (!m_s0.valid()) {
        finishInput(InputStatus::Unreliable, "the snapshot start does not reproduce the first pass (" + why + ")");
        return;
    }
    m_snap = m_s0;
    m_snapLocal = false;
    m_haveSnap = false;           // the next group advances from the start of the level
    m_groupExact = true;
    m_groupZeroOk = true;
    m_confirmPending = true;
    m_confirmDelta = resumeDelta;
    m_faultRetry = true;          // not a death confirmation
}

int64_t Analyzer::stepTrial(PlayLayer* pl) {
    if (m_forcePending) {         // right before the first step, exactly like runLocalCheck does it
        m_forcePending = false;
        if (m_snapLocal) forcePlayerState(pl, m_snap.tick);
    }
    Stepper st(pl);
    const uint32_t tick = st.tick();
    if (static_cast<int64_t>(tick) > m_t.hardEnd) {
        // Survived the whole check window without ever being identical to the original again.
        if (m_t.deepBlocked) ++m_deepBlocked;
        m_t.running = false;
        m_t.completedFull = true;
        onTrialResult(true);
        return 0;
    }
    queueDue(st, m_t.events, m_t.cursor, tick);
    std::string fault;
    const StepResult r = st.step(fault);
    if (r == StepResult::Fault) {
        if (m_transientRetries < 5) {
            // One update that does not advance (or advances oddly) is usually a hiccup of the game, not the end of the analysis: run
            // this very trial again from a fresh start.
            ++m_transientRetries;
            log::warn("[Windowpane] step fault, running the trial again ({}/5): {}", m_transientRetries, fault);
            m_t.running = false;
            m_confirmPending = true;
            m_confirmDelta = m_t.delta;
            m_faultRetry = true;
            return 1;
        }
        fail(fault);
        return -1;
    }
    if (r == StepResult::Died) {
        m_t.running = false;
        m_t.deathTick = static_cast<int64_t>(tick);
        if (!exactNow() && static_cast<int64_t>(tick) + 1 < static_cast<int64_t>(m_baseEndTick)) {
            // Died before the first pass did. A death before anything was shifted, or of the unshifted trial itself, means the start is
            // wrong, not the shift. Any other first death of a shifted trial in this group is checked once with an unshifted trial.
            if (m_zeroProbe) { startBroken(fmt::format("the unshifted macro dies at tick {}", tick), m_probeDelta); return 1; }
            if (static_cast<int64_t>(tick) < m_t.firstDiffTick) { startBroken(fmt::format("a trial died at tick {} before its shift", tick), m_t.delta); return 1; }
            if (!m_groupZeroOk && std::abs(m_t.delta) > 1e-9) {
                m_zeroProbe = true;
                m_probeDelta = m_t.delta;
                m_confirmPending = true;
                m_confirmDelta = 0.0;
                m_faultRetry = true;
                return 1;
            }
        }
        if (exactNow() && static_cast<int64_t>(tick) < m_t.firstDiffTick) {
            // Died while the run was still identical to the original, which survived: the replay is not deterministic. Replay the same
            // trial again (up to three times) before giving the input up.
            if (m_detRetry < 3) {
                ++m_detRetry;
                m_confirmPending = true;
                m_confirmDelta = m_t.delta;
                return 1;
            }
            finishInput(InputStatus::Unreliable,
                        fmt::format("a replay from the start of the level died at tick {} although the first pass survived (tick {} is before this input's shift): the game does not replay this macro deterministically",
                                    tick, tick));
            return 1;
        }
        if (!m_confirming && (m_rc || std::abs(m_t.delta) <= 2.0)) {
            // A death at a shift of one or two ticks is what turns a real window into "1 frame". Replay that exact trial once more
            // before believing it: if the repeat survives, the replay is not deterministic here and the survival counts.
            m_confirmPending = true;
            m_confirmDelta = m_t.delta;
            return 1;
        }
        onTrialResult(false);
        return 1;
    }
    if (r == StepResult::Ended) {
        m_t.running = false;
        m_t.completedFull = true;
        onTrialResult(true);
        return 1;
    }
    const size_t nxt = static_cast<size_t>(st.tick());   // tick after this step (2 ahead on the very first step of a level)
    if (exactNow() && static_cast<int64_t>(nxt) <= m_t.firstDiffTick && nxt < m_rec.size() && nxt <= m_baseEndTick) {
        // still the untouched part: it must equal the first pass (small differences are only reported once)
        auto d = compareTickState(readTickState(pl, false), m_rec[nxt], kDriftTol);
        if (!d.empty() && !m_warnedControl) {
            m_warnedControl = true;
            m_res->warnings.push_back(fmt::format("A replay from the start differs slightly from the first pass ({} at tick {}); it survives, so results stay valid.", d, nxt));
            log::warn("[Windowpane] replay drift from the start: {} at tick {}", d, nxt);
        }
    }
    // Early exit: once the shifted run is physically identical to the original for a few ticks in a row,
    // it will stay identical (same inputs from here on), so it survives exactly as long as the original.
    // The comparison includes the hidden player state, not just position / velocity / flags, so a run that only LOOKS like the
    // original (but would be buffering a jump, sitting on a used orb, ...) is not accepted early. A boundary re-check never
    // exits early at all.
    if (!m_t.fullRun && static_cast<int64_t>(nxt) > m_t.lastDiffTick && nxt < m_rec.size() && nxt <= m_baseEndTick) {
        auto cur = readTickState(pl, false);
        const bool useDeep = m_deepUsable && !m_snapLocal;   // a local start never reproduces the hidden state of the first pass
        bool matches = visibleTickStateEqual(cur, m_rec[nxt], kConvTol);
        if (matches && useDeep) {
            // Most trials differ visibly. Only pay for the dozens of hidden-field hashes once the cheap comparison passes.
            addDeepHashes(pl, cur);
            matches = cur.p1.deep == m_rec[nxt].p1.deep && (!cur.dual || cur.p2.deep == m_rec[nxt].p2.deep);
            if (!matches) m_t.deepBlocked = true;
        }
        if (matches) {
            if (++m_t.converge >= m_p.convergeTicks) {
                m_t.running = false;
                onTrialResult(true);
            }
        } else {
            m_t.converge = 0;
        }
    }
    return 1;
}

void Analyzer::finishInput(InputStatus forced, std::string const& note) {
    const size_t i = m_s.input;
    auto& r = m_res->inputs[i];
    const double earlier = m_s.passedEarlier + m_s.fracEarlier;
    const double later = m_s.passedLater + m_s.fracLater;
    r.earlier = earlier;
    r.later = later;
    r.window = earlier + later + 1.0;
    r.trials = m_s.trials;
    r.trace = m_s.trace;
    {
        // Say WHY a side is zero: no room (a neighbouring input of the same button is right next to it, so a shift would reorder or
        // merge them) is a property of the macro; a shift that was tried and died is a measurement.
        Plan const& P = m_plans[i];
        std::string why;
        if (P.earlier == 0) why += "earlier: no room (previous input of this button is adjacent)";
        else if (earlier <= 0.0) why += "earlier: the 1 tick shift died";
        if (P.later == 0) why += std::string(why.empty() ? "" : "; ") + "later: no room (next input of this button is adjacent)";
        else if (later <= 0.0) why += std::string(why.empty() ? "" : "; ") + "later: the 1 tick shift died";
        if (!why.empty()) {
            log::info("[Windowpane] input {} zero side(s): {} | room {}/{} ticks", i + 1, why, P.earlier, P.later);
            if (forced == InputStatus::Pending && note.empty() && r.note.empty()) r.note = why;
        }
    }
    log::info("[Windowpane] input {} (tick {}): window {:.2f} = {:.2f} earlier + {:.2f} later + 1 | {}", i + 1,
              m_macro->inputs[i].tick, r.window, earlier, later, m_s.trace);
    bool capped = m_s.cappedEarlier || m_s.cappedLater;
    if (m_s.downgraded) {
        // a side that was lowered by the re-check no longer reaches the maximum shift
        Plan const& P = m_plans[i];
        capped = (m_s.cappedEarlier && m_s.passedEarlier >= P.earlier) || (m_s.cappedLater && m_s.passedLater >= P.later);
    }
    r.status = forced != InputStatus::Pending ? forced : (capped ? InputStatus::Capped : InputStatus::Measured);
    if (!note.empty()) r.note = note;
    else if (capped) r.note = "the window reached the maximum shift, so it is at least this large";
    if (m_s.downgraded) {
        if (!r.note.empty()) r.note += " | ";
        r.note += "the final full-length re-check of the edge failed, so the window was lowered";
    }
    ++m_doneInputs;
    ++m_ii;
    m_inputActive = false;
    m_t.running = false;
}

// --------------------------------------------------------------------------------------------------
// control trial (exact mode)
// --------------------------------------------------------------------------------------------------

bool Analyzer::prepareControl() {
    m_t.events = m_macro->inputs;   // nothing moved
    m_t.cursor = 0;
    m_t.converge = 0;
    m_t.delta = 0;
    m_t.fullRun = true;
    m_t.completedFull = false;
    m_t.deepBlocked = false;
    m_controlLimit = std::min<int64_t>(static_cast<int64_t>(m_baseEndTick), m_macro->lastTick + 120);
    m_controlDiff.clear();
    m_t.running = true;
    m_t.deathTick = -1;
    return true;
}

void Analyzer::endControl(bool ok, std::string const& note) {
    m_isControl = false;
    m_controlDone = true;
    m_t.running = false;
    if (!ok) {
        log::warn("[Windowpane] control replay failed: {}", note);
        if (m_gi < m_groups.size()) {
            Group const& g = m_groups[m_gi];
            markUnreliable(g.first, g.last, "the unmodified macro does not replay the same way twice (" + note + ")");
        }
        m_gi = m_groups.size();   // nothing measured from here on could be trusted: the next unit finishes the analysis
        return;
    }
    if (!m_controlDiff.empty()) {
        m_res->warnings.push_back("The unmodified replay is not bit-identical to the first pass (" + m_controlDiff +
                                  "); it survived, so the results are still valid.");
        log::warn("[Windowpane] control replay drift: {}", m_controlDiff);
    }
}

// One tick of the control trial. The unmodified macro is replayed from a fresh reset exactly like a shifted trial would be
// and has to survive exactly as long as the first pass did. It is also the check that the hidden-state comparison used for
// early exits means something: two identical replays must agree on it at every tick.
int64_t Analyzer::stepControl(PlayLayer* pl) {
    Stepper st(pl);
    const uint32_t tick = st.tick();
    if (static_cast<int64_t>(tick) >= m_controlLimit) { endControl(true, ""); return 0; }
    queueDue(st, m_t.events, m_t.cursor, tick);
    std::string fault;
    const StepResult r = st.step(fault);
    if (r == StepResult::Fault) { fail(fault); return -1; }
    if (r == StepResult::Died) {
        const bool expected = m_baseDied && static_cast<int64_t>(tick) == static_cast<int64_t>(m_baseEndTick);
        endControl(expected, expected ? std::string() :
                   fmt::format("it dies at tick {} but the first pass {} {}", tick, m_baseDied ? "died at tick" : "survived until tick", m_baseEndTick));
        return 1;
    }
    if (r == StepResult::Ended) {
        endControl(true, "");
        return 1;
    }
    const size_t idx = st.tick();
    if (idx < m_rec.size() && idx <= m_baseEndTick) {
        auto cur = readTickState(pl, false);
        if (m_controlDiff.empty()) {
            auto d = compareTickState(cur, m_rec[idx], kDriftTol);
            if (!d.empty()) m_controlDiff = fmt::format("{} differs from the first pass at tick {}", d, idx);
        }
        if (m_deepUsable && m_controlDiff.empty() && visibleTickStateEqual(cur, m_rec[idx], kConvTol)) {
            addDeepHashes(pl, cur);
            const bool deepDiffers = cur.p1.deep != m_rec[idx].p1.deep || (cur.dual && cur.p2.deep != m_rec[idx].p2.deep);
            if (deepDiffers) {
                m_deepUsable = false;
                m_res->warnings.push_back(fmt::format(
                    "Two identical replays disagree on the hidden player state at tick {}, so that extra check is switched off for this analysis "
                    "(early exits compare position, velocity and flags only; the final re-check of every window edge still runs the full length).", idx));
                log::warn("[Windowpane] hidden-state hash differs between two identical replays at tick {}; switched off", idx);
            }
        }
    }
    return 1;
}

// --------------------------------------------------------------------------------------------------
// macro repair
// --------------------------------------------------------------------------------------------------

namespace {
using StreamSig = std::map<std::pair<bool, int>, std::vector<bool>>;
StreamSig streamSig(std::vector<InputEvent> const& ev) {
    StreamSig s;
    for (auto const& e : ev) s[{e.p2, static_cast<int>(e.button)}].push_back(e.down);
    return s;
}
}  // namespace

bool Analyzer::startRepair(PlayLayer* pl) {
    (void)pl;
    m_rp = Repair{};
    m_rp.events = m_macro->inputs;
    m_rp.reach = static_cast<int64_t>(m_baseEndTick);
    m_rp.bestReach = m_rp.reach;
    repairBuildCandidates();
    if (m_rp.cand.empty()) { m_repairDone = true; return false; }
    log::info("[Windowpane] the macro dies at tick {} of {}; trying to repair it ({} candidate inputs)", m_baseEndTick, m_macro->lastTick, m_rp.cand.size());
    m_phase = Phase::Repair;
    m_phaseText = fmt::format("Repairing the macro (it dies at tick {})", m_rp.reach);
    return true;
}

void Analyzer::repairBuildCandidates() {
    auto& R = m_rp;
    R.cand.clear();
    const int64_t D = R.reach;
    std::vector<std::pair<int64_t, size_t>> v;
    for (size_t i = 0; i < R.events.size(); ++i) {
        const int64_t t = R.events[i].tick;
        if (t >= D - 240 && t <= D + 8) v.emplace_back(std::abs(t - D), i);
    }
    std::sort(v.begin(), v.end());
    for (size_t k = 0; k < v.size() && k < 12; ++k) R.cand.push_back(v[k].second);
    R.ci = 0;
    R.si = 0;
    R.bestCand = -1;
    R.bestShift = 0;
    R.bestReach = R.reach;
    R.bestEnded = false;
    R.refining = false;
    R.passed.clear();
}

// The working macro with input `idx` moved by `shift` ticks (its release moves along when "keep hold" is on). Returns false
// when the move is impossible (before tick 0, or it would swap the order of a stream's presses and releases).
bool Analyzer::repairMakeTrial(size_t idx, int shift, std::vector<InputEvent>& out) const {
    auto const& ev = m_rp.events;
    if (idx >= ev.size()) return false;
    out = ev;
    const double base = 1.0;   // a macro's ticks are in its own tick rate, which is the one analysed
    const double scale = m_macro->timingScale > 0 ? m_macro->timingScale : 1.0;
    auto moveOne = [&](InputEvent& e) {
        const double pos = static_cast<double>(e.tick) + e.sub + shift;
        if (pos < 0.0) return false;
        const int64_t tk = static_cast<int64_t>(std::floor(pos + 1e-9));
        e.tick = tk;
        e.sub = std::max(0.0, pos - static_cast<double>(tk));
        if (e.sub < 1e-9) e.sub = 0.0;
        e.raw = static_cast<int64_t>(std::llround((pos - m_macro->timingOffset) / (base * scale)));   // keep the file's own frame numbering in step
        return true;
    };
    if (!moveOne(out[idx])) return false;
    if (m_p.keepHold && ev[idx].down) {
        for (size_t k = idx + 1; k < ev.size(); ++k) {
            if (ev[k].p2 == ev[idx].p2 && ev[k].button == ev[idx].button) {
                if (!ev[k].down && !moveOne(out[k])) return false;
                break;
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), EventLess{});
    return streamSig(out) == streamSig(ev);
}

void Analyzer::repairApply(size_t candIdx, int shift, int64_t reach, bool ended) {
    auto& R = m_rp;
    std::vector<InputEvent> nev;
    if (!repairMakeTrial(candIdx, shift, nev)) return;
    R.notes.push_back(fmt::format("input {} (tick {}) moved by {:+d} ticks", candIdx + 1, R.events[candIdx].tick, shift));
    R.events = std::move(nev);
    R.reach = reach;
    R.ended = ended;
    ++R.applied;
    ++R.rounds;
    // a fix was taken: whatever "best so far" was found for the old death no longer applies
    R.bestCand = -1;
    R.bestShift = 0;
    R.bestReach = reach;
    R.bestEnded = false;
}

void Analyzer::repairNextRound() {
    auto& R = m_rp;
    if (R.ended || R.rounds >= 40 || R.trials >= 900) { return; }
    repairBuildCandidates();
}

void Analyzer::repairTrialDone(int64_t reachTick, bool ended) {
    auto& R = m_rp;
    R.running = false;
    const bool good = ended || reachTick >= R.reach + 60;   // clearly past the old death
    if (R.refining) {
        if (good) {
            R.passed.push_back({R.curShift, {reachTick, ended}});
            const int next = R.curShift + R.refDir;
            if (std::abs(next) <= 2 * m_p.repairMaxShift) { R.refNext = next; return; }
        }
        // the range of working shifts has ended: take the middle one
        auto const& mid = R.passed[(R.passed.size() - 1) / 2];
        repairApply(R.refCand, mid.first, mid.second.first, mid.second.second);
        R.refining = false;
        R.passed.clear();
        repairNextRound();
        if (R.ended || R.rounds >= 40 || R.trials >= 900) R.cand.clear();
        return;
    }
    if (ended || reachTick > R.bestReach) {
        R.bestCand = static_cast<int>(R.cand[R.curCandIdx]);
        R.bestShift = R.curShift;
        R.bestReach = ended ? std::numeric_limits<int64_t>::max() / 2 : reachTick;
        R.bestEnded = ended;
    }
    if (good) {
        R.refining = true;
        R.refDir = R.curShift < 0 ? -1 : 1;
        R.refCand = R.cand[R.curCandIdx];
        R.passed.clear();
        R.passed.push_back({R.curShift, {reachTick, ended}});
        R.refNext = R.curShift + R.refDir;
        if (std::abs(R.refNext) > 2 * m_p.repairMaxShift) {
            // nowhere further to look: use it as it is
            auto const& mid = R.passed[0];
            repairApply(R.refCand, mid.first, mid.second.first, mid.second.second);
            R.refining = false;
            R.passed.clear();
            repairNextRound();
            if (R.ended || R.rounds >= 40 || R.trials >= 900) R.cand.clear();
        }
    }
}

void Analyzer::finishRepair(PlayLayer* pl) {
    (void)pl;
    auto& R = m_rp;
    m_repairDone = true;
    m_res->warnings.resize(std::min(m_res->warnings.size(), m_warnMark));
    if (R.applied > 0) {
        auto nm = std::make_shared<Macro>(*m_macro);
        nm->inputs = R.events;
        nm->lastTick = nm->inputs.empty() ? 0 : nm->inputs.back().tick;
        nm->hasSubTick = std::any_of(nm->inputs.begin(), nm->inputs.end(), [](InputEvent const& e) { return e.sub != 0.0; });
        m_macro = nm;
        m_res->scaledMacro = nm;
        m_res->repairedInputs = R.applied;
        std::string list;
        for (size_t k = 0; k < R.notes.size() && k < 6; ++k) list += (k ? "; " : "") + R.notes[k];
        m_res->warnings.push_back(fmt::format("The macro died at tick {} when replayed, so {} click(s) were moved to make it survive: {}{}. The repaired macro is used from now on.",
                                              m_baseEndTick, R.applied, list, R.notes.size() > 6 ? "; ..." : ""));
        log::info("[Windowpane] macro repaired: {} click(s) moved, it now survives until tick {}{}", R.applied, R.ended ? int64_t(nm->lastTick) : R.reach, R.ended ? " (the end)" : "");
    } else {
        log::info("[Windowpane] macro repair found nothing that helps (tried {} replays)", R.trials);
    }
    // record the original run again with the (possibly repaired) macro
    m_res->inputs.assign(m_macro->inputs.size(), InputResult{});
    for (size_t q = 0; q < m_res->inputs.size(); ++q) m_res->inputs[q].index = static_cast<int>(q);
    m_rec.clear();
    m_baseCursor = 0;
    m_baseDied = m_baseEnded = false;
    m_baseEndTick = 0;
    m_freshDone = false;
    m_initFrames = 0;
    m_rp = Repair{};
    m_phase = Phase::Init;
    m_phaseText = "Recording the original run again";
}

int64_t Analyzer::unitRepair(PlayLayer* pl) {
    auto& R = m_rp;
    if (R.resetting) {
        ++R.resetFrames;
        if (!pl->m_started && R.resetFrames == 4) pl->startGame();
        if (!pl->m_started) {
            if (R.resetFrames > 150) fail("The level never started after a reset (GJBaseGameLayer::m_started stayed false).");
            return -1;
        }
        if (ticksOf(pl->m_gameState.m_currentProgress) != 0) {
            fail(fmt::format("After a reset the level is at tick {}, expected 0.", ticksOf(pl->m_gameState.m_currentProgress)));
            return -1;
        }
        pl->m_queuedButtons.clear();
        R.resetting = false;
        R.running = true;
        R.cursor = 0;
        return 0;
    }
    if (R.running) {
        Stepper st(pl);
        const uint32_t tick = st.tick();
        if (static_cast<int64_t>(tick) >= m_baseTickLimit) { repairTrialDone(static_cast<int64_t>(tick), true); return 0; }
        queueDue(st, R.trialEv, R.cursor, tick);
        std::string fault;
        const StepResult r = st.step(fault);
        if (r == StepResult::Fault) { fail(fault); return -1; }
        if (r == StepResult::Died) { repairTrialDone(static_cast<int64_t>(tick), false); return 1; }
        if (r == StepResult::Ended) { repairTrialDone(static_cast<int64_t>(tick), true); return 1; }
        return 1;
    }

    // pick the next replay to run
    for (int guard = 0; guard < 100000; ++guard) {
        if (R.trials >= 900) { finishRepair(pl); return 0; }
        size_t candIdx = 0;
        int shift = 0;
        if (R.refining) {
            candIdx = R.refCand;
            shift = R.refNext;
        } else {
            if (R.ci >= R.cand.size()) {
                // every candidate was tried this round: take the best improvement, if any, and go on from the new death
                if (R.bestCand >= 0 && R.bestReach > R.reach) {
                    repairApply(static_cast<size_t>(R.bestCand), R.bestShift, R.bestReach, R.bestEnded);
                    repairNextRound();
                    if (R.ended || R.rounds >= 40 || R.cand.empty()) { finishRepair(pl); return 0; }
                    continue;
                }
                finishRepair(pl);
                return 0;
            }
            if (R.si >= 2 * m_p.repairMaxShift) { ++R.ci; R.si = 0; continue; }
            candIdx = R.cand[R.ci];
            shift = (R.si / 2 + 1) * ((R.si & 1) ? 1 : -1);
            ++R.si;
        }
        std::vector<InputEvent> ev;
        if (!repairMakeTrial(candIdx, shift, ev)) {
            if (R.refining) {
                // cannot move further: the range ends here
                repairTrialDone(0, false);
                if (R.cand.empty()) { finishRepair(pl); return 0; }
            }
            continue;
        }
        R.trialEv = std::move(ev);
        R.curCandIdx = R.ci < R.cand.size() ? R.ci : 0;
        R.curShift = shift;
        ++R.trials;
        ++m_trialsTotal;
        m_phaseText = fmt::format("Repairing the macro: replay {} (it dies at tick {})", R.trials, R.reach);
        pl->m_queuedButtons.clear();
        pl->resetLevelFromStart();
        R.resetting = true;
        R.resetFrames = 1;
        return 0;
    }
    finishRepair(pl);
    return 0;
}

}  // namespace wp::sim
