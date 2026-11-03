#include "Stepper.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>

using namespace geode::prelude;

namespace wp::sim {

namespace {
// Small order-sensitive 64-bit mixer (FNV-1a style with an extra avalanche step).
struct Mix {
    uint64_t h = 1469598103934665603ull;
    void u(uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 1099511628211ull;
        h ^= h >> 29;
    }
    void b(bool v) { u(v ? 0xB5ull : 0x4Full); }
    void i(int64_t v) { u(static_cast<uint64_t>(v)); }
    // Floating point values are quantised to 1e-4 so the harmless last-bit differences of two runs that rejoined do not
    // count as a difference, while anything that can change the physics does.
    void d(double v) {
        if (!std::isfinite(v)) { u(0xDEADBEEFull); return; }
        i(static_cast<int64_t>(std::llround(v * 1e4)));
    }
};

// The hidden state of one player that decides how it will move in the future. Time stamps, rotation / animation values and
// raw object pointers are left out on purpose: they differ between two runs without changing the physics.
uint64_t deepHash(PlayerObject* p) {
    Mix m;
    m.d(p->m_yVelocity);
    m.d(p->m_gravity);
    m.d(p->m_speedMultiplier);
    m.d(p->m_fallSpeed);
    m.d(p->m_groundYVelocity);
    m.d(p->m_accelerationOrSpeed);
    m.d(p->m_snapDistance);
    m.d(p->m_dashX);
    m.d(p->m_dashY);
    m.d(p->m_dashAngle);
    m.d(p->m_platformerXVelocity);
    m.d(p->m_currentSlopeYVelocity);
    m.d(p->m_yVelocityBeforeSlope);
    m.d(p->m_blackOrbRelated);
    m.d(p->m_maybeSlopeForce);
    m.d(p->m_gravityMod);
    m.d(p->m_playerSpeed);
    m.d(p->m_vehicleSize);
    m.d(p->m_slopeVelocity);
    m.d(p->m_slopeAngle);
    m.d(p->m_slopeAngleRadians);
    m.b(p->m_jumpBuffered);
    m.b(p->m_wasJumpBuffered);
    m.b(p->m_stateRingJump);
    m.b(p->m_stateRingJump2);
    m.b(p->m_ringJumpRelated);
    m.b(p->m_padRingRelated);
    m.b(p->m_touchedRing);
    m.b(p->m_touchedCustomRing);
    m.b(p->m_touchedPad);
    m.b(p->m_touchedGravityPortal);
    m.b(p->m_wasRobotJump);
    m.b(p->m_isOnSlope);
    m.b(p->m_wasOnSlope);
    m.b(p->m_isCollidingWithSlope);
    m.b(p->m_isSliding);
    m.b(p->m_isOnIce);
    m.b(p->m_isOnGround2);
    m.b(p->m_isOnGround3);
    m.b(p->m_isOnGround4);
    m.b(p->m_holdingLeft);
    m.b(p->m_holdingRight);
    m.b(p->m_leftPressedFirst);
    m.b(p->m_isDead);
    m.b(p->m_isAccelerating);
    m.b(p->m_hasEverJumped);
    m.b(p->m_hasEverHitRing);
    m.b(p->m_isLocked);
    m.b(p->m_controlsDisabled);
    m.b(p->m_inputsLocked);
    m.i(p->m_stateJumpBuffered);
    m.i(p->m_stateOnGround);
    m.i(p->m_stateNoStickX);
    m.i(p->m_stateNoStickY);
    m.i(p->m_stateBoostX);
    m.i(p->m_stateBoostY);
    m.i(p->m_stateScale);
    m.i(p->m_stateForce);
    m.i(p->m_stateNoAutoJump);
    m.i(p->m_stateDartSlide);
    m.i(p->m_stateHitHead);
    m.i(p->m_stateFlipGravity);
    m.i(p->m_stateUnk);
    m.i(p->m_stateUnk2);
    m.i(p->m_maybeStateForce2);
    m.i(p->m_collidingWithSlopeId);
    // containers: order independent (sum of per-element hashes)
    {
        uint64_t acc = 0;
        for (auto const& k : p->m_touchedRings) { Mix e; e.i(k); acc += e.h; }
        m.u(acc);
        m.u(p->m_touchedRings.size());
    }
    {
        uint64_t acc = 0;
        for (auto const& k : p->m_ringRelatedSet) { Mix e; e.i(k); acc += e.h; }
        m.u(acc);
        m.u(p->m_ringRelatedSet.size());
    }
    {
        uint64_t acc = 0;
        for (auto const& kv : p->m_holdingButtons) { Mix e; e.i(kv.first); e.b(kv.second); acc += e.h; }
        m.u(acc);
        m.u(p->m_holdingButtons.size());
    }
    {
        uint64_t acc = 0;
        for (auto const& kv : p->m_jumpPadRelated) { Mix e; e.i(kv.first); e.b(kv.second); acc += e.h; }
        m.u(acc);
        m.u(p->m_jumpPadRelated.size());
    }
    return m.h;
}

void fill(PlayerObject* p, PState& s, bool includeDeep) {
    if (!p) return;
    s.x = p->getPositionX();
    s.y = p->getPositionY();
    s.yVel = p->m_yVelocity;
    s.rot = p->getRotation();
    uint16_t f = 0;
    auto bit = [&](bool v, int n) { if (v) f |= static_cast<uint16_t>(1u << n); };
    bit(p->m_isShip, 0);
    bit(p->m_isBird, 1);
    bit(p->m_isBall, 2);
    bit(p->m_isDart, 3);
    bit(p->m_isRobot, 4);
    bit(p->m_isSpider, 5);
    bit(p->m_isSwing, 6);
    bit(p->m_isUpsideDown, 7);
    bit(p->m_isOnGround, 8);
    bit(p->m_isDashing, 9);
    bit(p->m_isSideways, 10);
    auto it = p->m_holdingButtons.find(1);
    bit(it != p->m_holdingButtons.end() && it->second, 11);
    bit(p->m_isGoingLeft, 12);
    s.flags = f;
    if (includeDeep) s.deep = deepHash(p);
}
}  // namespace

TickState readTickState(PlayLayer* pl, bool includeDeep) {
    TickState t;
    if (!pl) return t;
    fill(pl->m_player1, t.p1, includeDeep);
    t.dual = pl->m_gameState.m_isDualMode;
    if (t.dual) fill(pl->m_player2, t.p2, includeDeep);
    return t;
}

void addDeepHashes(PlayLayer* pl, TickState& state) {
    if (!pl) return;
    if (pl->m_player1) state.p1.deep = deepHash(pl->m_player1);
    if (state.dual && pl->m_player2) state.p2.deep = deepHash(pl->m_player2);
}

bool visibleTickStateEqual(TickState const& a, TickState const& b, double tol) {
    auto equal = [tol](PState const& x, PState const& y) {
        // Rotation is deliberately excluded, exactly as in compareTickState: it is visual, not physics.
        // Keep the diagnostic comparison's exact NaN behaviour as well as its tolerance: the old checks used `> tol`.
        return !(std::abs(x.x - y.x) > tol) && !(std::abs(x.y - y.y) > tol) &&
               !(std::abs(x.yVel - y.yVel) > tol) && x.flags == y.flags;
    };
    return equal(a.p1, b.p1) && a.dual == b.dual && (!a.dual || equal(a.p2, b.p2));
}

std::string compareTickState(TickState const& a, TickState const& b, double tol) {
    auto cmp = [&](PState const& x, PState const& y, char const* who) -> std::string {
        if (std::abs(x.x - y.x) > tol) return fmt::format("{}.x ({:.4f} vs {:.4f})", who, x.x, y.x);
        if (std::abs(x.y - y.y) > tol) return fmt::format("{}.y ({:.4f} vs {:.4f})", who, x.y, y.y);
        if (std::abs(x.yVel - y.yVel) > tol) return fmt::format("{}.yVelocity ({:.4f} vs {:.4f})", who, x.yVel, y.yVel);
        if (x.flags != y.flags) {
            const uint16_t d = x.flags ^ y.flags;
            static char const* names[] = {"ship", "ufo", "ball", "wave", "robot", "spider", "swing", "upsideDown",
                                          "onGround", "dashing", "sideways", "holdingJump", "goingLeft"};
            std::string n;
            for (int i = 0; i < 13; ++i)
                if (d & (1u << i)) { if (!n.empty()) n += "+"; n += names[i]; }
            return fmt::format("{}.flags[{}]", who, n);
        }
        // Rotation is NOT compared: it is a visual animation value that is not restored identically by a snapshot and
        // does not affect survival (the cube hitbox does not rotate), so comparing it made good replays "diverge".
        return {};
    };
    if (auto s = cmp(a.p1, b.p1, "p1"); !s.empty()) return s;
    if (a.dual != b.dual) return "dualMode";
    if (a.dual) return cmp(a.p2, b.p2, "p2");
    return {};
}

bool sameTickState(TickState const& a, TickState const& b, double tol, bool useDeep) {
    if (!visibleTickStateEqual(a, b, tol)) return false;
    if (!useDeep) return true;
    if (a.p1.deep != b.p1.deep) return false;
    return !a.dual || a.p2.deep == b.p2.deep;
}

bool onlyDeepDiffers(TickState const& a, TickState const& b, double tol) {
    if (!visibleTickStateEqual(a, b, tol)) return false;
    if (a.p1.deep != b.p1.deep) return true;
    return a.dual && a.p2.deep != b.p2.deep;
}

uint32_t Stepper::tick() const {
    return ticksOf(m_pl->m_gameState.m_currentProgress);
}

void Stepper::queueInput(InputEvent const& e, bool cbf) {
    // The input goes through GJBaseGameLayer's own queue and is consumed by processQueuedButtons at the
    // top of the next physics step - exactly how a real click or a bot's click arrives.
    // timestamp 0 means "at the start of the step"; with CBF it is the absolute level time of the click.
    auto& S = SimState::get();
    if (S.halfPassMode && e.sub >= 0.25) {
        S.secondPass.push_back({static_cast<int>(e.button), e.down, e.p2});   // delivered by the second command pass
        return;
    }
    const double ts = (!S.halfPassMode && cbf && e.sub > 0.0) ? (static_cast<double>(e.tick) + e.sub) * (1.0 / tps()) : 0.0;
    m_pl->queueButton(static_cast<int>(e.button), e.down, e.p2, ts);
}

float Stepper::dtFor() const {
    const float warp = std::min(m_pl->m_gameState.m_timeWarp, 1.f);
    return ((warp > 0.f ? warp : 1.f) / static_cast<float>(tps())) * SimState::get().dtScale;
}

uint32_t Stepper::probe() {
    auto& S = SimState::get();
    S.clearOutcome();
    const uint32_t before = m_pl->m_gameState.m_currentProgress;
    m_pl->m_extraDelta = 0.0;
    m_pl->m_resumeTimer = 0;
    S.passInUpdate = 0;
    S.stepping = true;
    m_pl->update(dtFor());
    S.stepping = false;
    if (!S.secondPass.empty()) { S.secondPassMissed = true; S.secondPass.clear(); }
    return m_pl->m_gameState.m_currentProgress - before;
}

StepResult Stepper::step(std::string& fault) {
    auto& S = SimState::get();
    S.clearOutcome();

    const uint32_t before = m_pl->m_gameState.m_currentProgress;
    // With a slow-motion trigger active one update(dt) must be exactly one step of dt = warp / 240, so
    // use that dt; GJBaseGameLayer::getModifiedDelta rounds dt / (warp / 240) to a whole step count.
    const float dt = dtFor();

    uint32_t after = before;
    uint32_t perAttempt[3] = {0, 0, 0};
    for (int attempt = 0; attempt < 3; ++attempt) {
        m_pl->m_extraDelta = 0.0;
        m_pl->m_resumeTimer = 0;
        S.passInUpdate = 0;
        S.stepping = true;
        m_pl->update(dt);
        S.stepping = false;
        if (!S.secondPass.empty()) { S.secondPassMissed = true; S.secondPass.clear(); }
        const uint32_t now = m_pl->m_gameState.m_currentProgress;
        perAttempt[attempt] = now - after;
        after = now;
        if (after != before) break;
    }
    m_advance = 1;

    if (after == before) {
        fault = fmt::format("the game did not advance a physics step at tick {} (m_started={}, resume timer={})",
                            ticksOf(before), m_pl->m_started, m_pl->m_resumeTimer);
        return StepResult::Fault;
    }
    // One update is exactly one tick = progressPerTick() progress counts (1 on a plain game, 2 on setups that count
    // twice per physics step; measured by calibrateTickRate).
    const uint32_t per = progressPerTick();
    const uint32_t adv = after - before;
    if (adv != per) {
        // The very first update of a level may run one tick more (ticks 0 and 1 together). The physics are
        // deterministic, so that equals two single steps; the analyzer fills in the per-tick records of the
        // skipped tick, and only an input due exactly on tick 1 would be applied late (it is reported).
        if (before == 0 && adv == 2 * per) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                log::warn("[Windowpane] the first update of the level ran 2 ticks ({} progress counts); continuing, tick 1 is treated as part of the first step.", adv);
            }
            m_advance = 2;
        } else {
            fault = fmt::format("one update advanced {} progress counts at tick {}, expected {} (time warp {:.3f}, per attempt {}/{}/{}); the stepper needs exactly one tick",
                                adv, ticksOf(before), per, m_pl->m_gameState.m_timeWarp, perAttempt[0], perAttempt[1], perAttempt[2]);
            return StepResult::Fault;
        }
    }
    if (S.died) return StepResult::Died;
    if (S.ended || m_pl->m_levelEndAnimationStarted || m_pl->m_hasCompletedLevel) return StepResult::Ended;
    return StepResult::Ok;
}

