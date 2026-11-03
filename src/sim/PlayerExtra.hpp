#pragma once
// Everything about a PlayerObject that GD's own PlayerCheckpoint forgets but the physics still uses.
// The member list is generated from the bindings (tools/gen_player_extra.py -> PlayerFields.inc).

#include <Geode/Geode.hpp>
#include <string>
#include <vector>

namespace wp::sim {

template <class T> struct ArgType;
template <class U> struct ArgType<void(U)> { using type = U; };
#define WP_TYPE_OF(T) ::wp::sim::ArgType<void T>::type

struct PlayerExtra {
    bool valid = false;

    // node transform
    cocos2d::CCPoint position{0, 0};
    float rotation = 0.f;
    float scaleX = 1.f, scaleY = 1.f;

    // rings the player is currently inside: a CCArray that is mutated in place every step, so a
    // pointer copy would alias the live array. Deep copy the element list instead.
    std::vector<geode::Ref<cocos2d::CCObject>> touchingRings;
    bool hadTouchingRings = false;

    // generated plain-state members
#define WP_F(T, N) WP_TYPE_OF(T) N{};
#include "PlayerFields.inc"
#undef WP_F

    void capture(PlayerObject* p);
    void restore(PlayerObject* p) const;
};

// The game mode of a player (cube when all are false). Writing these flags directly (PlayerExtra::restore) changes the physics
// but not what the player LOOKS like: the vehicle sprites are only switched by the game's own toggle functions. A restore that
// flips a flag without them leaves the old vehicle's icon drawn on top of the new one (and the next respawn does not clean it up,
// because by then the flags already say "cube").
struct ModeFlags {
    bool ship = false, bird = false, ball = false, dart = false, robot = false, spider = false, swing = false;
    static ModeFlags of(PlayerObject* p);
    static ModeFlags of(PlayerExtra const& e);
};
// Makes the sprites of `p` show the modes in `to`, given that they currently show the modes in `from`, by running the game's own
// mode switches for every mode that differs (modes are switched off first, then on). The flags end up as `to`.
void syncModeVisuals(PlayerObject* p, ModeFlags const& from, ModeFlags const& to);
// Switches every vehicle's visuals off, which leaves the plain icon (used when an analysis ends).
void resetModeVisuals(PlayerObject* p);

// A short, human readable list of the members that differ. Used for the snapshot self-test report so
// the log says WHICH field drifted (e.g. "m_holdingButtons, m_yVelocity") instead of just "mismatch".
std::string diffExtra(PlayerExtra const& a, PlayerExtra const& b);

}  // namespace wp::sim
