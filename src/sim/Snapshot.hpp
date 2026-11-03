#pragma once
#include "PlayerExtra.hpp"

#include <Geode/Geode.hpp>
#include <vector>

namespace wp::sim {

// A full restorable game state: GD's own CheckpointObject (level objects, trigger / effect-manager
// state, GJGameState, both PlayerCheckpoints) plus PlayerExtra for each player and the few
// GJBaseGameLayer members the checkpoint does not carry.
struct Snapshot {
    geode::Ref<CheckpointObject> checkpoint;
    PlayerExtra p1, p2;
    uint64_t randomSeed = 0;
    uint64_t replayRandSeed = 0;
    int currentStep = 0;
    uint32_t tick = 0;       // tick (progress / progressPerTick) when captured

    // Which orbs / pads / portals had already been used when the snapshot was taken (index into PlayLayer::m_objects, bit 0 = player 1,
    // bit 1 = player 2). The game's own checkpoint load does not reset these flags (a normal respawn resets every object first), so
    // after a trial had used an orb the same snapshot could not use it again and the next run from it died. See Snapshot::restore.
    struct Used { uint32_t idx; uint8_t flags; };
    std::vector<Used> used;
    uint32_t objCount = 0;

    bool valid() const { return checkpoint != nullptr && p1.valid; }

    // Must be called at a tick boundary: after a physics step finished and before the next one starts.
    bool capture(PlayLayer* pl);
    bool restore(PlayLayer* pl) const;
};

}  // namespace wp::sim