// --------------------------------------------------------------------------------------------------
// Tick-rate calibration. Drives the game two updates from `back` (a snapshot of the current state) with each setting
// and reads how far GJGameState::m_currentProgress moves per update(1/240 s). That is the number of progress counts
// in one tick; on a plain game it is 1. The state is restored to `back` afterwards.
// --------------------------------------------------------------------------------------------------
std::string calibrateTickRate(PlayLayer* pl, Snapshot const& back) {
    auto& S = SimState::get();
    std::string table;
    uint32_t found = 0;
    bool foundForce = true;
    auto measure = [&]() {
        for (bool force : {true, false}) {
            S.dtScale = 1.f;
            S.forceDelta = force;
            if (!back.restore(pl)) break;
            Stepper st(pl);
            const uint32_t a = st.probe();
            const uint32_t b = st.probe();
            table += fmt::format(" [{}: {}/{}]", force ? "forced step" : "raw", a, b);
            // steady state: b counts per update; the first update may carry one extra tick, or none at all (the
            // very first update of a level can be used up by the game getting going)
            if (b >= 1 && b <= 8 && (a == b || a == 2 * b || a == 0)) { found = b; foundForce = force; return true; }
        }
        return false;
    };
    if (!measure() && !pl->m_started) {
        // At 0% the level has not started yet, so no update advances anything (progress 0/0). Start it, which is
        // what the game does itself a moment after a reset, then measure again (the snapshot is restored afterwards).
        table.clear();
        pl->startGame();
        measure();
    }
    back.restore(pl);
    pl->m_queuedButtons.clear();
    S.clearOutcome();
    S.stepping = false;
    if (found == 0) {
        S.dtScale = 1.f;
        S.forceDelta = true;
        log::warn("[Windowpane] tick-rate calibration failed (progress counts for two updates):{}", table);
        return fmt::format("Could not work out how far the game advances per update (progress counts for two updates:{}). "
                           "Another mod is probably interfering with the physics step.", table);
    }
    S.dtScale = 1.f;
    S.forceDelta = foundForce;
    if (progressPerTickRef() != found) {
        log::warn("[Windowpane] the game advances {} progress counts per 1/240 s; ticks are counted accordingly ({}).", found, table);
        progressPerTickRef() = found;
        Mod::get()->setSavedValue<int>("progress-per-tick", static_cast<int>(found));
    }
    return "";
}

}  // namespace wp::sim
