#include "Session.hpp"

#include "Sigma.hpp"
#include "../import/MacroParser.hpp"
#include "../audio/FrameSoundData.hpp"
#include "../sim/Analyzer.hpp"
#include "../sim/Stepper.hpp"
#include "../sim/Snapshot.hpp"
#include "../export/Export.hpp"

#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <fstream>
#include <cmath>
#include <fmt/format.h>

using namespace geode::prelude;

namespace wp {

Session& Session::get() {
    static Session s;
    return s;
}

void Session::attach(PlayLayer* pl) {
    m_pl = pl;
    m_tickCalibrated = false;
    progressPerTickRef() = static_cast<uint32_t>(std::max(1, Mod::get()->getSavedValue<int>("progress-per-tick", 1)));
    {
        const int f = Mod::get()->getSavedValue<int>("fps", 240);
        tpsRef() = validTps(f) ? f : 240;
    }
    m_countsShown = ~size_t(0);
    m_rec.clear();
}

void Session::detach() {
    if (m_ffActive) { ffRestore(m_pl); m_ffActive = false; sim::SimState::get().muteAudio = false; sim::SimState::get().trialActive = false; }
    releaseRecordSpeed();
    m_pl = nullptr;
    m_recording = false;
    m_inReset = false;
    m_rec.clear();
}

static LevelInfo levelInfoOf(PlayLayer* pl) {
    LevelInfo li;
    if (pl && pl->m_level) {
        li.id = static_cast<uint32_t>(pl->m_level->m_levelID.value());
        li.platformer = pl->m_level->isPlatformer();
        li.name = pl->m_level->m_levelName;
    }
    return li;
}

std::string Session::loadMacro(std::filesystem::path const& path, std::vector<std::string>& warnings, MacroType type) {
    auto pr = loadMacroFile(path, type);
    if (!pr.ok()) return pr.error;
    warnings = pr.warnings;
    if (m_pl) {
        auto err = validateAgainstLevel(pr.macro, levelInfoOf(m_pl), warnings);
        if (!err.empty()) return err;
    }
    auto saved = pr.analysis;   // the analysis stored in a .wpmacro file, if any
    setMacro(std::make_shared<Macro const>(std::move(pr.macro)));
    if (saved && saved->inputs.size() == m_macro->inputs.size()) {
        m_result = saved;
        m_result->macroName = m_macro->name;
        auto s = computeSigma(*m_macro, *m_result, cfg().sigmaHours);
        m_result->sigmaPerSec = s.valid ? s.perSecond : 0.0;
        m_result->sigmaHours = cfg().sigmaHours;
        m_bucketFor = nullptr;
        m_soundFor = nullptr;
        warnings.push_back("The saved analysis was restored with the macro.");
    }
    return "";
}

std::vector<size_t> Session::editableInputs() const {
    std::vector<size_t> out;
    if (!m_result || !m_macro) return out;
    for (size_t i = 0; i < m_result->inputs.size() && i < m_macro->inputs.size(); ++i) {
        auto s = m_result->inputs[i].status;
        if (s == InputStatus::Measured || s == InputStatus::Capped) out.push_back(i);
    }
    return out;
}

bool Session::editWindow(size_t input, double value) {
    if (!m_result || !m_macro || input >= m_result->inputs.size()) return false;
    auto& r = m_result->inputs[input];
    if (r.status != InputStatus::Measured && r.status != InputStatus::Capped) return false;
    value = std::clamp(std::round(value * 100.0) / 100.0, 0.0, 999.0);
    if (!r.edited) { r.edited = true; r.analyzedWindow = r.window; }
    r.window = value;
    r.status = InputStatus::Measured;
    r.note = "edited by hand";
    auto s = computeSigma(*m_macro, *m_result, cfg().sigmaHours);
    m_result->sigmaPerSec = s.valid ? s.perSecond : 0.0;
    m_bucketFor = nullptr;   // the sidebar / markers / sounds re-read the windows
    m_soundFor = nullptr;
    return true;
}

bool Session::resetWindow(size_t input) {
    if (!m_result || !m_macro || input >= m_result->inputs.size()) return false;
    auto& r = m_result->inputs[input];
    if (!r.edited) return true;
    r.window = r.analyzedWindow;
    r.edited = false;
    r.note.clear();
    auto s = computeSigma(*m_macro, *m_result, cfg().sigmaHours);
    m_result->sigmaPerSec = s.valid ? s.perSecond : 0.0;
    m_bucketFor = nullptr;
    m_soundFor = nullptr;
    return true;
}

std::string Session::saveMacroFile(std::filesystem::path const& path) {
    if (!m_macro) return "There is no macro to save.";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fmt::format("Could not open {} for writing.", path.string());
    const auto text = exportWpMacroText(*m_macro, m_result.get());
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return "Writing the file failed.";
    return "";
}

void Session::setMacro(std::shared_ptr<Macro const> m) {
    m_macro = std::move(m);
    m_result.reset();
    m_bucket.clear();
    m_totals.clear();
    m_counts.clear();
    m_countsShown = ~size_t(0);
    m_bucketFor = nullptr;
    m_pbResync = true;
    m_botAttempt = false;
}

std::string Session::startAnalysis() {
    if (!m_pl) return "Open a level first.";
    if (!m_macro) return "Load a macro first.";
    lastReport.clear();
    auto& c = cfg();
    sim::AnalysisParams p;
    p.maxShift = c.maxShift;
    p.maxTrials = c.maxTrials;
    p.horizonInputs = c.horizonInputs;
    p.horizonTail = c.horizonTail;
    p.horizonMin = c.horizonMin;
    p.horizonMax = c.horizonMax;
    p.convergeTicks = c.convergeTicks;
    p.selfTest = c.fidelitySelfTest;
    p.longSelfTest = c.fidelityLongSelfTest;
    p.localStart = c.localStart;
    p.cbf = c.cbfMode;
    p.cbfSubdivisions = c.cbfSubdivisions;
    p.speedX = c.analysisSpeed;
    p.autoThrottle = c.autoThrottle;
    p.minFps = c.minFps;
    p.mute = c.muteAnalysis;
    p.keepHold = c.keepHold;
    p.exact = c.exactReplay;
    p.autoTiming = c.autoTiming;
    p.releaseMode = c.releaseTiming == "Always" ? 1 : (c.releaseTiming == "Never" ? 2 : 0);
    p.autoSubtick = c.subtickAuto;
    p.rejoinExtra = c.rejoinExtra;
    p.repair = c.autoRepair;
    p.repairMaxShift = c.repairMaxShift;
    p.verify = c.verifyBoundaries;
    p.recheck = c.recheckTight;
    p.flyStrict = c.flyStrictness;
    p.fastSearch = c.fastSearch;
    if (macroFpsMismatch())
        return fmt::format("This macro was recorded at {0} FPS, but FPS is set to {1}. Press the FPS button on the main page until it says {0} and analyze again. (Windows are only valid at the rate the macro was recorded at.)",
                           static_cast<int>(std::lround(m_macro->sourceTps)), fps());
    auto& an = sim::Analyzer::get();
    an.onFinished = [](std::shared_ptr<AnalysisResult> r) { Session::get().onAnalysisFinished(std::move(r)); };
    std::string err;
    if (!an.start(m_pl, m_macro, p, err)) return err;
    return "";
}

void Session::onAnalysisFinished(std::shared_ptr<AnalysisResult> r) {
    if (!r) return;
    r->sigmaHours = cfg().sigmaHours;
    if (m_macro) {
        auto s = computeSigma(*m_macro, *r, r->sigmaHours);
        r->sigmaPerSec = s.valid ? s.perSecond : 0.0;
    }
    bool useful = r->complete || (!r->inputs.empty() && std::any_of(r->inputs.begin(), r->inputs.end(), [](InputResult const& i) {
                      return i.status == InputStatus::Measured || i.status == InputStatus::Capped;
                  }));
    if (useful) {
        if (r->scaledMacro) {
            // the macro had to be read differently to replay (timing corrected); use that version from now on
            m_macro = r->scaledMacro;
            m_pbResync = true;
        }
        m_result = r;
        m_bucketFor = nullptr;
        m_countsShown = ~size_t(0);
    }
    lastMessage = r->complete ? fmt::format("Done: {} inputs, {} trials, {:.1f}s.", r->inputs.size(), r->totalTrials, r->elapsedSec)
                              : (r->error.empty() ? "Stopped." : r->error);
    if (useful && r->repairedInputs > 0)
        lastMessage = fmt::format("Macro repaired: {} click(s) moved so it survives. ", r->repairedInputs) + lastMessage;
    else if (useful && r->scaledMacro)
        lastMessage = fmt::format("Macro timing corrected automatically (x{:g}, {:+g} ticks). ", r->timingScale, r->timingOffset) + lastMessage;
    // Nothing measured? Say why (the unmodified run dying, inputs past the end, ...), not just "0 trials".
    lastReport.clear();
    if (r->complete && !r->cancelled) {
        const bool any = std::any_of(r->inputs.begin(), r->inputs.end(), [](InputResult const& i) {
            return i.status == InputStatus::Measured || i.status == InputStatus::Capped;
        });
        if (!any) {
            std::vector<std::string> why;
            auto add = [&](std::string const& t) {
                if (!t.empty() && std::find(why.begin(), why.end(), t) == why.end() && why.size() < 4) why.push_back(t);
            };
            for (auto const& w : r->warnings) add(w);
            for (auto const& i : r->inputs) add(i.note);
            for (auto const& t : why) lastReport += (lastReport.empty() ? "" : "\n") + t;
            if (lastReport.empty()) lastReport = "No input could be measured.";
            lastMessage += " Nothing measured: " + (why.empty() ? lastReport : why.front());
        }
    }
    if (r->complete && cfg().exportOnFinish) {
        std::filesystem::path p;
        auto e = saveJson(p);
        if (!e.empty()) log::warn("[Windowpane] export failed: {}", e);
    }
}

void Session::refresh() {
    auto& c = cfg();
    if (!m_result) return;
    if (m_bucketFor == m_result.get() && m_cfgRev == c.revision) return;
    m_bucketFor = m_result.get();
    m_cfgRev = c.revision;
    m_bucket.assign(m_result->inputs.size(), -1);
    m_totals.assign(c.buckets.size(), 0);
    for (size_t i = 0; i < m_result->inputs.size(); ++i) {
        auto const& r = m_result->inputs[i];
        if (r.status != InputStatus::Measured && r.status != InputStatus::Capped) continue;
        int b = bucketIndexFor(c, r.window);
        m_bucket[i] = b;
        if (b >= 0) ++m_totals[b];
    }
    m_countsShown = ~size_t(0);
}

size_t Session::inputsUpTo(int64_t tick) const {
    if (!m_macro) return 0;
    auto const& ev = m_macro->inputs;
    auto it = std::upper_bound(ev.begin(), ev.end(), tick, [](int64_t t, InputEvent const& e) { return t < e.tick; });
    return static_cast<size_t>(it - ev.begin());
}

std::vector<int> const& Session::countsAt(size_t n) {
    refresh();
    if (n == m_countsShown && m_counts.size() == m_totals.size()) return m_counts;
    m_counts.assign(m_totals.size(), 0);
    for (size_t i = 0; i < n && i < m_bucket.size(); ++i)
        if (m_bucket[i] >= 0) ++m_counts[m_bucket[i]];
    m_countsShown = n;
    return m_counts;
}

int Session::fps() const { return static_cast<int>(std::lround(tps())); }

void Session::cycleFps() {
    const int v = nextTps(fps());
    tpsRef() = v;
    Mod::get()->setSavedValue<int>("fps", v);
    m_tickCalibrated = false;     // how the game counts ticks has to be measured again at the new rate
    m_pbResync = true;
    m_measTps = 0.0;              // the old measurement belongs to the old rate
    m_stepCount = 0;
}

bool Session::macroFpsMismatch() const {
    return m_macro && std::abs(m_macro->sourceTps - tps()) > 0.5;
}

void Session::noteStep(float timeWarp) {
    using clk = std::chrono::steady_clock;
    const auto now = clk::now();
    auto secs = [](clk::duration d) { return std::chrono::duration<double>(d).count(); };
    // a gap (pause, loading, a lag spike) starts a new window
    if (m_stepCount == 0 || secs(now - m_stepLast) > 0.25) {
        m_stepStart = now;
        m_stepCount = 0;
    }
    m_stepLast = now;
    ++m_stepCount;
    const double el = secs(now - m_stepStart);
    if (el < 0.6) return;
    // (count - 1) steps happened between the first and the last counted step. The game may run slowed down (time warp) or
    // at the recording speed; what the TPS mod sets is the rate at normal speed.
    const double warp = timeWarp > 0.f ? static_cast<double>(timeWarp) : 1.0;
    const double speed = m_recording ? std::max(0.05, recordSpeed()) : 1.0;
    m_measTps = static_cast<double>(m_stepCount - 1) / el / (warp * speed);
    m_stepStart = now;
    m_stepCount = 0;
    if (m_verifyTps) {
        m_verifyTps = false;
        if (m_recording && std::abs(m_measTps / tps() - 1.0) > 0.10) cancelRecordingForTps(m_measTps);
    }
}

void Session::cancelRecordingForTps(double measured) {
    m_recording = false;
    m_rec.clear();
    m_carryCheck = false;
    // closest rate the FPS button can be set to, if the game is clearly at one of them
    int closestRate = 0;
    for (int v : {240, 320, 480, 720, 960}) if (std::abs(measured / v - 1.0) <= 0.10) closestRate = v;
    const int setTo = fps();
    std::string text = fmt::format("The game runs at about {:.0f} TPS, but FPS is set to {}. A recording is only valid when the two match, so it was cancelled.\n\n"
                                   "Set the TPS in your TPS mod (Eclipse's TPS bypass or similar) to {}{}, then press Record again. "
                                   "If it already says {}, your PC may not be able to keep up with {} TPS in this level.",
                                   measured, setTo, setTo,
                                   closestRate ? fmt::format(", or press the FPS button until it says {}", closestRate) : std::string(), setTo, setTo);
    Loader::get()->queueInMainThread([text] {
        if (auto* pl = PlayLayer::get()) if (!pl->m_isPaused) pl->pauseGame(false);
        FLAlertLayer::create("TPS does not match", text, "OK")->show();
    });
}

std::string Session::startRecording() {
    if (!m_pl) return "Open a level first.";
    if (m_pl->m_level && m_pl->m_level->isPlatformer()) return "Recording is only supported for classic levels.";
    // Recording only works in practice mode, so switch it on for the player (same call the pause menu's button makes).
    if (!m_pl->m_isPracticeMode) {
        m_pl->togglePracticeMode(true);
        if (!m_pl->m_isPracticeMode)
            return "Recording works in practice mode, and it could not be switched on automatically. Turn on practice mode (the checkpoint button), then press Record again.";
    }
    if (!m_tickCalibrated) {
        // Find out how the game counts ticks before stamping any clicks (only needed once per level entry).
        sim::Snapshot snap;
        if (snap.capture(m_pl)) {
            auto& S = sim::SimState::get();
            const bool was = S.trialActive;
            S.trialActive = true;   // the probe steps must not kill the player or finish the level for real
            auto err = sim::calibrateTickRate(m_pl, snap);
            S.trialActive = was;
            S.clearOutcome();
            if (!err.empty()) return err;
            m_tickCalibrated = true;
        }
    }
    m_rec.clear();
    std::fill(&m_held[0][0], &m_held[0][0] + 8, false);
    m_carryCheck = false;
    m_recording = true;
    // Always record from the very start: drop any practice checkpoint that was already placed and restart the level, so the
    // recording begins at 0.00% and is one run from tick 0 (the reset hook sees the recording and clears the macro).
    clearPracticeCheckpoints(m_pl);
    m_pl->resetLevelFromStart();
    m_stepCount = 0;
    m_verifyTps = true;     // the first 0.6 s of play decide whether the game's tick rate matches the FPS that was set
    return "";
}

void Session::recordInput(int button, bool down, bool p2, int64_t tick) {
    if (!m_recording || m_inReset || button < 1 || button > 3) return;
    // A press while already held (or a release while up) changes nothing. This is what the game's automatic
    // re-press after a respawn looks like, so a button you keep holding across an attempt stays ONE hold.
    bool& h = m_held[p2 ? 1 : 0][button];
    if (h == down) return;
    h = down;
    InputEvent e;
    e.tick = tick;
    e.button = static_cast<Button>(button);
    e.down = down;
    e.p2 = p2;
    m_rec.push_back(e);
}

void Session::beginLevelReset(PlayLayer* pl) {
    m_inReset = true;
    if (!m_recording) return;
    // The tick we respawn at: the last practice checkpoint (exact), or 0 for a full restart.
    int64_t tick = 0;
    if (pl && pl->m_isPracticeMode && pl->m_checkpointArray && pl->m_checkpointArray->count() > 0)
        if (auto* cp = static_cast<CheckpointObject*>(pl->m_checkpointArray->lastObject()))
            tick = static_cast<int64_t>(ticksOf(cp->m_gameState.m_currentProgress));
    m_carryCheck = false;
    std::fill(&m_held[0][0], &m_held[0][0] + 8, false);
    if (tick <= 0) { m_rec.clear(); return; }
    m_rec.erase(std::remove_if(m_rec.begin(), m_rec.end(), [tick](InputEvent const& e) { return e.tick >= tick; }), m_rec.end());
    // button state at the checkpoint, from the inputs that were kept
    for (auto const& e : m_rec) m_held[e.p2 ? 1 : 0][static_cast<int>(e.button)] = e.down;
    m_carryCheck = true;
    m_carryTick = tick;
}

int64_t Session::lastCheckpointTick(PlayLayer* pl) const {
    if (pl && pl->m_isPracticeMode && pl->m_checkpointArray && pl->m_checkpointArray->count() > 0)
        if (auto* cp = static_cast<CheckpointObject*>(pl->m_checkpointArray->lastObject()))
            return static_cast<int64_t>(ticksOf(cp->m_gameState.m_currentProgress));
    return 0;
}

void Session::clearPracticeCheckpoints(PlayLayer* pl) {
    if (!pl || !pl->m_checkpointArray) return;
    if (pl->m_checkpointArray->count() > 0) {
        pl->removeAllCheckpoints();
        // Whatever the game's own call left in the list is removed one by one the same way (never by emptying the array).
        for (unsigned guard = pl->m_checkpointArray->count(); guard > 0 && pl->m_checkpointArray->count() > 0; --guard) {
            const unsigned before = pl->m_checkpointArray->count();
            pl->removeCheckpoint(false);
            if (pl->m_checkpointArray->count() >= before) break;
        }
    }
    // PlayLayer::m_currentCheckpoint is a plain pointer: it must not outlive the checkpoint it points to.
    if (pl->m_checkpointArray->count() == 0) pl->m_currentCheckpoint = nullptr;
}

void Session::beginExactRespawn(PlayLayer* pl, int64_t target) {
    m_ffActive = true;
    m_ffPhase = 0;
    m_ffFrames = 0;
    m_ffTarget = target;
    m_ffCursor = 0;
    m_ffShown.clear();
    m_ffEvents = m_rec;   // beginLevelReset already dropped everything from the checkpoint on
    std::stable_sort(m_ffEvents.begin(), m_ffEvents.end(), [](InputEvent const& a, InputEvent const& b) { return a.tick < b.tick; });
    // The game restarts from the checkpoint list it sees: give it an empty one so it starts from the very beginning.
    m_ffSaved = pl->m_checkpointArray;
    m_ffCurrent = pl->m_currentCheckpoint;
    m_ffTemp = CCArray::create();
    m_ffTemp->retain();
    pl->m_checkpointArray = m_ffTemp;
    pl->m_currentCheckpoint = nullptr;
    // The rebuild (restart + fast-forward) must not be seen: hide the level objects and players (the background stays, so no
    // black screen) until the checkpoint is reached, then show the level at the checkpoint in one go.
    m_ffHideAll = cfg().rebuildHideScreen;
    if (m_ffHideAll) {
        // beta 2 behaviour: the whole level and its UI are hidden while it is rebuilt
        pl->setVisible(false);
        if (pl->m_uiLayer) pl->m_uiLayer->setVisible(false);
        m_ffHidden = true;
    } else if (pl->m_objectLayer) {
        pl->m_objectLayer->setVisible(false);
        m_ffHidden = true;
    }
}

void Session::ffRestore(PlayLayer* pl) {
    if (pl && m_ffTemp && pl->m_checkpointArray == m_ffTemp) {
        // While the level was rebuilt the game may have dropped checkpoints of its own into the temporary list (practice
        // auto-checkpoints). Their level objects are in the level, so they have to go the game's way before the list is released;
        // releasing the list with them in it left objects behind that point at deleted checkpoints.
        for (unsigned guard = m_ffTemp->count(); guard > 0 && m_ffTemp->count() > 0; --guard) {
            const unsigned before = m_ffTemp->count();
            pl->removeCheckpoint(false);
            if (m_ffTemp->count() >= before) break;
        }
        pl->m_currentCheckpoint = nullptr;
        if (m_ffSaved) pl->m_checkpointArray = m_ffSaved;
        pl->m_currentCheckpoint = m_ffCurrent;
    }
    if (m_ffTemp) {
        // Never free a list that still holds checkpoints the game knows about: leaking it is harmless, a dangling one is not.
        if (m_ffTemp->count() == 0) m_ffTemp->release();
        m_ffTemp = nullptr;
    }
    m_ffCurrent = nullptr;
    m_ffSaved = nullptr;
    if (m_ffLabel) { m_ffLabel->removeFromParent(); m_ffLabel = nullptr; }
    if (pl && m_ffHidden) {
        if (pl->m_objectLayer) pl->m_objectLayer->setVisible(true);
        pl->setVisible(true);                                  // also undoes the whole-screen hide of an earlier setting value
        if (pl->m_uiLayer) pl->m_uiLayer->setVisible(true);
    }
    m_ffHidden = false;
    m_ffHideAll = false;
}

void Session::ffAbort(PlayLayer* pl, std::string const& why) {
    auto& S = sim::SimState::get();
    S.trialActive = false;
    S.muteAudio = false;
    S.clearOutcome();
    ffRestore(pl);
    m_ffActive = false;
    // The exact rebuild did not work out (typically the replayed run dies before the checkpoint, which happens on hard levels with
    // many attempts). That must not throw the recording away: fall back to what the game itself does in practice mode, load the
    // last checkpoint, and keep recording. The recorded clicks up to the checkpoint are kept (beginLevelReset already cut everything
    // after it), the player state the game's checkpoint leaves out is put back by the loadFromCheckpoint hook.
    if (pl && pl->m_checkpointArray && pl->m_checkpointArray->count() > 0) {
        if (auto* cp = static_cast<CheckpointObject*>(pl->m_checkpointArray->lastObject())) {
            pl->m_queuedButtons.clear();
            pl->loadFromCheckpoint(cp);
            pl->m_queuedButtons.clear();
            if (auto* fmod = FMODAudioEngine::sharedEngine()) {
                const double offset = pl->m_levelSettings ? static_cast<double>(pl->m_levelSettings->m_songOffset) : 0.0;
                const double sec = static_cast<double>(ticksOf(pl->m_gameState.m_currentProgress)) / tps() + offset;
                fmod->setMusicTimeMS(static_cast<unsigned>(std::max(0.0, sec) * 1000.0), true, 0);
            }
            m_pbResync = true;
            log::warn("[Windowpane] exact checkpoint rebuild failed ({}); respawned at the checkpoint the normal way, recording continues", why);
            Notification::create("Respawned at the checkpoint the normal way; recording continues.", NotificationIcon::Info)->show();
            return;
        }
    }
    m_recording = false;
    m_rec.clear();
    m_carryCheck = false;
    const std::string text = fmt::format("The checkpoint could not be rebuilt exactly ({}). The recording was stopped, nothing was stored. "
                                         "You can turn off \"Recording: respawn at practice checkpoints\" to record a straight run instead.", why);
    Loader::get()->queueInMainThread([text] {
        if (auto* p = PlayLayer::get()) if (!p->m_isPaused) p->pauseGame(false);
        FLAlertLayer::create("Recording stopped", text, "OK")->show();
    });
}

bool Session::ffUpdate(PlayLayer* pl) {
    auto& S = sim::SimState::get();
    if (!m_ffActive) return false;
    if (!pl || pl != m_pl) { m_ffActive = false; return false; }
    if (m_ffPhase == 0) {
        ++m_ffFrames;
        if (!pl->m_started) {
            if (m_ffFrames == 4) pl->startGame();
            else if (m_ffFrames > 240) ffAbort(pl, "the level did not start again after the restart");
            return true;
        }
        if (ticksOf(pl->m_gameState.m_currentProgress) != 0) {
            ffAbort(pl, fmt::format("after the restart the level was already at tick {}", ticksOf(pl->m_gameState.m_currentProgress)));
            return true;
        }
        m_ffPhase = 1;
    }
    // phase 1: advance with the recorded clicks, as long as the frame budget allows
    const auto t0 = std::chrono::steady_clock::now();
    sim::Stepper st(pl);
    std::string fault;
    const bool wasTrial = S.trialActive;
    S.trialActive = true;     // the replayed run must not kill the player or finish the level for real, and the recorder stays out
    S.muteAudio = true;
    bool done = false, failed = false;
    while (true) {
        const int64_t tick = static_cast<int64_t>(ticksOf(pl->m_gameState.m_currentProgress));
        if (tick >= m_ffTarget) { done = true; break; }
        while (m_ffCursor < m_ffEvents.size() && m_ffEvents[m_ffCursor].tick <= tick) st.queueInput(m_ffEvents[m_ffCursor++], false);
        const auto r = st.step(fault);
        if (r != sim::StepResult::Ok) {
            failed = true;
            if (fault.empty()) fault = r == sim::StepResult::Died ? "the replayed run died before the checkpoint" : "the replayed run ended before the checkpoint";
            break;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > 0.025) break;
    }
    S.trialActive = wasTrial;
    S.muteAudio = false;
    S.clearOutcome();
    if (failed) { ffAbort(pl, fault); return true; }
    if (done) {
        pl->m_queuedButtons.clear();
        ffRestore(pl);
        m_ffActive = false;
        // The restart played the song from its start (and the fast-forward is muted): put the music where the level now is.
        if (auto* fmod = FMODAudioEngine::sharedEngine()) {
            const double offset = pl->m_levelSettings ? static_cast<double>(pl->m_levelSettings->m_songOffset) : 0.0;
            const double sec = static_cast<double>(ticksOf(pl->m_gameState.m_currentProgress)) / tps() + offset;
            fmod->setMusicTimeMS(static_cast<unsigned>(std::max(0.0, sec) * 1000.0), true, 0);
        }
        m_pbResync = true;
        return true;
    }
    return true;
}

void Session::endLevelReset(PlayLayer*) {
    m_inReset = false;
    m_pbResync = true;       // the progress is final only at the next command pass; resync there
    m_botAttempt = false;
}

// ---- playback ------------------------------------------------------------------------------------------
bool Session::playbackOn() const { return Mod::get()->getSavedValue<bool>("playback", true); }

bool Session::togglePlayback() {
    const bool v = !playbackOn();
    Mod::get()->setSavedValue<bool>("playback", v);
    m_pbResync = true;
    return v;
}

// The sounds are compiled into the mod (audio/FrameSoundData.cpp). They are written once to the mod's save folder and
// played from there, so nothing depends on how the resources folder was packaged.
static std::string frameSoundFile(int n) {
    static std::string cache[10];
    if (n < 1 || n > 9) return {};
    if (!cache[n].empty()) return cache[n];
    auto bytes = frameSoundBytes(n);
    if (!bytes.data) return {};
    std::error_code ec;
    const auto dir = Mod::get()->getSaveDir() / "sounds";
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / fmt::format("frame-{}.wav", n);
    bool need = true;
    if (std::filesystem::exists(file, ec)) need = std::filesystem::file_size(file, ec) != bytes.size;
    if (need) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            log::warn("[Windowpane] could not write {}", file.string());
            return {};
        }
        out.write(reinterpret_cast<char const*>(bytes.data), static_cast<std::streamsize>(bytes.size));
    }
    return cache[n] = file.string();
}

