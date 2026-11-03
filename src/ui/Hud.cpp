#include "Ui.hpp"

#include "../core/Cfg.hpp"
#include "../core/Session.hpp"
#include "../sim/Analyzer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fmt/format.h>
#include <map>
#include <vector>

using namespace geode::prelude;

namespace wp::ui {

namespace {

// Soft white radial glow used behind a count that just went up (the "flash").
CCTexture2D* glowTexture() {
    static Ref<CCTexture2D> tex;
    if (tex) return tex;
    constexpr int kG = 64;
    std::vector<uint8_t> px(static_cast<size_t>(kG) * kG * 4);
    for (int y = 0; y < kG; ++y) {
        for (int x = 0; x < kG; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) / kG * 2.f - 1.f;
            const float dy = (static_cast<float>(y) + 0.5f) / kG * 2.f - 1.f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float a = std::pow(std::clamp(1.f - d, 0.f, 1.f), 1.7f);
            auto* p = &px[(static_cast<size_t>(y) * kG + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(std::clamp(a * 255.f, 0.f, 255.f));
        }
    }
    auto* t = new CCTexture2D();
    if (!t->initWithData(px.data(), kCCTexture2DPixelFormat_RGBA8888, kG, kG, CCSize(kG, kG))) {
        delete t;
        return nullptr;
    }
    t->autorelease();
    tex = t;
    return tex;
}

// A text label with an optional four-copy black outline (settings: "Outline").
struct Txt {
    CCLabelBMFont* main = nullptr;
    CCLabelBMFont* sh[4] = {nullptr, nullptr, nullptr, nullptr};
    std::string text;

    void set(std::string const& s) {
        if (!main || s == text) return;
        text = s;
        main->setString(s.c_str());
        for (auto* c : sh) if (c) c->setString(s.c_str());
    }
};

Txt makeTxt(CCNode* parent, std::string const& s, char const* font, float scale, ccColor3B col, float outline) {
    Txt t;
    t.main = CCLabelBMFont::create(s.c_str(), font);
    t.main->setScale(scale);
    t.main->setColor(col);
    t.text = s;
    if (outline > 0.f) {
        const float o = outline;
        const CCPoint offs[4] = {{o, 0}, {-o, 0}, {0, o}, {0, -o}};
        for (int i = 0; i < 4; ++i) {
            auto* c = CCLabelBMFont::create(s.c_str(), font);
            c->setColor({0, 0, 0});
            c->setAnchorPoint({0, 0});
            c->setPosition(offs[i]);
            c->setZOrder(-1);
            t.main->addChild(c);
            t.sh[i] = c;
        }
    }
    parent->addChild(t.main);
    return t;
}

ccColor3B toCc(Rgb c) { return {c.r, c.g, c.b}; }

std::string rangeText(Bucket const& b) {
    if (b.hi == kOpen) return fmt::format("{}+", b.lo);
    if (b.lo == b.hi) return std::to_string(b.lo);
    return fmt::format("{}-{}", b.lo, b.hi);
}

}  // namespace

bool shouldShow(PlayLayer* pl) {
    auto& c = cfg();
    if (!pl || !c.enabled) return false;
    if (wp::sim::SimState::get().trialActive) return false;
    if (pl->m_isTestMode) return c.showPlayback;
    return pl->m_isPracticeMode ? c.showPractice : c.showNormal;
}

class Hud : public CCNode {
public:
    static Hud* create() {
        auto* h = new Hud();
        if (h && h->init()) { h->autorelease(); return h; }
        delete h;
        return nullptr;
    }

