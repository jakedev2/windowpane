// All game hooks in one place. Each hook is small; the heavy lifting lives in sim/ and ui/.
#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <vector>

#include "../core/Cfg.hpp"
#include "../core/Session.hpp"
#include "../sim/Analyzer.hpp"
#include "../sim/PlayerExtra.hpp"
#include "../sim/SimState.hpp"
#include "../ui/Ui.hpp"

using namespace geode::prelude;

namespace wp {
void applyFrameLimiter(bool inLevel);   // main.cpp
}

namespace {
// After a practice respawn while recording: the button state of the checkpoint is what the attempt must start with. When the button was
// up at the checkpoint but is held now (you held into the attempt), the game's automatic re-press would make the player jump / change
// its velocity compared with the moment the checkpoint was placed. The hold is released right after the respawn and once more before the
// first update, so the attempt starts exactly like the checkpoint; a click after that works as usual.
bool g_relP1 = false, g_relP2 = false;
bool holdingJump(PlayerObject* p) {
    if (!p) return false;
    auto it = p->m_holdingButtons.find(1);
    return it != p->m_holdingButtons.end() && it->second;
}
}

// ---- time ownership + live input recorder ------------------------------------------------------------------
class $modify(WPBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto& S = wp::sim::SimState::get();
        // A Stepper-driven update is physics only. Recording / playback bookkeeping cannot observe a real user action here,
        // and running it millions of times was pure overhead. The normal scheduler path still performs all of it below.
        if (S.stepping) {
            GJBaseGameLayer::update(dt);
            return;
        }
        // Exact checkpoint respawn while recording: the fast-forward drives the level itself, the game's own update waits.
        {
            auto& ses = wp::Session::get();
            if (ses.ffActive() && !wp::sim::SimState::get().stepping && ses.pl() && static_cast<GJBaseGameLayer*>(ses.pl()) == this) {
                if (ses.ffUpdate(ses.pl())) return;
            }
        }
        // Recording helpers (practice-mode slow-down, hold carry-over after a checkpoint respawn).
        {
            auto& ses = wp::Session::get();
            if ((g_relP1 || g_relP2) && ses.recording() && ses.pl() && static_cast<GJBaseGameLayer*>(ses.pl()) == this) {
                if (g_relP1 && holdingJump(m_player1)) this->handleButton(false, 1, true);
                if (g_relP2 && holdingJump(m_player2)) this->handleButton(false, 1, false);
                m_queuedButtons.clear();
            }
            g_relP1 = g_relP2 = false;
            ses.beginUpdatePasses();
            if (ses.pl() && static_cast<GJBaseGameLayer*>(ses.pl()) == this) {
                ses.updateRecordSpeed();
                ses.recordCarryCheck(this);
            }
        }
        // While an analysis runs, the analyzer decides when physics advances. It calls update() itself
        // with SimState::stepping set; any other update (the game's own scheduler) is swallowed so the two
        // never interleave.
        if (wp::sim::Analyzer::get().active()) return;
        GJBaseGameLayer::update(dt);
    }

    // Playback: the loaded macro plays itself in the live level. Inputs are queued right before the command pass of
    // their tick, which is exactly what the analyzer's stepper does, so what you watch is what was measured.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& S = wp::sim::SimState::get();
        if (S.stepping) {
            // Only the analyzer's half-pass input delivery belongs here. Live recording, playback, sounds and FPS
            // measurement must not run for simulated commands; skipping their checks also keeps the hot loop lean.
            if (S.halfPassMode) {
                if (S.passInUpdate == 1 && !S.secondPass.empty()) {
                    for (auto const& q : S.secondPass) this->queueButton(q.button, q.down, q.p2, 0.0);
                    S.secondPass.clear();
                }
                ++S.passInUpdate;
            }
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }
        auto& ses = wp::Session::get();
        const bool mine = ses.pl() && static_cast<GJBaseGameLayer*>(ses.pl()) == this;
        if (mine) ses.enterPass(wp::ticksOf(m_gameState.m_currentProgress));
        if (mine && !S.trialActive && !wp::sim::Analyzer::get().active()) {
            if (!isHalfTick) ses.noteStep(m_gameState.m_timeWarp);   // measures the real physics tick rate (see Session::noteStep)
            ses.injectPlayback(this);
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (mine) ses.leavePass();
    }

    // Exactly one physics step per stepper update, whatever tick rate another mod (Eclipse's TPS bypass, ...) set up.
    // Without this the game could split one update into two steps ("one update advanced 2 physics steps").
    double getModifiedDelta(float dt) {
        auto& S = wp::sim::SimState::get();
        if (S.stepping && S.forceDelta) {
            float warp = std::min(m_gameState.m_timeWarp, 1.f);
            if (!(warp > 0.f)) warp = 1.f;
            return static_cast<double>(warp) / wp::tps() * static_cast<double>(S.dtScale);
        }
        return GJBaseGameLayer::getModifiedDelta(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& S = wp::sim::SimState::get();
        auto& ses = wp::Session::get();
        if (!S.trialActive && ses.recording() && static_cast<GJBaseGameLayer*>(ses.pl()) == this && ses.pl()->m_isPracticeMode) {
            ses.recordInput(button, down, !isPlayer1, ses.stampFor(wp::ticksOf(m_gameState.m_currentProgress)));
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// ---- level lifetime, death / completion interception ----------------------------------------------------
namespace {
// The practice fix: the game's own checkpoint forgets the players' vertical velocity. One entry per practice checkpoint, saved when the
// game creates it and written back right after the game's own respawn (the same small fix zBot uses). Everything else is left to the game.
struct CpVel { double y1 = 0, y2 = 0; bool hold1 = false, hold2 = false; };
std::vector<CpVel> g_cpVel;
bool analysing() { return wp::sim::SimState::get().trialActive || wp::sim::Analyzer::get().active(); }
}

class $modify(WPPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_cpVel.clear();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    CheckpointObject* createCheckpoint() {
        if (!analysing() && m_player1) {
            CpVel v;
            v.y1 = m_player1->m_yVelocity;
            v.y2 = m_player2 ? m_player2->m_yVelocity : 0.0;
            v.hold1 = holdingJump(m_player1);
            v.hold2 = holdingJump(m_player2);
            g_cpVel.push_back(v);
        }
        return PlayLayer::createCheckpoint();
    }

    void removeCheckpoint(bool p0) {
        if (!analysing() && !g_cpVel.empty()) g_cpVel.pop_back();
        PlayLayer::removeCheckpoint(p0);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        wp::Session::get().attach(this);
        wp::ui::attach(this);
        wp::applyFrameLimiter(true);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        // postUpdate runs once per physics step; the analyzer performs thousands, so keep it out of there.
        if (!wp::sim::SimState::get().trialActive) {
            wp::ui::frame(this);
            // The practice checkpoint markers are ordinary level objects. Make sure none of them stays hidden after the mod rebuilt the
            // level (respawn, analysis): show every marker of a checkpoint that still exists.
            if (this->m_isPracticeMode && this->m_checkpointArray && !wp::Session::get().ffActive()) {
                for (auto* o : CCArrayExt<CheckpointObject*>(this->m_checkpointArray)) {
                    auto* g = o ? o->m_physicalCheckpointObject : nullptr;
                    if (g && g->getParent() && !g->isVisible()) g->setVisible(true);
                }
            }
        }
    }

    void resetLevel() {
        auto& ses = wp::Session::get();
        // "Recording: respawn at practice checkpoints" off = straight-run recording: checkpoints are removed, every death restarts at 0%.
        // 2.5.3: practice checkpoints are the game's own again. The old "exact respawn" (restart the level, swap the checkpoint list, fast-forward
        // the recorded clicks to the checkpoint) made the player jump between checkpoints and flicker on death, so it is no longer used:
        // the game loads its checkpoint itself and the recording is simply cut back to that checkpoint's tick (beginLevelReset).
        if (!wp::sim::SimState::get().trialActive && ses.recording() && this->m_isPracticeMode && this->m_checkpointArray) {
            if (!wp::cfg().recordStitch) wp::Session::clearPracticeCheckpoints(this);
        }
        if (!wp::sim::SimState::get().trialActive) ses.beginLevelReset(this);   // drop what comes after the checkpoint
        PlayLayer::resetLevel();
        if (!analysing() && this->m_isPracticeMode && this->m_checkpointArray && m_player1) {
            // keep the list in step with the game's own list, then put the vertical velocity of the checkpoint we respawned at back
            const size_t n = this->m_checkpointArray->count();
            while (g_cpVel.size() > n) g_cpVel.pop_back();
            if (n > 0 && g_cpVel.size() == n) {
                auto const& c = g_cpVel.back();
                if (ses.recording()) {
                    // button up at the checkpoint, held now: start the attempt with the button up (see g_relP1)
                    if (!c.hold1 && holdingJump(m_player1)) this->handleButton(false, 1, true);
                    if (m_player2 && !c.hold2 && holdingJump(m_player2)) this->handleButton(false, 1, false);
                    this->m_queuedButtons.clear();
                    g_relP1 = !c.hold1;
                    g_relP2 = !c.hold2 && m_player2;
                }
                m_player1->m_yVelocity = c.y1;
                if (m_player2) m_player2->m_yVelocity = c.y2;
            }
            if (ses.recording()) {
                m_player1->m_isDashing = false;
                if (m_player2) m_player2->m_isDashing = false;
            }
        }
        if (!wp::sim::SimState::get().trialActive) ses.endLevelReset(this);
    }

    void togglePracticeMode(bool on) {
        g_cpVel.clear();
        auto& ses = wp::Session::get();
        if (!on && ses.recording()) {
            // Leaving practice mode ends the recording (it only works in practice mode); keep what we have.
            auto err = ses.stopRecording();
            ses.releaseRecordSpeed();
            Notification::create(err.empty() ? "Recording stopped (left practice mode)." : "Recording stopped: " + err,
                                 err.empty() ? NotificationIcon::Info : NotificationIcon::Warning)->show();
        }
        PlayLayer::togglePracticeMode(on);
    }

    void pauseGame(bool unfocused) {
        wp::Session::get().releaseRecordSpeed();   // menus run at normal speed; re-applied once the level runs again
        PlayLayer::pauseGame(unfocused);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& S = wp::sim::SimState::get();
        {
            // GD's anti-cheat spike: an invisible normal spike at the start of every level. A macro touches it
            // (that is what it is there for), so it must never count as a death while the macro is being analysed
            // or played back. Real, hand-played attempts are left completely alone.
            auto& ses = wp::Session::get();
            if (object && object == m_anticheatSpike &&
                (S.trialActive || (ses.macro() && ses.playbackOn() && !ses.recording())))
                return;
        }
        if (S.trialActive) {
            // A simulated death must not play animations, sounds, or change game state; the analyzer only
            // needs to know it happened.
            S.died = true;
            S.deathP2 = player == m_player2;
            S.deathTick = wp::ticksOf(m_gameState.m_currentProgress);
            S.deathObjectId = object ? object->m_objectID : -1;
            S.deathObjectType = object ? static_cast<int>(object->m_objectType) : -1;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto& S = wp::sim::SimState::get();
        if (S.trialActive) { S.ended = true; return; }
        auto& ses = wp::Session::get();
        static bool s_botEndPending = false;   // the game can call levelComplete more than once for one finish
        if (s_botEndPending) return;
        if (ses.botAttempt() && !ses.recording()) {
            // The macro, not you, beat the level (any mode): no completion screen, nothing counted, start again.
            ses.clearBotAttempt();
            s_botEndPending = true;
            Notification::create("Macro finished (not counted).", NotificationIcon::Info)->show();
            Loader::get()->queueInMainThread([] {
                s_botEndPending = false;
                if (auto* pl = PlayLayer::get()) pl->resetLevelFromStart();
            });
            return;
        }
        if (ses.recording()) {
            // Beat the level while recording: store the pieced-together run as the current macro.
            auto err = ses.stopRecording();
            ses.releaseRecordSpeed();
            Notification::create(err.empty() ? "Recording stored. Press Analyze: it checks the macro and fixes clicks that are a few ticks off." : "Recording not stored: " + err,
                                 err.empty() ? NotificationIcon::Success : NotificationIcon::Error)->show();
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        g_cpVel.clear();
        wp::Session::get().releaseRecordSpeed();
        wp::sim::Analyzer::get().abandon();
        wp::ui::detach(this);
        wp::Session::get().detach();
        wp::applyFrameLimiter(false);
        PlayLayer::onQuit();
    }
};

// ---- audio: checkpoint loads would seek the music thousands of times ----------------------------------
class $modify(WPAudio, FMODAudioEngine) {
    void loadAudioState(FMODAudioState& state) {
        if (wp::sim::SimState::get().muteAudio) return;
        FMODAudioEngine::loadAudioState(state);
    }
};

// ---- pause menu button ---------------------------------------------------------------------------------------
class $modify(WPPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!wp::Session::get().pl()) return;
        auto* btn = CCMenuItemSpriteExtra::create(wp::ui::makePauseButtonNode(), this, menu_selector(WPPauseLayer::onWindowpane));
        btn->setID("windowpane-button"_spr);
        auto win = CCDirector::get()->getWinSize();
        auto* menu = CCMenu::create();
        menu->setPosition({win.width - 36.1f, win.height - 110.8f});   // right below the % button, same column
        menu->addChild(btn);
        menu->setID("windowpane-menu"_spr);
        this->addChild(menu, 10);
    }

    void onWindowpane(CCObject*) { wp::ui::openControlPopup(); }
};