void Session::playFrameSoundN(int n) {
    n = std::clamp(n, 1, 9);
    auto* eng = FMODAudioEngine::sharedEngine();
    if (!eng) return;
    const auto file = frameSoundFile(n);
    if (file.empty()) {
        static bool warned = false;
        if (!warned) { warned = true; log::warn("[Windowpane] frame sound {} is not available", n); }
        return;
    }
    const float vol = static_cast<float>(cfg().frameSoundVolume);
    // Straight through FMOD: load once, then play on a fresh channel with our own volume.
    static FMOD::Sound* sounds[10] = {};
    if (eng->m_system) {
        FMOD::Sound*& snd = sounds[n];
        if (!snd) {
            auto res = eng->m_system->createSound(file.c_str(), FMOD_DEFAULT | FMOD_CREATESAMPLE, nullptr, &snd);
            if (res != FMOD_OK) {
                snd = nullptr;
                log::warn("[Windowpane] FMOD could not load {} (error {}); falling back to the game's effect player", file, static_cast<int>(res));
            }
        }
        if (snd) {
            FMOD::Channel* ch = nullptr;
            if (eng->m_system->playSound(snd, nullptr, false, &ch) == FMOD_OK && ch) {
                ch->setPriority(0);   // highest: many clicks overlap, none may be stolen
                ch->setVolume(std::clamp(vol, 0.f, 2.f) * std::max(0.15f, eng->m_sfxVolume));   // never fully silent unless the volume setting is 0
                return;
            }
        }
    }
    eng->playEffect(file, 1.f, 0.f, vol);
}