    void frame(PlayLayer* pl) {
        auto& c = cfg();
        auto& S = Session::get();
        S.refresh();
        const bool show = shouldShow(pl) && S.playbackOn();   // Playback OFF hides the sidebar, the caption and the readout
        m_sidebar->setVisible(false);
        m_caption->setVisible(false);
        m_readout->setVisible(false);

        if (c.screenshotMode) applyScreenshotMode(pl, true);
        else if (m_screenshotApplied) applyScreenshotMode(pl, false);

        const auto win = CCDirector::get()->getWinSize();
        const uint32_t tick = wp::ticksOf(pl->m_gameState.m_currentProgress);

        // ---- REC indicator while recording (practice mode) ----
        if (S.recording()) {
            if (!m_rec) {
                m_rec = CCLabelBMFont::create("REC", "bigFont.fnt");
                m_rec->setColor({255, 60, 60});
                m_rec->setScale(.4f);
                this->addChild(m_rec, 5);
            }
            const double sp = S.recordSpeed();
            std::string t = sp < 0.999 ? fmt::format("REC {:g}x", sp) : std::string("REC");
            if (t != m_recText) { m_recText = t; m_rec->setString(t.c_str()); }
            m_rec->setPosition({win.width - 46.f, win.height - 22.f});
            m_rec->setVisible(true);
        } else if (m_rec) {
            m_rec->setVisible(false);
        }

        // ---- NaN caption (under the vanilla percentage label) ----
        if (show && c.nanEnabled && pl->m_percentageLabel) updateCaption(pl);

        // ---- readout ----
        if (show && c.readoutEnabled) {
            if (!m_readoutTxt.main) {
                m_readoutTxt = makeTxt(m_readout, "", "bigFont.fnt", 0.4f, {255, 255, 255}, 0.f);
                m_readoutTxt.main->setAnchorPoint({0, 0});
            }
            m_readoutTxt.main->setColor(c.readoutColor);
            m_readout->setScale(static_cast<float>(c.readoutScale));
            std::string t = fmt::format("{:.1f}%", pl->getCurrentPercent());
            if (c.readoutTime) {
                const double s = tick / tps();
                t += fmt::format("\n{}:{:06.3f}", static_cast<int>(s) / 60, std::fmod(s, 60.0));
            }
            m_readoutTxt.set(t);
            m_readout->setPosition({static_cast<float>(c.readoutX / 100.0 * win.width), static_cast<float>(c.readoutY / 100.0 * win.height)});
            m_readout->setVisible(true);
        }

        // ---- sidebar ----
        if (!show || !c.sidebarEnabled || !S.hasResult()) { m_alpha = 0.f; return; }
        m_sidebar->setVisible(true);
        const size_t shown = S.inputsUpTo(tick);
        auto const& counts = S.countsAt(shown);
        auto const& totals = S.totals();
        layoutSidebar(win, counts, totals, S.sigmaPerSec());

        const float target = 1.f;
        m_alpha = std::min(target, m_alpha + 0.06f);
        const float a = c.sidebarAnim == "None" ? 1.f : m_alpha;
        m_sidebar->setOpacity(static_cast<GLubyte>(std::clamp(c.sidebarOpacity * 2.55 * a, 0.0, 255.0)));
        if (c.sidebarAnim == "Slide") m_sidebar->setPositionX(m_sidebarBaseX + (1.f - a) * -40.f * (m_rightSide ? -1.f : 1.f));
    }

private:
    CCNodeRGBA* m_sidebar = nullptr;
    Ref<CCNode> m_caption;   // retained: it is re-parented onto the percentage label's parent, and
                             // removeFromParent would otherwise free it
    CCNode* m_readout = nullptr;
    CCLabelBMFont* m_rec = nullptr;
    std::string m_recText;
    Txt m_readoutTxt;
    float m_alpha = 0.f;
    float m_sidebarBaseX = 0.f;
    bool m_rightSide = false;
    bool m_screenshotApplied = false;

    // sidebar rows
    struct Row {
        int bucket = -1;      // index into cfg().buckets, -2 = total, -3 = sigma
        Txt label, count;
        float labelW = 0.f;
        CCSprite* glow = nullptr;     // flash halo behind the count
        ccColor3B baseColor{255, 255, 255};
    };
    struct Flash {
        int last = 0;
        double age = 99.0;   // seconds since the count last went up
    };
    std::map<int, Flash> m_flash;
    std::vector<Row> m_rows;
    std::vector<int> m_rowKey;
    uint64_t m_rowRev = ~0ull;
    const void* m_rowResult = nullptr;
    std::string m_rowFont;
    float m_rowOutline = -1.f;
    float m_rowTextScale = -1.f;

    // caption
    std::string m_capKey;

    bool init() override {
        if (!CCNode::init()) return false;
        m_sidebar = CCNodeRGBA::create();
        m_sidebar->setCascadeOpacityEnabled(true);
        m_sidebar->setCascadeColorEnabled(false);
        m_caption = CCNode::create();
        m_readout = CCNode::create();
        this->addChild(m_sidebar);
        this->addChild(m_caption);
        this->addChild(m_readout);
        return true;
    }

public:
    // The caption may live under the percentage label's parent instead of this node; take it out with us.
    void detachCaption() {
        if (m_caption) m_caption->removeFromParentAndCleanup(true);
    }
private:

