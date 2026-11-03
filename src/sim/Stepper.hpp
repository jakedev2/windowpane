#pragma once
#include "../core/Types.hpp"
#include "SimState.hpp"

#include <Geode/Geode.hpp>
#include <vector>

namespace wp::sim {

// Compact per-tick picture of both players, used to (a) detect when a shifted run has rejoined the
// original run and (b) check that two supposedly identical simulations really are identical.
struct PState {
    float x = 0, y = 0;
    double yVel = 0;
    float rot = 0;
    uint16_t flags = 0;   // vehicle bits, gravity, ground, dashing, holding
    // Hash of the hidden physics state (jump buffers, ring / pad flags, dash, slope and platformer state, ...) that
    // x / y / yVelocity / flags do not show. Two runs only count as "the same run again" when this matches too.
    uint64_t deep = 0;
    bool operator==(PState const& o) const = default;
};
struct TickState {
    PState p1, p2;
    bool dual = false;
};

// Reads the visible state and, optionally, the more expensive hidden-state hash. Diagnostic-only comparisons should pass false.
TickState readTickState(PlayLayer* pl, bool includeDeep = true);
// Fills in the hidden-state hashes after a visible-only read. Used only after the visible states already match.
void addDeepHashes(PlayLayer* pl, TickState& state);
// Allocation-free hot-path predicate. compareTickState below is for diagnostics, where a string is actually needed.
bool visibleTickStateEqual(TickState const& a, TickState const& b, double posTol = 1e-4);
// returns "" when equal, otherwise the name of the first field that differs
std::string compareTickState(TickState const& a, TickState const& b, double posTol = 1e-4);
// True when the visible state matches (compareTickState) AND, if `useDeep`, the hidden physics state matches as well.
bool sameTickState(TickState const& a, TickState const& b, double posTol, bool useDeep);
// True when the visible state matches but the hidden state does not (used to notice an over-strict hidden-state hash).
bool onlyDeepDiffers(TickState const& a, TickState const& b, double posTol);

enum class StepResult { Ok, Died, Ended, Fault };

// Applies due inputs through the game's own input queue and advances exactly one 240 Hz physics step
// with the game's own update function - the same code path as normal play.
class Stepper {
public:
    explicit Stepper(PlayLayer* pl) : m_pl(pl) {}

    uint32_t tick() const;

    // How many physics steps the last step() call really advanced. Always 1, except for the very first step
    // of a level (tick 0), which some game setups run as two steps in one update; see Stepper.cpp.
    uint32_t lastAdvance() const { return m_advance; }

    // Runs ONE update with the current settings and returns how many physics steps it advanced. No retries and no
    // result handling: this is only for the calibration at the start of an analysis.
    uint32_t probe();
    float dtFor() const;

    // Queue one input for the NEXT step. `sub` is only used when `cbf` is true.
    void queueInput(InputEvent const& e, bool cbf);

    // Advance one step. On Fault, `faultText` explains what the game did instead of advancing by one.
    StepResult step(std::string& faultText);

private:
    PlayLayer* m_pl;
    uint32_t m_advance = 1;
};

struct Snapshot;
// Measures progress counts per tick (see core/Types.hpp) and picks how the stepper drives the game. Returns "" on
// success or an error text. `back` is restored afterwards.
std::string calibrateTickRate(PlayLayer* pl, Snapshot const& back);

}  // namespace wp::sim