void Session::playFrameSound(size_t eventIndex) {
    auto& c = cfg();
    if (!c.frameSounds || !m_result || !m_macro) return;
    if (m_soundFor != m_result.get()) {
        m_soundFor = m_result.get();
        m_soundWindow.assign(m_macro->inputs.size(), -1.f);   // -1 = not measured, no sound
        for (auto const& r : m_result->inputs)
            if ((r.status == InputStatus::Measured || r.status == InputStatus::Capped) && r.index >= 0 &&
                static_cast<size_t>(r.index) < m_soundWindow.size())
                m_soundWindow[r.index] = static_cast<float>(r.window);
    }
    if (eventIndex >= m_soundWindow.size() || eventIndex >= m_macro->inputs.size()) return;
    const float w = m_soundWindow[eventIndex];
    if (w < 0.f) return;
    // Only clicks (presses) make a sound; releases only when release timings are set to "Always".
    if (!m_macro->inputs[eventIndex].down && c.releaseTiming != "Always") return;
    // Only windows that are among the "Timings to show".
    refresh();
    if (bucketOf(eventIndex) < 0) return;
    // The sounds are one step up: a window of 0 plays the 1 frame sound, 1 plays frame 2, ... (the top sound is shared by the rest).
    const int n = std::clamp(static_cast<int>(std::floor(w + 1e-4f)) + 1, 1, 9);
    playFrameSoundN(n);
}

