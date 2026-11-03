#include "PlayerExtra.hpp"

#include <fmt/format.h>

using namespace geode::prelude;

namespace wp::sim {

void PlayerExtra::capture(PlayerObject* p) {
    valid = false;
    if (!p) return;

    position = p->CCNode::getPosition();
    rotation = p->getRotation();
    scaleX = p->getScaleX();
    scaleY = p->getScaleY();

    touchingRings.clear();
    hadTouchingRings = p->m_touchingRings != nullptr;
    if (p->m_touchingRings) {
        auto* arr = p->m_touchingRings;
        for (unsigned i = 0; i < arr->count(); ++i)
            touchingRings.emplace_back(arr->objectAtIndex(i));
    }

#define WP_F(T, N) N = p->N;
#include "PlayerFields.inc"
#undef WP_F
    valid = true;
}

void PlayerExtra::restore(PlayerObject* p) const {
    if (!p || !valid) return;

#define WP_F(T, N) p->N = N;
#include "PlayerFields.inc"
#undef WP_F

    if (p->m_touchingRings && hadTouchingRings) {
        p->m_touchingRings->removeAllObjects();
        for (auto const& o : touchingRings) p->m_touchingRings->addObject(o);
    }

    // Node transform last: the vanilla checkpoint has already placed the player, this only guards
    // against the rotation / scale bookkeeping that PlayerCheckpoint does not carry.
    p->setRotation(rotation);
    p->setScaleX(scaleX);
    p->setScaleY(scaleY);
}

ModeFlags ModeFlags::of(PlayerObject* p) {
    ModeFlags m;
    if (!p) return m;
    m.ship = p->m_isShip; m.bird = p->m_isBird; m.ball = p->m_isBall; m.dart = p->m_isDart;
    m.robot = p->m_isRobot; m.spider = p->m_isSpider; m.swing = p->m_isSwing;
    return m;
}

ModeFlags ModeFlags::of(PlayerExtra const& e) {
    ModeFlags m;
    m.ship = e.m_isShip; m.bird = e.m_isBird; m.ball = e.m_isBall; m.dart = e.m_isDart;
    m.robot = e.m_isRobot; m.spider = e.m_isSpider; m.swing = e.m_isSwing;
    return m;
}

namespace {
// Runs the game's own switch for one mode. The game ignores a switch to the state the flag already has, so the flag is put to the
// opposite first: the switch then always does the whole job (sprites, glow, particles).
void switchMode(PlayerObject* p, int mode, bool on) {
    switch (mode) {
        case 0: p->m_isShip = !on; p->toggleFlyMode(on, false); break;
        case 1: p->m_isBird = !on; p->toggleBirdMode(on, false); break;
        case 2: p->m_isBall = !on; p->toggleRollMode(on, false); break;
        case 3: p->m_isDart = !on; p->toggleDartMode(on, false); break;
        case 4: p->m_isRobot = !on; p->toggleRobotMode(on, false); break;
        case 5: p->m_isSpider = !on; p->toggleSpiderMode(on, false); break;
        case 6: p->m_isSwing = !on; p->toggleSwingMode(on, false); break;
    }
}
void setFlags(PlayerObject* p, ModeFlags const& m) {
    p->m_isShip = m.ship; p->m_isBird = m.bird; p->m_isBall = m.ball; p->m_isDart = m.dart;
    p->m_isRobot = m.robot; p->m_isSpider = m.spider; p->m_isSwing = m.swing;
}
}  // namespace

void syncModeVisuals(PlayerObject* p, ModeFlags const& from, ModeFlags const& to) {
    if (!p) return;
    const bool f[7] = {from.ship, from.bird, from.ball, from.dart, from.robot, from.spider, from.swing};
    const bool t[7] = {to.ship, to.bird, to.ball, to.dart, to.robot, to.spider, to.swing};
    for (int m = 0; m < 7; ++m) if (f[m] && !t[m]) switchMode(p, m, false);
    for (int m = 0; m < 7; ++m) if (!f[m] && t[m]) switchMode(p, m, true);
    setFlags(p, to);
}

void resetModeVisuals(PlayerObject* p) {
    if (!p) return;
    for (int m = 0; m < 7; ++m) switchMode(p, m, false);
    setFlags(p, ModeFlags{});
}

namespace {
template <class T>
bool same(T const& a, T const& b) {
    if constexpr (std::is_same_v<T, cocos2d::CCPoint>) {
        return a.x == b.x && a.y == b.y;
    } else if constexpr (std::is_same_v<T, geode::SeedValueRSV>) {
        auto ca = a;
        auto cb = b;
        return ca.value() == cb.value();
    } else if constexpr (std::is_same_v<T, gd::unordered_map<int, GJPointDouble>>) {
        // GJPointDouble has no operator==, so std::unordered_map's would not compile
        if (a.size() != b.size()) return false;
        for (auto const& [k, v] : a) {
            auto it = b.find(k);
            if (it == b.end() || it->second.m_x != v.m_x || it->second.m_y != v.m_y) return false;
        }
        return true;
    } else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
        return a == b;
    } else {
        return true;   // not comparable: cannot tell, do not report a false difference
    }
}
}  // namespace

std::string diffExtra(PlayerExtra const& a, PlayerExtra const& b) {
    std::string out;
    auto add = [&](char const* n) {
        if (!out.empty()) out += ", ";
        out += n;
    };
    if (!same(a.position, b.position)) add("position");
    if (a.rotation != b.rotation) add("rotation");
#define WP_F(T, N) if (!same<WP_TYPE_OF(T)>(a.N, b.N)) add(#N);
#include "PlayerFields.inc"
#undef WP_F
    return out;
}

}  // namespace wp::sim
