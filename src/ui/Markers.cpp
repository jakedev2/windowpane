#include "Ui.hpp"

#include "../core/Cfg.hpp"
#include "../core/Session.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <unordered_map>

using namespace geode::prelude;

namespace wp::ui {

namespace {

constexpr int kTex = 128;

float smooth(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Procedurally drawn white shape with a black outline; tinted at runtime with setColor().
//   ring:  band between 62% and 84% of the radius
//   core:  disc of 40% of the radius
// Straight (non-premultiplied) alpha: outline coverage is composited under the white fill.
Ref<CCTexture2D> makeTexture(bool ring) {
    std::vector<uint8_t> px(static_cast<size_t>(kTex) * kTex * 4, 0);
    const float R = kTex * 0.5f - 1.f;
    const float inner = ring ? 0.56f * R : 0.f, outer = ring ? 0.92f * R : 0.42f * R;   // thick band, like the reference
    const float ow = 0.065f * R;  // outline width
    for (int y = 0; y < kTex; ++y) {
        for (int x = 0; x < kTex; ++x) {
            const float d = std::hypot(x + 0.5f - kTex * 0.5f, y + 0.5f - kTex * 0.5f);
            float fill, fullShape;
            if (ring) {
                fill = smooth(inner - 0.7f, inner + 0.7f, d) * (1.f - smooth(outer - 0.7f, outer + 0.7f, d));
                fullShape = smooth(inner - ow - 0.7f, inner - ow + 0.7f, d) * (1.f - smooth(outer + ow - 0.7f, outer + ow + 0.7f, d));
            } else {
                fill = 1.f - smooth(outer - 0.7f, outer + 0.7f, d);
                fullShape = 1.f - smooth(outer + ow - 0.7f, outer + ow + 0.7f, d);
            }
            const float a = fill + fullShape * (1.f - fill);
            const float v = a > 1e-4f ? fill / a : 0.f;   // white share inside the shape, black elsewhere
            auto* p = &px[(static_cast<size_t>(y) * kTex + x) * 4];
            p[0] = p[1] = p[2] = static_cast<uint8_t>(std::clamp(v * 255.f, 0.f, 255.f));
            p[3] = static_cast<uint8_t>(std::clamp(a * 255.f, 0.f, 255.f));
        }
    }
    auto* tex = new CCTexture2D();
    if (!tex->initWithData(px.data(), kCCTexture2DPixelFormat_RGBA8888, kTex, kTex, CCSize(kTex, kTex))) {
        delete tex;
        return nullptr;
    }
    tex->autorelease();
    return tex;
}

constexpr float kRunSpeed = 311.f;   // object-layer units per second at 1x speed: turns "seconds" into distance

struct Marker {
    Ref<CCNode> root;
    CCSprite* ring = nullptr;
    CCSprite* core = nullptr;
    CCLabelBMFont* num = nullptr;
    int input = -1;
};

}  // namespace

class Markers : public CCNode {
public:
    static Markers* create() {
        auto* m = new Markers();
        if (m && m->init()) { m->autorelease(); return m; }
        delete m;
        return nullptr;
    }

    void frame(PlayLayer* pl) {
        auto& c = cfg();
        auto& S = Session::get();
        auto res = S.result();
        const bool on = c.markerEnabled && shouldShow(pl) && res && !res->inputs.empty() && S.macro();
        if (!on) { releaseAll(); return; }
        if (!m_ringTex) {
            m_ringTex = makeTexture(true);
            m_coreTex = makeTexture(false);
            if (!m_ringTex || !m_coreTex) { log::error("[Windowpane] could not create marker textures"); return; }
        }
        S.refresh();

        auto* layer = pl->m_objectLayer;
        if (getParent() != layer) {
            removeFromParentAndCleanup(false);
            layer->addChild(this, 100000);
        }

        const auto win = CCDirector::get()->getWinSize();
        const float lscale = std::max(0.05f, layer->getScale());
        const CCPoint lpos = layer->getPosition();
        const float pad = win.height * 0.12f;
        const double now = ticksOf(pl->m_gameState.m_currentProgress) / tps();
        const float px = pl->m_player1 ? pl->m_player1->getPositionX() : 0.f;
        auto const& macro = *S.macro();

        // collect inputs to show
        std::vector<int> want;
        for (size_t i = 0; i < res->inputs.size() && i < macro.inputs.size(); ++i) {
            auto const& r = res->inputs[i];
            if (r.status != InputStatus::Measured && r.status != InputStatus::Capped) continue;
            auto const& e = macro.inputs[i];
            if (c.markerKinds == "Presses only" && !e.down) continue;
            if (c.markerKinds == "Releases only" && e.down) continue;
            if (c.markerFilterBelow > 0 && r.window >= c.markerFilterBelow) continue;
            if (Session::get().bucketOf(i) < 0) continue;   // not one of the "Timings to show"
            const float sx = r.x * lscale + lpos.x, sy = r.y * lscale + lpos.y;
            if (sx < -pad || sx > win.width + pad || sy < -pad || sy > win.height + pad) continue;
            // The ring appears at the moment the player reaches the click and fades as they move on.
            if (px < r.x) continue;
            if (c.markerLifetime > 0.0 && px - r.x > c.markerLifetime * kRunSpeed) continue;
            want.push_back(static_cast<int>(i));
        }
        const size_t cap = static_cast<size_t>(std::max(4, c.markerMax));
        if (want.size() > cap) {
            std::sort(want.begin(), want.end(), [&](int a, int b) {
                return std::abs(res->inputs[a].x - px) < std::abs(res->inputs[b].x - px);
            });
            want.resize(cap);
        }

        // recycle markers that are no longer wanted
        std::unordered_map<int, bool> wanted;
        for (int i : want) wanted[i] = true;
        for (auto it = m_active.begin(); it != m_active.end();) {
            if (!wanted.count(it->first)) {
                it->second.root->setVisible(false);
                it->second.input = -1;
                m_free.push_back(std::move(it->second));
                it = m_active.erase(it);
            } else {
                ++it;
            }
        }

        const float base = win.height * 0.13f * static_cast<float>(c.markerSize);   // ring diameter on screen
        for (int i : want) {
            auto& r = res->inputs[i];
            auto it = m_active.find(i);
            if (it == m_active.end()) {
                Marker m;
                if (!m_free.empty()) { m = std::move(m_free.back()); m_free.pop_back(); }
                else m = build();
                m.input = i;
                it = m_active.emplace(i, std::move(m)).first;
            }
            style(it->second, r, c, base, lscale, now);
        }
    }