void Session::injectPlayback(GJBaseGameLayer* gl) {
    if (!gl || !m_macro || m_recording || !playbackOn()) return;
    if (std::abs(m_macro->sourceTps - tps()) > 0.5) return;   // a macro at another tick rate would play at the wrong ticks
    if (gl->m_level && gl->m_level->isPlatformer()) return;
    auto const& ev = m_macro->inputs;
    const int64_t tick = static_cast<int64_t>(ticksOf(gl->m_gameState.m_currentProgress));

    // A respawn / restart / jump: inputs before this tick are already part of the restored state, the rest still play.
    // (A lag spike can advance one frame by well over 16 ticks; treating that as a restart skipped the clicks and sounds inside it.)
    if (m_pbResync || tick < m_pbLast || tick > m_pbLast + 240) {
        m_pbResync = false;
        m_soundFor = nullptr;   // rebuild the per-input sound table (the analysis or its windows may have changed)
        m_pbPassTick = -1;
        m_pbPassCount = 0;
        auto it = std::lower_bound(ev.begin(), ev.end(), tick, [](InputEvent const& e, int64_t t) { return e.tick < t; });
        m_pbCursor = static_cast<size_t>(it - ev.begin());
        if (tick > 0) {
            // make the held state agree with what the macro says it was just before this tick
            bool want[2] = {false, false};
            for (size_t i = 0; i < m_pbCursor; ++i)
                if (ev[i].button == Button::Jump) want[ev[i].p2 ? 1 : 0] = ev[i].down;
            for (int p = 0; p < 2; ++p) {
                PlayerObject* po = p ? gl->m_player2 : gl->m_player1;
                if (!po) continue;
                auto h = po->m_holdingButtons.find(1);
                const bool holding = h != po->m_holdingButtons.end() && h->second;
                if (holding != want[p]) gl->queueButton(1, want[p], p == 1, 0.0);
            }
        }
    }
    m_pbLast = tick;
    // 0 = first command pass OF THIS TICK, 1 = second (half-tick clicks go here). One game update can run several ticks
    // at normal FPS, so the pass has to be counted per tick, not per update (counting per update made half-tick clicks
    // land half a tick early or a whole tick late).
    if (tick != m_pbPassTick) { m_pbPassTick = tick; m_pbPassCount = 0; }
    const int pass = m_pbPassCount++;
    while (m_pbCursor < ev.size() && ev[m_pbCursor].tick <= tick) {
        if (progressPerTick() > 1 && ev[m_pbCursor].sub >= 0.25 && ev[m_pbCursor].tick == tick && pass == 0) break;   // wait for the second pass
        auto const& e = ev[m_pbCursor++];
        gl->queueButton(static_cast<int>(e.button), e.down, e.p2, 0.0);
        m_botAttempt = true;
        playFrameSound(m_pbCursor - 1);
    }
}