    void applyScreenshotMode(PlayLayer* pl, bool on) {
        m_screenshotApplied = on;
        if (auto* ui = pl->m_uiLayer) {
            if (ui->m_pauseBtn) ui->m_pauseBtn->setVisible(!on);
            if (ui->m_checkpointMenu) ui->m_checkpointMenu->setVisible(!on);
        }
        if (pl->m_attemptLabel) pl->m_attemptLabel->setVisible(!on);
    }

    // ---- small-caps caption -------------------------------------------------------------------------
    // GD's bitmap fonts have no small capitals, so the text is split into runs: lowercase letters are
    // drawn as capitals at ~78% scale, everything else at full scale. The runs are laid out left to
    // right and the baselines are matched by shifting the smaller runs down (a capital's centre sits
    // roughly 0.35 line-heights above the baseline).
    void updateCaption(PlayLayer* pl) {
        auto& c = cfg();
        auto* pct = pl->m_percentageLabel;
        const std::string key = fmt::format("{}|{}|{}|{:.3f}|{}|{}", c.nanText, c.nanFont, c.nanSmallCaps, c.nanSize,
                                            c.nanColor.r * 65536 + c.nanColor.g * 256 + c.nanColor.b, c.nanOpacity);
        if (key != m_capKey) {
            m_capKey = key;
            m_caption->removeAllChildren();
            struct Run { std::string text; bool isSmall; };
            std::vector<Run> runs;
            for (char ch : c.nanText) {
                const bool isSm = c.nanSmallCaps && std::islower(static_cast<unsigned char>(ch));
                const char out = isSm ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : ch;
                if (runs.empty() || runs.back().isSmall != isSm) runs.push_back({std::string(1, out), isSm});
                else runs.back().text.push_back(out);
            }
            const float big = static_cast<float>(c.nanSize);
            const float smallS = big * 0.78f;
            float x = 0.f, h = 0.f;
            std::vector<CCLabelBMFont*> made;
            for (auto const& r : runs) {
                auto* l = CCLabelBMFont::create(r.text.c_str(), c.nanFont.c_str());
                const float s = r.isSmall ? smallS : big;
                l->setScale(s);
                l->setAnchorPoint({0, 0.5f});
                l->setColor(c.nanColor);
                l->setOpacity(static_cast<GLubyte>(std::clamp(c.nanOpacity * 2.55, 0.0, 255.0)));
                const float lineH = l->getContentSize().height;
                l->setPosition({x, r.isSmall ? -0.35f * lineH * (big - smallS) : 0.f});
                x += l->getContentSize().width * s;
                h = std::max(h, lineH * big);
                m_caption->addChild(l);
                made.push_back(l);
            }
            // centre the block under the percentage label
            for (auto* l : made) l->setPositionX(l->getPositionX() - x / 2.f);
            m_capH = h;
        }
        auto* want = pct->getParent();
        if (!want) { m_caption->setVisible(false); return; }
        if (m_caption->getParent() != want) {
            m_caption->removeFromParentAndCleanup(false);   // safe: m_caption is a Ref
            want->addChild(m_caption, pct->getZOrder());
        }
        // Not attached to the percentage text: just below the progress bar row, centred on the screen's x axis.
        // (The percentage label sits at the bar's height, so its y is the bar's y.)
        const float ph = pct->getContentSize().height * pct->getScaleY();
        const auto anc = pct->getAnchorPoint();
        const float midY = pct->getPositionY() + (0.5f - anc.y) * ph;
        const auto winSz = CCDirector::get()->getWinSize();
        const float localCx = want->convertToNodeSpace({winSz.width / 2.f, 0.f}).x;
        m_caption->setPosition({localCx + static_cast<float>(c.nanOffsetX), midY - (ph * 0.5f + m_capH * 0.55f) + static_cast<float>(c.nanOffsetY)});
        m_caption->setVisible(true);
    }
    float m_capH = 0.f;

