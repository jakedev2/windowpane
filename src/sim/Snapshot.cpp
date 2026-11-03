#include "Snapshot.hpp"
#include "../core/Types.hpp"

#include <type_traits>
#include <utility>

using namespace geode::prelude;

namespace wp::sim {

namespace {
// Orbs / pads / portals / triggers remember that they were used (GameObject::hasBeenActivatedByPlayer). The member that holds this is not
// named in these bindings, so it is read through the game's own getter and cleared through the game's own GameObject::resetObject, which
// is what a normal respawn calls on every object. resetObject is looked up at compile time: if a binding version lacks it, nothing is reset.
template <class T, class = void> struct HasReset : std::false_type {};
template <class T> struct HasReset<T, std::void_t<decltype(std::declval<T&>().resetObject())>> : std::true_type {};
template <class T> auto doReset(T* o, int) -> decltype(o->resetObject(), void()) { o->resetObject(); }
template <class T> void doReset(T*, long) {}
constexpr bool hasReset() { return HasReset<GameObject>::value; }

uint8_t usedFlags(GameObject* o, PlayerObject* p1, PlayerObject* p2) {
    uint8_t f = 0;
    if (p1 && o->hasBeenActivatedByPlayer(p1)) f |= 1;
    if (p2 && o->hasBeenActivatedByPlayer(p2)) f |= 2;
    return f;
}
}

bool Snapshot::capture(PlayLayer* pl) {
    checkpoint = nullptr;
    p1 = {};
    p2 = {};
    if (!pl || !pl->m_player1) return false;

    // createCheckpoint() is what practice mode calls when it drops a checkpoint. In some game builds it
    // also appends to m_checkpointArray; our snapshots must never show up as practice checkpoints.
    const unsigned before = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    // PlayLayer::m_currentCheckpoint is a plain (not retained) pointer. A snapshot must never become it: when the snapshot is
    // dropped the checkpoint is deleted and the game would be left pointing at it (crash on the next death in practice mode).
    CheckpointObject* const prevCurrent = pl->m_currentCheckpoint;
    CheckpointObject* cp = pl->createCheckpoint();
    if (!cp) { pl->m_currentCheckpoint = prevCurrent; return false; }
    checkpoint = cp;   // Ref retains
    if (pl->m_checkpointArray) {
        while (pl->m_checkpointArray->count() > before) pl->m_checkpointArray->removeLastObject();
    }
    pl->m_currentCheckpoint = prevCurrent;

    used.clear();
    objCount = 0;
    if (!hasReset()) {
        static bool warned = false;
        if (!warned) { warned = true; log::warn("[Windowpane] these game bindings have no GameObject::resetObject, so used orbs / pads are not reset by snapshot restores"); }
    }
    if (hasReset() && pl->m_objects) {
        objCount = static_cast<uint32_t>(pl->m_objects->count());
        for (uint32_t i = 0; i < objCount; ++i) {
            auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            if (!o) continue;
            const uint8_t f = usedFlags(o, pl->m_player1, pl->m_player2);
            if (f) used.push_back({i, f});
        }
    }

    p1.capture(pl->m_player1);
    p2.capture(pl->m_player2);
    randomSeed = pl->m_randomSeed;
    replayRandSeed = pl->m_replayRandSeed;
    currentStep = pl->m_currentStep;
    tick = ticksOf(pl->m_gameState.m_currentProgress);
    return true;
}

bool Snapshot::restore(PlayLayer* pl) const {
    if (!pl || !valid()) return false;

    // Drop any real input that arrived since the last step so it cannot leak into the trial.
    pl->m_queuedButtons.clear();

    // What the players looked like before the load: the vanilla load may not switch the vehicle sprites back (see ModeFlags).
    const ModeFlags pre1 = ModeFlags::of(pl->m_player1);
    const ModeFlags pre2 = ModeFlags::of(pl->m_player2);

    CheckpointObject* const prevCurrent = pl->m_currentCheckpoint;   // see capture(): never leave the game pointing at a snapshot
    pl->loadFromCheckpoint(checkpoint);
    pl->m_currentCheckpoint = prevCurrent;

    // Orbs / pads / portals the first pass or an earlier trial used after this snapshot was taken are still marked as used: a normal
    // respawn resets every object before it loads a checkpoint, this load alone does not, so the click on such an orb did nothing in every
    // later run from the same snapshot. Reset exactly the objects that are marked used now but were not when the snapshot was taken.
    if (hasReset() && pl->m_objects && static_cast<uint32_t>(pl->m_objects->count()) == objCount) {
        size_t j = 0;
        for (uint32_t i = 0; i < objCount; ++i) {
            const bool usedAtCapture = j < used.size() && used[j].idx == i;
            if (usedAtCapture) { ++j; continue; }
            auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            if (!o) continue;
            if (usedFlags(o, pl->m_player1, pl->m_player2)) doReset(o, 0);
        }
    }

    // Make the visuals match the restored game mode BEFORE the physics state is written back, because switching a mode can move
    // or rescale the player; the restore below puts all of that back.
    syncModeVisuals(pl->m_player1, pre1, ModeFlags::of(p1));
    if (p2.valid) syncModeVisuals(pl->m_player2, pre2, ModeFlags::of(p2));

    // The vanilla load rebuilds the level and both PlayerCheckpoints. Re-apply the physics state it
    // leaves out. Order matters: players last, because loadFromCheckpoint re-positions them.
    p1.restore(pl->m_player1);
    p2.restore(pl->m_player2);

    pl->m_randomSeed = randomSeed;
    pl->m_replayRandSeed = replayRandSeed;
    pl->m_currentStep = currentStep;
    pl->m_extraDelta = 0.0;          // the stepper feeds exact 1/240 s deltas; no leftover time
    pl->m_resumeTimer = 0;           // vanilla "resume" countdown would swallow steps
    pl->m_queuedButtons.clear();

    // Snapshots are only ever taken mid-run, so the "level finished / player died" flags were false.
    // A previous trial may have left them set; the vanilla load does not clear them.
    pl->m_levelEndAnimationStarted = false;
    pl->m_hasCompletedLevel = false;
    pl->m_playerDied = false;
    return true;
}

}  // namespace wp::sim