void Session::recordCarryCheck(GJBaseGameLayer* gl) {
    if (!m_recording || !m_carryCheck || !gl) return;
    const int64_t tick = static_cast<int64_t>(ticksOf(gl->m_gameState.m_currentProgress));
    if (tick < m_carryTick + 2) return;
    m_carryCheck = false;
    // A couple of ticks after the respawn: make the recording agree with what the player is really doing.
    //  - recorded "held" but the button is up (it was released while dead): close the hold at the checkpoint
    //  - recorded "up" but the button is down (you held it into the attempt): open the hold at the checkpoint
    for (bool p2 : {false, true}) {
        PlayerObject* po = p2 ? gl->m_player2 : gl->m_player1;
        if (!po) continue;
        auto it = po->m_holdingButtons.find(1);
        const bool holding = it != po->m_holdingButtons.end() && it->second;
        bool& h = m_held[p2 ? 1 : 0][1];
        if (holding == h) continue;
        InputEvent e;
        e.tick = m_carryTick;
        e.button = Button::Jump;
        e.down = holding;
        e.p2 = p2;
        m_rec.push_back(e);
        h = holding;
    }
}

// ---- recording speed -----------------------------------------------------------------------------------
double Session::recordSpeed() const {
    return std::clamp(Mod::get()->getSavedValue<double>("record-speed", 1.0), 0.05, 1.0);
}