    void releaseAll() {
        for (auto& [i, m] : m_active) { m.root->setVisible(false); m.input = -1; m_free.push_back(std::move(m)); }
        m_active.clear();
    }

private:
    Ref<CCTexture2D> m_ringTex, m_coreTex;
    std::unordered_map<int, Marker> m_active;
    std::vector<Marker> m_free;

    Marker build() {
        Marker m;
        m.root = CCNode::create();
        m.ring = CCSprite::createWithTexture(m_ringTex);
        m.core = CCSprite::createWithTexture(m_coreTex);
        m.num = CCLabelBMFont::create("", "bigFont.fnt");
        m.root->addChild(m.ring, 1);
        m.root->addChild(m.core, 2);
        m.root->addChild(m.num, 3);
        this->addChild(m.root);
        return m;
    }

    void style(Marker& m, InputResult const& r, Cfg const& c, float base, float lscale, double now) {
        auto& S = Session::get();
        const int b = S.bucketOf(static_cast<size_t>(m.input));
        const Rgb bc = b >= 0 ? c.buckets[b].markerColor : Rgb{255, 255, 255};
        const float s = base / lscale;               // marker diameter in object-layer units
        m.root->setPosition({r.x, r.y});
        m.root->setVisible(true);
        m.ring->setScale(s / kTex);
        m.core->setScale(s / kTex);
        m.ring->setColor({bc.r, bc.g, bc.b});   // the ring always has the number's colour
        m.core->setVisible(false);   // the reference marker is a plain ring + number
        const std::string text = fmt::format("{:.{}f}", r.window, c.windowDecimals());
        if (text != m.num->getString()) m.num->setString(text.c_str());
        m.num->setColor({bc.r, bc.g, bc.b});
        // digits about half as tall as the ring is wide, whatever the font metrics
        const float ch = std::max(1.f, m.num->getContentSize().height);
        m.num->setScale(0.5f * s * static_cast<float>(c.markerNumberSize) / ch);
        const float off = s * (0.5f + static_cast<float>(c.markerTextDistance));   // 0 = the number touches the ring
        if (c.markerNumberPos == "Right") { m.num->setAnchorPoint({0, 0.5f}); m.num->setPosition({off, 0}); }
        else if (c.markerNumberPos == "Above") { m.num->setAnchorPoint({0.5f, 0}); m.num->setPosition({0, off}); }
        else { m.num->setAnchorPoint({1, 0.5f}); m.num->setPosition({-off, 0}); }

        float alpha = 1.f;
        if (c.markerFade) {
            if (c.markerLifetime > 0.0) {
                const float px = pl1x();
                alpha = std::clamp(1.f - (px - r.x) / (static_cast<float>(c.markerLifetime) * kRunSpeed), 0.f, 1.f);
            }
            else {
                // fade out as the marker slides to the left edge of the screen
                const float sx = r.x * lscale + m_parentX();
                alpha = std::clamp(sx / (CCDirector::get()->getWinSize().width * 0.25f), 0.f, 1.f);
            }
        }
        const GLubyte o = static_cast<GLubyte>(alpha * 255.f);
        m.ring->setOpacity(o);
        m.num->setOpacity(o);
    }
    float pl1x() {
        auto* pl = PlayLayer::get();
        return pl && pl->m_player1 ? pl->m_player1->getPositionX() : 0.f;
    }
    float m_parentX() { return getParent() ? getParent()->getPositionX() : 0.f; }
};

static Ref<Markers> s_markers;

void markersAttach(PlayLayer* pl) {
    if (!pl || !pl->m_objectLayer) return;
    s_markers = Markers::create();
    if (s_markers) pl->m_objectLayer->addChild(s_markers, 100000);
}
void markersFrame(PlayLayer* pl) { if (s_markers) s_markers->frame(pl); }
void markersDetach() {
    if (s_markers) s_markers->removeFromParentAndCleanup(true);
    s_markers = nullptr;
}

}  // namespace wp::ui
