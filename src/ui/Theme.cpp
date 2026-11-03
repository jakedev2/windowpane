#include "Theme.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace wp::ui {

namespace {

struct Pal { char const* name; ccColor3B c; };
constexpr Pal kPal[kPaletteCount] = {
    {"Cyan", {34, 200, 255}},   {"Blue", {64, 120, 255}},   {"Purple", {168, 64, 255}}, {"Magenta", {255, 80, 200}},
    {"Red", {255, 84, 96}},     {"Orange", {255, 148, 52}}, {"Yellow", {255, 210, 64}}, {"Green", {52, 208, 120}},
    {"Teal", {36, 206, 190}},   {"Slate", {150, 164, 200}},
};

struct Preset { char const* name; int a, b; };
constexpr Preset kPre[kPresetCount] = {
    {"Neon", 0, 2}, {"Ocean", 1, 8}, {"Sunset", 5, 3}, {"Forest", 7, 8}, {"Candy", 3, 2}, {"Fire", 6, 4},
};

void load(Theme& t) {
    auto* m = Mod::get();
    t.dark = m->getSavedValue<bool>("ui-dark", true);
    const int i1 = std::clamp(m->getSavedValue<int>("ui-a1", 0), 0, kPaletteCount - 1);
    const int i2 = std::clamp(m->getSavedValue<int>("ui-a2", 2), 0, kPaletteCount - 1);
    t.a1 = kPal[i1].c;
    t.a2 = kPal[i2].c;
    if (t.dark) {
        t.bg = {6, 10, 20};      t.card = {18, 30, 58};   t.cardHi = {27, 50, 88};   t.pill = {38, 64, 90};
        t.line = {44, 62, 100};  t.text = {240, 244, 255}; t.muted = {128, 144, 184};
        t.good = {64, 214, 128}; t.warn = {255, 190, 84};  t.bad = {255, 92, 92};
    } else {
        t.bg = {230, 235, 247};  t.card = {244, 247, 255}; t.cardHi = {255, 255, 255}; t.pill = {219, 226, 244};
        t.line = {198, 207, 232}; t.text = {24, 30, 56};   t.muted = {104, 116, 150};
        t.good = {30, 170, 96};  t.warn = {214, 140, 20};  t.bad = {220, 60, 70};
    }
}

Theme& mut() {
    static Theme t;
    static bool loaded = false;
    if (!loaded) { loaded = true; load(t); }
    return t;
}

// CCDrawNode::drawPolygon builds its edge normals from neighbouring points; two (nearly) identical points give a
// zero-length edge and a broken normal, which shows up as a spike on the shape. Drop such points (also across the
// wrap-around from the last point to the first).
void dropDuplicatePts(std::vector<CCPoint>& v) {
    constexpr float kEps = 0.01f;
    auto same = [&](CCPoint const& a, CCPoint const& b) {
        return std::fabs(a.x - b.x) < kEps && std::fabs(a.y - b.y) < kEps;
    };
    std::vector<CCPoint> out;
    out.reserve(v.size());
    for (auto const& p : v) {
        if (out.empty() || !same(out.back(), p)) out.push_back(p);
    }
    while (out.size() > 1 && same(out.back(), out.front())) out.pop_back();
    v.swap(out);
}

std::vector<CCPoint> roundPts(float w, float h, float r) {
    std::vector<CCPoint> v;
    r = std::max(0.f, std::min(r, std::min(w, h) / 2.f));
    if (r < 0.5f) {
        v.push_back(CCPoint(0.f, 0.f));
        v.push_back(CCPoint(w, 0.f));
        v.push_back(CCPoint(w, h));
        v.push_back(CCPoint(0.f, h));
        return v;
    }
    const int segs = std::clamp(static_cast<int>(r), 4, 14);
    constexpr float kHalfPi = 1.57079633f;
    auto arc = [&](float cx, float cy, float a0) {
        for (int i = 0; i <= segs; ++i) {
            const float a = a0 + kHalfPi * static_cast<float>(i) / static_cast<float>(segs);
            v.push_back(CCPoint(cx + std::cos(a) * r, cy + std::sin(a) * r));
        }
    };
    arc(w - r, r, -kHalfPi);        // bottom right
    arc(w - r, h - r, 0.f);         // top right
    arc(r, h - r, kHalfPi);         // top left
    arc(r, r, 2.f * kHalfPi);       // bottom left
    dropDuplicatePts(v);            // pills and circles (r == h/2 or w/2) repeat the point where two arcs meet
    return v;
}

CCDrawNode* polyNode(float w, float h, float r, ccColor3B c, GLubyte a) {
    auto* d = CCDrawNode::create();
    auto pts = roundPts(w, h, r);
    d->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()),
                   ccc4f(c.r / 255.f, c.g / 255.f, c.b / 255.f, a / 255.f), 0.f, ccc4f(0.f, 0.f, 0.f, 0.f));
    return d;
}