double Session::cycleRecordSpeed() {
    static constexpr double steps[] = {1.0, 0.5, 0.25, 0.1};
    const double cur = recordSpeed();
    size_t at = 0;
    for (size_t i = 0; i < 4; ++i) if (std::abs(cur - steps[i]) < 1e-6) at = i;
    const double next = steps[(at + 1) % 4];
    Mod::get()->setSavedValue<double>("record-speed", next);
    return next;
}

static void setMusicPitch(float p) {
    if (auto* fmod = FMODAudioEngine::sharedEngine())
        if (fmod->m_backgroundMusicChannel) fmod->m_backgroundMusicChannel->setPitch(p);
}

void Session::releaseRecordSpeed() {
    if (!m_slowActive) return;
    CCDirector::get()->getScheduler()->setTimeScale(m_slowSaved);
    setMusicPitch(1.f);
    m_slowActive = false;
}

void Session::updateRecordSpeed() {
    const float speed = static_cast<float>(recordSpeed());
    // Practice mode only (a slowed run could otherwise save real progress), never while the analyzer drives time.
    const bool want = m_recording && m_pl && m_pl->m_isPracticeMode && !sim::Analyzer::get().active() && speed < 0.999f;
    if (!want) { releaseRecordSpeed(); return; }
    auto* sch = CCDirector::get()->getScheduler();
    if (!m_slowActive) { m_slowSaved = sch->getTimeScale(); m_slowActive = true; }
    if (std::abs(sch->getTimeScale() - speed) > 0.001f) sch->setTimeScale(speed);
    setMusicPitch(speed);
}

std::string Session::stopRecording() {
    m_recording = false;
    if (m_rec.empty()) return "No inputs were recorded.";
    ParseResult pr;
    pr.macro.name = "recording";
    pr.macro.format = "recording";
    pr.macro.sourceTps = tps();   // the macro is in this tick rate; it can only be analysed at it
    pr.macro.platformer = m_pl && m_pl->m_level && m_pl->m_level->isPlatformer();
    if (m_pl && m_pl->m_level) {
        pr.macro.levelId = static_cast<uint32_t>(m_pl->m_level->m_levelID.value());
        pr.macro.levelName = m_pl->m_level->m_levelName;
    }
    pr.macro.inputs = std::move(m_rec);
    m_rec.clear();
    finalizeMacro(pr);
    if (!pr.ok()) return pr.error;
    setMacro(std::make_shared<Macro const>(std::move(pr.macro)));
    return "";
}

std::string Session::saveJson(std::filesystem::path& outPath) {
    if (!m_result || !m_macro) return "There is no result to save yet.";
    return exportJson(*m_macro, *m_result, outPath);
}

std::string Session::summaryText() const {
    if (!m_result || !m_macro) return "";
    return exportSummary(*m_macro, *m_result);
}

}  // namespace wp