    // ---- sidebar layout --------------------------------------------------------------------------------
    void layoutSidebar(CCSize win, std::vector<int> const& counts, std::vector<int> const& totals, double sigma) {
        auto& c = cfg();
        auto& S = Session::get();
        std::vector<int> key;
        int total = 0, totalAll = 0;
        bool anyExt = false;
        for (size_t i = 0; i < c.buckets.size(); ++i) {
            if (i < totals.size() && totals[i] > 0 && c.buckets[i].extended) anyExt = true;
            total += i < counts.size() ? counts[i] : 0;
            totalAll += i < totals.size() ? totals[i] : 0;
        }
        for (size_t i = 0; i < c.buckets.size(); ++i) {
            auto const& b = c.buckets[i];
            if (b.extended) {
                if (c.extendedRows == "Never") continue;
                if (c.extendedRows == "Auto" && !anyExt) continue;
                if (c.extendedRows == "Auto" && (i >= totals.size() || totals[i] == 0)) continue;
            } else if (!c.showZeroRows && (i >= counts.size() || counts[i] == 0)) {
                continue;
            }
            key.push_back(static_cast<int>(i));
        }
        if (c.showTotalRow) key.push_back(-2);
        if (c.showSigma && sigma > 0) key.push_back(-3);

        const float textScale = static_cast<float>(c.sidebarTextScale);
        const float outline = static_cast<float>(c.sidebarOutline);
        if (key != m_rowKey || c.revision != m_rowRev || S.result().get() != m_rowResult || c.sidebarFont != m_rowFont ||
            outline != m_rowOutline || textScale != m_rowTextScale) {
            m_rowKey = key;
            m_rowRev = c.revision;
            m_rowResult = S.result().get();
            m_rowFont = c.sidebarFont;
            m_rowOutline = outline;
            m_rowTextScale = textScale;
            m_sidebar->removeAllChildren();
            m_rows.clear();
            for (int k : key) {
                Row r;
                r.bucket = k;
                std::string name;
                ccColor3B lc{255, 255, 255}, cc{255, 255, 255};
                // Reference look: "5-6:" and its count share one colour.
                if (k >= 0) { name = c.buckets[k].label + ":"; lc = toCc(c.buckets[k].labelColor); cc = lc; }
                else if (k == -2) { name = "All:"; lc = {200, 200, 200}; cc = lc; }
                else { name = "s/s:"; lc = {200, 200, 200}; cc = lc; }
                r.label = makeTxt(m_sidebar, name, c.sidebarFont.c_str(), textScale, lc, outline);
                r.count = makeTxt(m_sidebar, "0", c.sidebarFont.c_str(), textScale, cc, outline);
                r.labelW = r.label.main->getContentSize().width * textScale;
                r.baseColor = cc;
                if (k != -3) {
                    if (auto* gt = glowTexture()) {
                        r.glow = CCSprite::createWithTexture(gt);
                        r.glow->setOpacity(0);
                        m_sidebar->addChild(r.glow, -1);   // behind the text
                    }
                }
                m_rows.push_back(std::move(r));
            }
        }

        // update texts
        for (auto& r : m_rows) {
            std::string t;
            if (r.bucket >= 0) {
                const int n = static_cast<size_t>(r.bucket) < counts.size() ? counts[r.bucket] : 0;
                const int tot = static_cast<size_t>(r.bucket) < totals.size() ? totals[r.bucket] : 0;
                if (c.countStyle == "Count / Total") t = fmt::format("{}/{}", n, tot);
                else if (c.countStyle == "Percent") t = tot > 0 ? fmt::format("{:.0f}%", 100.0 * n / tot) : "0%";
                else t = std::to_string(n);
            } else if (r.bucket == -2) {
                t = c.countStyle == "Count / Total" ? fmt::format("{}/{}", total, totalAll) : std::to_string(total);
            } else {
                t = fmt::format("{:.1f}", sigma);
            }
            r.count.set(t);
        }

        // geometry (unscaled units; the node itself carries sidebarScale)
        const float pitch = static_cast<float>(win.height * c.sidebarRowSpacing / 100.0);
        float labelCol = 0.f, countCol = 0.f;
        for (auto& r : m_rows) {
            labelCol = std::max(labelCol, r.labelW);
            countCol = std::max(countCol, r.count.main->getContentSize().width * textScale);
        }
        const float gap = pitch * 0.2f;   // label -> count: about one character, like the reference
        const bool right = c.sidebarAlign == "Right" || c.sidebarCorner == "Top-right" || c.sidebarCorner == "Bottom-right";
        m_rightSide = right && c.sidebarAlign != "Left";
        const float scale = static_cast<float>(c.sidebarScale);
        const float blockH = pitch * m_rows.size();
        const float blockW = labelCol + gap + countCol;
        const float mx = static_cast<float>(c.sidebarMarginX / 100.0 * win.width);
        const float my = static_cast<float>(c.sidebarMarginY / 100.0 * win.height);
        float ox, oy;   // top-left of block in screen space
        auto const& corner = c.sidebarCorner;
        if (corner == "Custom") {
            ox = static_cast<float>(c.sidebarCustomX / 100.0 * win.width);
            oy = win.height - static_cast<float>(c.sidebarCustomY / 100.0 * win.height);
        } else {
            ox = (corner == "Top-right" || corner == "Bottom-right") ? win.width - mx - blockW * scale : mx;
            oy = (corner == "Bottom-left" || corner == "Bottom-right") ? my + blockH * scale : win.height - my;
        }
        m_sidebar->setScale(scale);
        m_sidebar->setPosition({ox, oy});
        m_sidebarBaseX = ox;

        float y = -pitch * 0.5f;
        for (auto& r : m_rows) {
            if (c.sidebarAlign == "Right") {
                r.count.main->setAnchorPoint({1, 0.5f});
                r.count.main->setPosition({blockW, y});
                r.label.main->setAnchorPoint({1, 0.5f});
                r.label.main->setPosition({blockW - countCol - gap, y});
            } else {
                const float shift = c.sidebarAlign == "Center" ? 0.f : 0.f;
                r.label.main->setAnchorPoint({0, 0.5f});
                r.label.main->setPosition({shift, y});
                r.count.main->setAnchorPoint({0, 0.5f});
                r.count.main->setPosition({shift + labelCol + gap, y});
            }
            y -= pitch;
        }

        // ---- flash: when a count goes up its number turns near-white with a glow, holds a moment, then fades back ----
        {
            const double dt = std::min(0.1, static_cast<double>(CCDirector::get()->getDeltaTime()));
            constexpr double kHold = 0.22, kFade = 0.50;   // measured from the reference video: ~0.2 s bright, ~0.5 s fade
            const ccColor3B flashColor{255, 238, 220};
            for (auto& r : m_rows) {
                if (r.bucket == -3 || !r.glow) continue;
                int n = 0;
                if (r.bucket >= 0) n = static_cast<size_t>(r.bucket) < counts.size() ? counts[r.bucket] : 0;
                else n = total;
                auto fit = m_flash.find(r.bucket);
                if (fit == m_flash.end()) fit = m_flash.emplace(r.bucket, Flash{n, 99.0}).first;   // first sight: no flash
                auto& fs = fit->second;
                if (n > fs.last) fs.age = 0.0;          // a new click was counted
                else if (n < fs.last) fs.age = 99.0;    // restart / rewind: no flash
                fs.last = n;
                fs.age += dt;
                float k = 0.f;
                if (fs.age < kHold) k = 1.f;
                else if (fs.age < kHold + kFade) {
                    const float u = static_cast<float>((fs.age - kHold) / kFade);
                    k = 1.f - u * u * (3.f - 2.f * u);   // smooth fade
                }
                auto mix = [&](GLubyte a, GLubyte b) { return static_cast<GLubyte>(a + (b - a) * k); };
                r.count.main->setColor({mix(r.baseColor.r, flashColor.r), mix(r.baseColor.g, flashColor.g), mix(r.baseColor.b, flashColor.b)});
                // glow centred on the number
                const float w = r.count.main->getContentSize().width * textScale;
                const float h = r.count.main->getContentSize().height * textScale;
                const auto pos = r.count.main->getPosition();
                const auto anc = r.count.main->getAnchorPoint();
                r.glow->setPosition({pos.x + (0.5f - anc.x) * w, pos.y});
                const float diameter = std::max(w, h) * 2.6f;
                r.glow->setScale(diameter / std::max(1.f, r.glow->getContentSize().width));
                r.glow->setOpacity(static_cast<GLubyte>(std::clamp(k * 150.f, 0.f, 255.f)));
            }
        }
    }
};

static Ref<Hud> s_hud;

void hudAttach(PlayLayer* pl) {
    if (!pl || !pl->m_uiLayer) return;
    s_hud = Hud::create();
    if (s_hud) pl->m_uiLayer->addChild(s_hud, 50);
}
void hudFrame(PlayLayer* pl) { if (s_hud) s_hud->frame(pl); }
void hudDetach() {
    if (s_hud) {
        s_hud->detachCaption();
        s_hud->removeFromParentAndCleanup(true);
    }
    s_hud = nullptr;
}

}  // namespace wp::ui