CCNode* base(float w, float h) {
    auto* n = CCNode::create();
    n->setContentSize({w, h});
    n->setAnchorPoint({0.5f, 0.5f});
    return n;
}

}  // namespace

Theme const& theme() { return mut(); }
void reloadTheme() { load(mut()); }

void setThemeDark(bool dark) {
    Mod::get()->setSavedValue<bool>("ui-dark", dark);
    reloadTheme();
}
void setThemeAccent(int which, int idx) {
    Mod::get()->setSavedValue<int>(which == 0 ? "ui-a1" : "ui-a2", std::clamp(idx, 0, kPaletteCount - 1));
    reloadTheme();
}
void setThemePreset(int p) {
    p = std::clamp(p, 0, kPresetCount - 1);
    Mod::get()->setSavedValue<int>("ui-a1", kPre[p].a);
    Mod::get()->setSavedValue<int>("ui-a2", kPre[p].b);
    reloadTheme();
}
int themeAccentIndex(int which) {
    return std::clamp(Mod::get()->getSavedValue<int>(which == 0 ? "ui-a1" : "ui-a2", which == 0 ? 0 : 2), 0, kPaletteCount - 1);
}

ccColor3B paletteColor(int i) { return kPal[std::clamp(i, 0, kPaletteCount - 1)].c; }
char const* paletteName(int i) { return kPal[std::clamp(i, 0, kPaletteCount - 1)].name; }
char const* presetName(int i) { return kPre[std::clamp(i, 0, kPresetCount - 1)].name; }
void presetColors(int i, int& a, int& b) {
    i = std::clamp(i, 0, kPresetCount - 1);
    a = kPre[i].a;
    b = kPre[i].b;
}

CCNode* shapeRound(float w, float h, float r, ccColor3B c, GLubyte a) {
    auto* n = base(w, h);
    n->addChild(polyNode(w, h, r, c, a));
    return n;
}

CCNode* shapeGrad(float w, float h, float r, ccColor3B c1, ccColor3B c2, bool horizontal, GLubyte a) {
    auto* n = base(w, h);
    // The rounded shape is a stencil; the gradient layer is only drawn where the stencil is.
    auto* stencil = polyNode(w, h, r, {255, 255, 255}, 255);
    auto* clip = CCClippingNode::create(stencil);
    auto* g = CCLayerGradient::create(ccc4(c1.r, c1.g, c1.b, a), ccc4(c2.r, c2.g, c2.b, a));
    g->setContentSize({w, h});
    g->setVector(horizontal ? ccp(1.f, 0.f) : ccp(0.f, -1.f));
    clip->addChild(g);
    n->addChild(clip);
    return n;
}

void fitText(CCLabelBMFont* l, float maxW, float maxH, float maxScale) {
    auto cs = l->getContentSize();
    float s = maxScale;
    if (cs.width > 0.f) s = std::min(s, maxW / cs.width);
    if (cs.height > 0.f) s = std::min(s, maxH / cs.height);
    l->setScale(std::max(s, 0.05f));
}

}  // namespace wp::ui
