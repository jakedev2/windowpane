#include "Cfg.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

using namespace geode::prelude;

namespace wp {

namespace {
std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

bool parseInt(std::string const& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    for (char ch : s) if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    out = std::atoi(s.c_str());
    return true;
}

Rgb toRgb(ccColor3B c) { return Rgb{c.r, c.g, c.b}; }
}  // namespace

bool parseRange(std::string const& textIn, int& lo, int& hi) {
    std::string text = trim(textIn);
    if (text.empty()) return false;
    if (text.back() == '+') {
        if (!parseInt(trim(text.substr(0, text.size() - 1)), lo)) return false;
        hi = kOpen;
        return lo >= 0;
    }
    auto dash = text.find('-');
    if (dash == std::string::npos) {
        if (!parseInt(text, lo)) return false;
        hi = lo;
        return lo >= 0;
    }
    if (!parseInt(trim(text.substr(0, dash)), lo)) return false;
    if (!parseInt(trim(text.substr(dash + 1)), hi)) return false;
    if (hi < lo) std::swap(lo, hi);
    return lo >= 0;
}

Cfg& Cfg::get() {
    static Cfg instance;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        instance.reload();
    }
    return instance;
}

void Cfg::reload() {
    auto* mod = Mod::get();

    // One-time move to the accuracy-tuned defaults (a shorter, local survival check, and a top timing bucket with no upper limit).
    if (mod->getSavedValue<int>("tuning-profile", 0) < 4) {
        mod->setSettingValue<int64_t>("horizon-inputs", 3);
        mod->setSettingValue<int64_t>("horizon-tail", 16);
        mod->setSettingValue<int64_t>("horizon-min", 60);
        mod->setSettingValue<int64_t>("rejoin-extra", 60);
        mod->setSettingValue<int64_t>("fly-strictness", 2);
        if (mod->getSettingValue<std::string>("timings") == "1,2,3,4,5-6,7-8,9-12")
            mod->setSettingValue<std::string>("timings", "1,2,3,4,5-6,7-8,9+");
        mod->setSavedValue<int>("tuning-profile", 4);
    }
    // One-time (beta.19): faster analysis and no downward bias in fast ship / wave sections.
    if (mod->getSavedValue<int>("speed-profile", 0) < 1) {
        mod->setSettingValue<int64_t>("fly-strictness", 1);
        mod->setSettingValue<bool>("exact-replay", false);   // snapshots (self-tested, with automatic fallback) instead of replaying every trial from tick 0
        mod->setSettingValue<bool>("recheck-tight", false);  // the end re-check replays every tight click from the start of the level
        mod->setSavedValue<int>("speed-profile", 1);
    }
    // One-time (beta.21): ship / wave windows were capped at 1-3 frames by later clicks; faster group fallback for analysis speed.
    if (mod->getSavedValue<int>("speed-profile", 0) < 2) {
        mod->setSettingValue<int64_t>("fly-strictness", 0);
        mod->setSavedValue<int>("speed-profile", 2);
    }
    // One-time (2.5.1): the 3 inputs / 16 / 60 / 60 survival check (beta.18) gave too many 1-2 frame windows next to the reference counter
    // (10.9% of the clicks, the reference has 1.6%), because the tolerance of later clicks, which stay at their tick, counted against the
    // shifted one. The older 2 / 12 / 36 / 0 check was a little too generous in ship, wave and some cube parts. This is in between.
    if (mod->getSavedValue<int>("survival-profile", 0) < 1) {
        mod->setSettingValue<int64_t>("horizon-inputs", 2);
        mod->setSettingValue<int64_t>("horizon-tail", 14);
        mod->setSettingValue<int64_t>("horizon-min", 48);
        mod->setSettingValue<int64_t>("rejoin-extra", 30);
        mod->setSavedValue<int>("survival-profile", 1);
    }
    // One-time (beta.23): 5 fps is plenty while analysing; fewer, longer slices so long levels run as fast as short ones.
    if (mod->getSavedValue<int>("fps-profile", 0) < 1) {
        mod->setSettingValue<int64_t>("min-fps", 5);
        mod->setSavedValue<int>("fps-profile", 1);
    }
    // One-time (2.5.3): the old 100x target capped the work done per frame far below what a PC can do (about 24,000 ticks per second).
    if (mod->getSavedValue<int>("speed-profile", 0) < 3) {
        mod->setSettingValue<int64_t>("analysis-speed", 1000);
        mod->setSavedValue<int>("speed-profile", 3);
    }
    // One-time (2.5.3): the reference counter measures at most 5 ticks each way; match it.
    if (mod->getSavedValue<int>("speed-profile", 0) < 4) {
        mod->setSettingValue<int64_t>("max-shift", 5);
        mod->setSavedValue<int>("speed-profile", 4);
    }
    // One-time: measure every release as well as every click.
    if (mod->getSavedValue<int>("release-profile", 0) < 2) {
        mod->setSettingValue<std::string>("release-timing", "Always");
        mod->setSavedValue<int>("release-profile", 2);
    }
    // One-time: row 8 is pink for 12-15 frames and row 9 (16+) is a greyish white.
    if (mod->getSavedValue<int>("colour-profile", 0) < 1) {
        if (mod->getSettingValue<std::string>("row-8-range") == "13-15") mod->setSettingValue<std::string>("row-8-range", "12-15");
        if (mod->getSettingValue<std::string>("row-9-range") == "16+") {
            for (char const* k : {"row-9-label-color", "row-9-count-color", "row-9-marker-color"})
                mod->setSettingValue<cocos2d::ccColor3B>(k, {226, 226, 232});
        }
        mod->setSavedValue<int>("colour-profile", 1);
    }

#define WP_B(f, k) f = mod->getSettingValue<bool>(k);
#define WP_I(f, k) f = static_cast<int>(mod->getSettingValue<int64_t>(k));
#define WP_D(f, k) f = mod->getSettingValue<double>(k);
#define WP_S(f, k) f = mod->getSettingValue<std::string>(k);
#define WP_C(f, k) f = mod->getSettingValue<ccColor3B>(k);
#include "CfgFields.inc"
#undef WP_B
#undef WP_I
#undef WP_D
#undef WP_S
#undef WP_C

    buckets.clear();
    for (int n = 1; n <= 9; ++n) {
        auto key = [n](char const* tail) { return fmt::format("row-{}-{}", n, tail); };
        if (!mod->getSettingValue<bool>(key("visible"))) continue;
        Bucket b;
        std::string range = mod->getSettingValue<std::string>(key("range"));
        if (!parseRange(range, b.lo, b.hi)) continue;   // blank / invalid range disables the row
        b.label = trim(mod->getSettingValue<std::string>(key("label")));
        if (b.label.empty()) b.label = trim(range);
        b.extended = mod->getSettingValue<bool>(key("extended"));
        b.labelColor = toRgb(mod->getSettingValue<ccColor3B>(key("label-color")));
        b.countColor = toRgb(mod->getSettingValue<ccColor3B>(key("count-color")));
        b.markerColor = toRgb(mod->getSettingValue<ccColor3B>(key("marker-color")));
        buckets.push_back(std::move(b));
    }
    std::stable_sort(buckets.begin(), buckets.end(),
                     [](Bucket const& a, Bucket const& b) { return a.lo > b.lo; });

    // "Timings to show": "1,2,3,4,5-6,7-8,9-12" replaces the rows by exactly these entries. Colours come from the
    // bucket-editor row with the same range, else from the row with the nearest lower start.
    {
        std::vector<Bucket> rows = buckets;   // sorted highest first
        std::vector<Bucket> listed;
        std::string token;
        auto flush = [&]() {
            std::string tok = trim(token);
            token.clear();
            if (tok.empty()) return;
            Bucket b;
            if (!parseRange(tok, b.lo, b.hi)) return;   // ignore what cannot be read
            b.label = tok;
            const Bucket* src = nullptr;
            for (auto const& r : rows) if (r.lo == b.lo && r.hi == b.hi) { src = &r; break; }
            if (!src) {
                // nearest row that starts at or below this entry (rows are sorted highest first)
                for (auto const& r : rows) if (r.lo <= b.lo) { src = &r; break; }
                if (!src && !rows.empty()) src = &rows.back();
            }
            if (src) {
                b.labelColor = src->labelColor;
                b.countColor = src->countColor;
                b.markerColor = src->markerColor;
            } else {
                b.labelColor = b.countColor = b.markerColor = Rgb{255, 255, 255};
            }
            listed.push_back(std::move(b));
        };
        for (char ch : timings) {
            if (ch == ',' || ch == ';' || ch == ' ' || ch == '\n') flush();
            else token.push_back(ch);
        }
        flush();
        if (!listed.empty()) {
            buckets = std::move(listed);
            std::stable_sort(buckets.begin(), buckets.end(), [](Bucket const& a, Bucket const& b) { return a.lo > b.lo; });
        }
    }

    // Colours by rank, not by number: the easiest shown window (the biggest) is light purple, the hardest (the smallest) is red, and
    // everything shown in between is spread over the same rainbow, however many timings are listed.
    if (rankColours && !buckets.empty()) {
        static const Rgb kRamp[] = {{190, 150, 255}, {135, 135, 255}, {80, 170, 255}, {90, 225, 255}, {110, 230, 110},
                                    {255, 235, 90},  {255, 160, 60},  {255, 100, 80}, {255, 64, 64}};
        constexpr int kLast = static_cast<int>(sizeof(kRamp) / sizeof(kRamp[0])) - 1;
        const size_t n = buckets.size();
        for (size_t i = 0; i < n; ++i) {   // buckets are sorted easiest (highest) first
            const double t = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) * kLast : 0.0;
            const int a = std::min(kLast, static_cast<int>(t));
            const int b2 = std::min(kLast, a + 1);
            const double f = t - a;
            auto mix = [&](uint8_t x, uint8_t y) { return static_cast<uint8_t>(std::lround(x + (y - x) * f)); };
            const Rgb col{mix(kRamp[a].r, kRamp[b2].r), mix(kRamp[a].g, kRamp[b2].g), mix(kRamp[a].b, kRamp[b2].b)};
            buckets[i].labelColor = buckets[i].countColor = buckets[i].markerColor = col;
        }
    }

    ++revision;
}

int Cfg::displayFpsValue() const {
    if (displayFps == "custom") return std::clamp(displayFpsCustom, 10, 1000);
    int v = std::atoi(displayFps.c_str());
    return v > 0 ? v : 60;
}

int Cfg::windowDecimals() const {
    return cbfMode ? std::max(decimals, 2) : decimals;
}

int bucketIndexFor(Cfg const& c, double window) {
    for (size_t i = 0; i < c.buckets.size(); ++i)
        if (c.buckets[i].contains(window)) return static_cast<int>(i);
    return -1;
}

}  // namespace wp
