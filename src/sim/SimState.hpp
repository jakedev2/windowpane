#pragma once
#include <vector>
// Flags shared between the analysis engine and the game hooks. While the engine drives physics steps
// the hooks (death, level complete, audio, input observer, update) consult this to stay out of the way.

#include <cstdint>

namespace wp::sim {

struct SimState {
    bool stepping = false;       // inside a physics step the analyzer started (update hook passes through)
    bool trialActive = false;    // analyzer is simulating; death / completion are intercepted
    bool muteAudio = false;      // swallow audio-state loads and effects

    // How the stepper drives one physics step (chosen by the calibration at the start of an analysis).
    bool forceDelta = true;      // getModifiedDelta returns exactly one step while stepping
    float dtScale = 1.f;         // multiplier on the dt handed to update()

    // Half-pass mode: when the game runs several command passes per tick, a click "between two ticks" is queued in
    // the second pass of the update (plain queue, timestamp 0) instead of with a timestamp.
    struct PassInput { int button; bool down; bool p2; };
    bool halfPassMode = false;
    std::vector<PassInput> secondPass;
    int passInUpdate = 0;
    bool secondPassMissed = false;   // inputs were waiting for a second pass that never came

    // results of the most recent step
    bool died = false;
    bool deathP2 = false;
    uint32_t deathTick = 0;
    bool ended = false;          // level completed / end trigger reached
    int deathObjectId = -1;      // what killed the player (for the diagnostics only)
    int deathObjectType = -1;

    void clearOutcome() { died = false; deathP2 = false; deathTick = 0; ended = false; deathObjectId = -1; deathObjectType = -1; }

    static SimState& get() {
        static SimState s;
        return s;
    }
};

}  // namespace wp::sim
