#pragma once
// Typed, always-current copy of the mod settings. Reloaded whenever any setting changes, so the
// rest of the code reads plain struct members instead of hitting the settings API every frame.

#include "Types.hpp"

#include <Geode/Geode.hpp>
#include <string>
#include <vector>

namespace wp {

struct Cfg {
#define WP_B(f, k) bool f = false;
#define WP_I(f, k) int f = 0;
#define WP_D(f, k) double f = 0;
#define WP_S(f, k) std::string f;
#define WP_C(f, k) cocos2d::ccColor3B f = {255, 255, 255};
#include "CfgFields.inc"
#undef WP_B
#undef WP_I
#undef WP_D
#undef WP_S
#undef WP_C

    std::vector<Bucket> buckets;   // all configured rows, sorted highest range first
    uint64_t revision = 0;         // bumped on every reload so UI nodes know to rebuild

    void reload();
    static Cfg& get();

    // display helpers
    int displayFpsValue() const;
    int windowDecimals() const;    // decimals actually used for window numbers
};

inline Cfg& cfg() { return Cfg::get(); }

// "5", "5-6", "16+"  ->  lo/hi. Returns false when the text is not a valid range.
bool parseRange(std::string const& text, int& lo, int& hi);

// Index of the bucket a window belongs to (into Cfg::buckets) or -1.
int bucketIndexFor(Cfg const& c, double window);

}  // namespace wp
