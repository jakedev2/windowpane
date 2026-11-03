#include "Ui.hpp"

#include "../core/Cfg.hpp"
#include "../core/Session.hpp"
#include "../import/MacroParser.hpp"
#include "../sim/Analyzer.hpp"

#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Notification.hpp>
#include "PickFile.hpp"
#include "Theme.hpp"
#include "LogoData.hpp"

#ifndef GEODE_IS_WINDOWS
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#endif
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <fmt/format.h>

using namespace geode::prelude;

namespace wp::ui {

namespace {
void toast(std::string const& text, NotificationIcon icon = NotificationIcon::Success) {
    Notification::create(text, icon, 2.5f)->show();
}
void alert(std::string const& title, std::string const& body) {
    FLAlertLayer::create(title.c_str(), body, "OK")->show();
}
}  // namespace

// Loads the picked macro, reports problems / success, then runs `done` (refreshes the menu).
static void finishPick(std::optional<std::filesystem::path> path, MacroType type, std::function<void()> const& done) {
    if (!path) return;   // cancelled
    std::vector<std::string> warnings;
    auto err = Session::get().loadMacro(*path, warnings, type);
    if (!err.empty()) { alert("Could not use this macro", err); return; }
    auto m = Session::get().macro();
    std::string msg = fmt::format("Loaded {} ({} inputs{})", m->name, m->inputs.size(),
                                  type == MacroType::Auto ? std::string() : std::string(", ") + macroTypeName(type));
    Session::get().lastMessage = msg;
    if (!warnings.empty()) {
        std::string w;
        for (auto const& s : warnings) w += "- " + s + "\n";
        alert("Macro loaded with warnings", w);
    } else {
        toast(msg);
    }
    if (done) done();
}

void pickMacroOfType(MacroType type, std::function<void()> done) {
#ifdef GEODE_IS_WINDOWS
    // Native dialog: no coroutines involved.
    std::vector<std::string> patterns;
    for (auto const& e : macroTypeExtensions(type)) patterns.push_back("*" + e);
    // Async: the click handler returns at once, the dialog runs on its own thread, finishPick runs on the main thread.
    if (!nativePickFileAsync(macroTypeName(type), patterns, [done, type](std::optional<std::filesystem::path> p) { finishPick(std::move(p), type, done); }))
        toast("A file dialog is already open.", NotificationIcon::Warning);
#else
    file::FilePickOptions opts;
    file::FilePickOptions::Filter f;
    f.description = macroTypeName(type);
    for (auto const& e : macroTypeExtensions(type)) f.files.insert("*" + e);
    opts.filters.push_back(std::move(f));
    async::spawn(file::pick(file::PickMode::OpenFile, opts), [done = std::move(done), type](Result<std::optional<std::filesystem::path>> res) {
        if (res.isErr()) { alert("Windowpane", res.unwrapErr()); return; }
        finishPick(std::move(res).unwrap(), type, done);
    });
#endif
}

void copySummary() {
    auto s = Session::get().summaryText();
    if (s.empty()) { toast("Nothing to copy yet.", NotificationIcon::Warning); return; }
    utils::clipboard::write(s);
    toast("Summary copied.");
}

void saveJsonNow() {
    std::filesystem::path p;
    auto err = Session::get().saveJson(p);
    if (!err.empty()) { toast(err, NotificationIcon::Error); return; }
    toast("Saved " + p.filename().string());
}

// -------------------------------------------------------------------------------------------------------------
// Progress view: same look as Frame Window Analyzer's analysis screen (black screen, wide bar, big title,
// small stats text). It is still a Popup so it keeps the touch/keyboard handling, and it is still the driver
// of the analyzer: one slice per rendered frame, which keeps working while the game is paused.
static CCTexture2D* logoTexture();

// The loading screen of an analysis, in the look of the Windowpane menu: gradient backdrop with slow colour orbs, one card
// with the logo, the percentage, a gradient bar with a moving shine, six live stat chips and a Stop button.
class ProgressPopup : public Popup {
public:
    static ProgressPopup* create() {
        auto* p = new ProgressPopup();
        if (p->init()) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }

protected:
    struct Chip {
        CCLabelBMFont* value = nullptr;
        std::string shown;
    };
    CCLayerColor* m_stencil = nullptr;
    CCNode* m_shimmer = nullptr;
    CCNode* m_orbA = nullptr;
    CCNode* m_orbB = nullptr;
    CCLabelBMFont* m_percent = nullptr;
    CCLabelBMFont* m_phase = nullptr;
    CCLabelBMFont* m_foot = nullptr;
    Chip m_chips[6];
    CCPoint m_orbA0, m_orbB0;
    float m_barW = 0.f, m_barH = 18.f, m_t = 0.f, m_phaseMaxW = 300.f;
    std::string m_shownPercent, m_shownPhase, m_shownFoot;
    bool m_restoreVisible = false;

    static ccColor3B mixC(ccColor3B a, ccColor3B b, float t) {
        return {static_cast<GLubyte>(a.r + (b.r - a.r) * t), static_cast<GLubyte>(a.g + (b.g - a.g) * t),
                static_cast<GLubyte>(a.b + (b.b - a.b) * t)};
    }
    static std::string clock(double sec) {
        const int s = std::max(0, static_cast<int>(sec));
        return s >= 3600 ? fmt::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60) : fmt::format("{}:{:02}", s / 60, s % 60);
    }

    bool init() {
        auto win = CCDirector::get()->getWinSize();
        if (!Popup::init(win.width, win.height)) return false;
        if (m_bgSprite) m_bgSprite->setVisible(false);
        if (m_closeBtn) m_closeBtn->setVisible(false);
        auto const& T = theme();
        auto sz = m_mainLayer->getContentSize();
        const float cx = sz.width / 2.f, cy = sz.height / 2.f;

        // opaque backdrop (nothing of the level may show through), a soft vertical gradient and two slowly drifting colour orbs
        auto* bg = CCLayerGradient::create(ccc4(T.bg.r, T.bg.g, T.bg.b, 255),
                                           ccc4(mixC(T.bg, T.a2, 0.16f).r, mixC(T.bg, T.a2, 0.16f).g, mixC(T.bg, T.a2, 0.16f).b, 255));
        bg->setContentSize(sz);
        bg->setVector(ccp(0.f, -1.f));
        m_mainLayer->addChild(bg, -10);
        m_orbA0 = CCPoint{sz.width * 0.20f, sz.height * 0.78f};
        m_orbB0 = CCPoint{sz.width * 0.82f, sz.height * 0.22f};
        m_orbA = shapeRound(380.f, 380.f, 190.f, T.a1, 34);
        m_orbA->setPosition(m_orbA0);
        m_mainLayer->addChild(m_orbA, -9);
        m_orbB = shapeRound(460.f, 460.f, 230.f, T.a2, 30);
        m_orbB->setPosition(m_orbB0);
        m_mainLayer->addChild(m_orbB, -9);

        // The whole card sits in one node that is scaled down about the screen centre, so it fits with room around it (at full
        // size it was as tall as the screen). The backdrop and orbs stay full screen.
        const float kScale = std::clamp(win.height * 0.704f / 330.f, 0.44f, 0.77f);
        auto* root = CCNode::create();
        root->setContentSize(sz);
        root->setAnchorPoint({.5f, .5f});
        root->setPosition({cx, cy});
        root->setScale(kScale);
        m_mainLayer->addChild(root, 1);

        // the card
        const float cw = std::min(580.f, sz.width * 0.84f), ch = 330.f;
        const float top = cy + ch / 2.f, bottom = cy - ch / 2.f, left = cx - cw / 2.f, right = cx + cw / 2.f;
        auto* border = shapeGrad(cw + 4.f, ch + 4.f, 22.f, mixC(T.a1, T.card, 0.55f), mixC(T.a2, T.card, 0.55f), true, 255);
        border->setPosition({cx, cy});
        root->addChild(border, 0);
        auto* card = shapeRound(cw, ch, 20.f, T.card, 255);
        card->setPosition({cx, cy});
        root->addChild(card, 1);
        auto* strip = shapeGrad(cw - 44.f, 4.f, 2.f, T.a1, T.a2, true, 255);
        strip->setPosition({cx, top - 12.f});
        root->addChild(strip, 2);

        // header: logo disc, title, phase line, big percentage
        const float hy = top - 58.f;
        auto* disc = shapeGrad(50.f, 50.f, 25.f, T.a1, T.a2, false, 255);
        disc->setPosition({left + 46.f, hy});
        root->addChild(disc, 2);
        if (auto* tex = logoTexture()) {
            auto* logo = CCSprite::createWithTexture(tex);
            logo->setScale(32.f / std::max(1.f, logo->getContentSize().width));
            logo->setPosition({left + 46.f, hy});
            root->addChild(logo, 3);
        }
        auto* title = CCLabelBMFont::create("Analyzing macro", "bigFont.fnt");
        title->setScale(.5f);
        title->setColor(T.text);
        title->setAnchorPoint({0.f, .5f});
        title->setPosition({left + 82.f, hy + 11.f});
        root->addChild(title, 3);
        m_phaseMaxW = cw - 82.f - 140.f;
        m_phase = CCLabelBMFont::create("Starting...", "chatFont.fnt");
        m_phase->setColor(T.muted);
        m_phase->setScale(.6f);
        m_phase->setAnchorPoint({0.f, .5f});
        m_phase->setPosition({left + 82.f, hy - 12.f});
        root->addChild(m_phase, 3);
        m_percent = CCLabelBMFont::create("0%", "bigFont.fnt");
        m_percent->setScale(.85f);
        m_percent->setColor(T.a1);
        m_percent->setAnchorPoint({1.f, .5f});
        m_percent->setPosition({right - 26.f, hy});
        root->addChild(m_percent, 3);

        // progress bar: a track, and a gradient bar revealed by a growing stencil, with a shine sliding over the filled part
        m_barW = cw - 52.f;
        const float bx = left + 26.f, by = top - 128.f;
        auto* track = shapeRound(m_barW, m_barH, m_barH / 2.f, T.line, 255);
        track->setPosition({bx + m_barW / 2.f, by + m_barH / 2.f});
        root->addChild(track, 2);
        m_stencil = CCLayerColor::create({255, 255, 255, 255}, 1.f, m_barH);
        auto* clip = CCClippingNode::create(m_stencil);
        clip->setPosition({bx, by});
        auto* fullBar = shapeGrad(m_barW, m_barH, m_barH / 2.f, T.a1, T.a2, true, 255);
        fullBar->setPosition({m_barW / 2.f, m_barH / 2.f});
        clip->addChild(fullBar);
        m_shimmer = shapeRound(70.f, m_barH, m_barH / 2.f, {255, 255, 255}, 80);
        m_shimmer->setPosition({-60.f, m_barH / 2.f});
        clip->addChild(m_shimmer, 1);
        root->addChild(clip, 3);

        // stat chips, 3 x 2
        static const char* kLabels[6] = {"SPEED", "TRIALS / S", "ETA", "INPUTS", "TRIALS", "ELAPSED"};
        const float gap = 10.f, chipW = (m_barW - 2.f * gap) / 3.f, chipH = 48.f;
        for (int i = 0; i < 6; ++i) {
            const int col = i % 3, row = i / 3;
            const float ccx = bx + chipW / 2.f + static_cast<float>(col) * (chipW + gap);
            const float ccy = by - 40.f - static_cast<float>(row) * (chipH + 8.f);
            auto* chip = shapeRound(chipW, chipH, 12.f, T.cardHi, 255);
            chip->setPosition({ccx, ccy});
            root->addChild(chip, 2);
            auto* lab = CCLabelBMFont::create(kLabels[i], "chatFont.fnt");
            lab->setColor(T.muted);
            lab->setScale(.5f);
            lab->setAnchorPoint({0.f, .5f});
            lab->setPosition({ccx - chipW / 2.f + 14.f, ccy + 11.f});
            root->addChild(lab, 3);
            m_chips[i].value = CCLabelBMFont::create("-", "bigFont.fnt");
            m_chips[i].value->setColor(T.text);
            m_chips[i].value->setScale(.42f);
            m_chips[i].value->setAnchorPoint({0.f, .5f});
            m_chips[i].value->setPosition({ccx - chipW / 2.f + 14.f, ccy - 9.f});
            root->addChild(m_chips[i].value, 3);
        }

        // footer: throttle / CBF note on the left, Stop on the right
        const float fy = bottom + 34.f;
        m_foot = CCLabelBMFont::create("", "chatFont.fnt");
        m_foot->setColor(T.muted);
        m_foot->setScale(.5f);
        m_foot->setAnchorPoint({0.f, .5f});
        m_foot->setPosition({left + 26.f, fy});
        root->addChild(m_foot, 3);
        auto* btn = CCNode::create();
        btn->setContentSize({124.f, 36.f});
        btn->setAnchorPoint({.5f, .5f});
        auto* body = shapeGrad(124.f, 36.f, 18.f, T.bad, mixC(T.bad, T.a2, 0.45f), true, 255);
        body->setPosition({62.f, 18.f});
        btn->addChild(body);
        auto* bl = CCLabelBMFont::create("Stop", "bigFont.fnt");
        bl->setScale(.5f);
        bl->setPosition({62.f, 19.f});
        btn->addChild(bl, 1);
        auto* item = CCMenuItemSpriteExtra::create(btn, this, menu_selector(ProgressPopup::onCancel));
        item->setPosition({right - 26.f - 62.f, fy});
        auto* menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->addChild(item);
        root->addChild(menu, 4);

        // like the other analyzer: nothing of the level is drawn while it runs
        if (auto* pl = Session::get().pl()) {
            m_restoreVisible = pl->isVisible();
            pl->setVisible(false);
            if (pl->m_uiLayer) pl->m_uiLayer->setVisible(false);
        }
        this->scheduleUpdate();
        return true;
    }

    void restoreLevel() {
        if (auto* pl = Session::get().pl()) {
            pl->setVisible(true);
            if (pl->m_uiLayer) pl->m_uiLayer->setVisible(true);
        }
        // The analysis ran with the level resumed (see openProgressPopup); put the player back in the pause menu.
        Loader::get()->queueInMainThread([] {
            if (auto* pl = PlayLayer::get()) if (!pl->m_isPaused) pl->pauseGame(false);
        });
    }

    void setChip(int i, std::string const& v) {
        if (m_chips[i].shown == v) return;   // rebuilding a bitmap-font label is not free
        m_chips[i].shown = v;
        m_chips[i].value->setString(v.c_str());
    }

    void update(float dt) override {
        auto& an = sim::Analyzer::get();
        auto* pl = Session::get().pl();
        if (an.active() && pl) an.runSlice(pl, dt);
        if (!an.active()) {
            auto const& msg = Session::get().lastMessage;
            if (!msg.empty()) toast(msg);
            auto const& rep = Session::get().lastReport;
            if (!rep.empty()) alert("Nothing could be measured", rep);
            restoreLevel();
            Popup::onClose(nullptr);
            return;
        }
        m_t += dt;
        // motion: orbs drift, the shine slides over the bar
        m_orbA->setPosition({m_orbA0.x + std::sin(m_t * 0.35f) * 34.f, m_orbA0.y + std::cos(m_t * 0.27f) * 22.f});
        m_orbB->setPosition({m_orbB0.x + std::cos(m_t * 0.30f) * 40.f, m_orbB0.y + std::sin(m_t * 0.22f) * 26.f});
        m_shimmer->setPositionX(std::fmod(m_t * 170.f, m_barW + 140.f) - 70.f);

        auto p = an.progress();
        m_stencil->setContentSize({std::max(1.f, static_cast<float>(m_barW * std::clamp(p.fraction, 0.0, 1.0))), m_barH});

        std::string pct = fmt::format("{:.0f}%", p.fraction * 100.0);
        if (pct != m_shownPercent) { m_shownPercent = pct; m_percent->setString(pct.c_str()); }
        if (p.phase != m_shownPhase) {
            m_shownPhase = p.phase;
            m_phase->setString(p.phase.c_str());
            fitText(m_phase, m_phaseMaxW, 20.f, .6f);
        }
        setChip(0, fmt::format("{:.1f}x", p.speedX));
        setChip(1, fmt::format("{:.0f}", p.trialsPerSec));
        setChip(2, p.etaSec > 0 ? clock(p.etaSec) : "--:--");
        setChip(3, fmt::format("{} / {}", p.doneInputs, p.totalInputs));
        setChip(4, fmt::format("{}", p.trials));
        setChip(5, clock(p.elapsedSec));
        std::string foot = fmt::format("Throttle {:.0f}%{}   |   Esc to cancel", p.throttle * 100.0, p.cbfActive ? "   |   CBF" : "");
        if (foot != m_shownFoot) { m_shownFoot = foot; m_foot->setString(foot.c_str()); }
    }

    void onCancel(CCObject*) { this->onClose(nullptr); }

    void onClose(CCObject* o) override {
        auto& an = sim::Analyzer::get();
        if (an.active()) {
            an.cancel("Cancelled");
            if (auto* pl = Session::get().pl()) an.runSlice(pl, 0.016);   // finishes at once and restores the level
            else an.abandon();
        }
        restoreLevel();
        Popup::onClose(o);
    }
};

void openProgressPopup() {
    auto err = Session::get().startAnalysis();
    if (!err.empty()) { alert("Windowpane", err); return; }
    // The game must be running, not paused, while it is simulated (a paused level behaves differently on its first
    // steps). Frame Window Analyzer does the same: leave the pause menu first.
    if (auto* pl = Session::get().pl()) {
        bool resumed = false;
        if (auto* scene = CCDirector::get()->getRunningScene())
            if (auto* pause = scene->getChildByType<PauseLayer>(0)) { pause->onResume(nullptr); resumed = true; }
        if (!resumed && pl->m_isPaused) pl->resume();
    }
    if (auto* p = ProgressPopup::create()) p->show();
}

// -------------------------------------------------------------------------------------------------------------
// Control popup. 470x270: a dark info panel (macro + status), a dark result panel, and a 3x3 grid of coloured
// buttons. Every text is scaled down to fit its panel, so a long macro name or many buckets never spill out.
namespace {

// Shrinks `l` (never enlarges past maxScale) so its text fits in maxW x maxH.
void fitLabel(CCLabelBMFont* l, float maxW, float maxH, float maxScale) {
    auto cs = l->getContentSize();
    float s = maxScale;
    if (cs.width > 0.f) s = std::min(s, maxW / cs.width);
    if (cs.height > 0.f) s = std::min(s, maxH / cs.height);
    l->setScale(std::max(s, 0.05f));
}

CCScale9Sprite* makePanel(float w, float h, GLubyte opacity) {
    auto* bg = CCScale9Sprite::create("square02b_001.png");
    bg->setContentSize({w, h});
    bg->setColor({0, 0, 0});
    bg->setOpacity(opacity);
    return bg;
}

// Splits the exported summary into a short header plus a few compact lines of "label: count" entries.
std::string compactSummary(std::string const& full) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < full.size()) {
        size_t nl = full.find('\n', pos);
        if (nl == std::string::npos) nl = full.size();
        if (nl > pos) lines.push_back(full.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (lines.empty()) return "";
    std::string out = lines[0];
    constexpr size_t kPerLine = 5;
    for (size_t i = 1; i < lines.size(); ++i) {
        out += ((i - 1) % kPerLine == 0) ? "\n" : "     ";
        out += lines[i];
    }
    return out;
}

}  // namespace

// =================================================================================================================
// Windowpane 2.0 look: dark navy panels, cyan / magenta accents, flat coloured buttons with a gloss strip, cards for
// the macro, a mini histogram of the timings and the rating.
// =================================================================================================================
namespace {

// These follow the menu theme (light / dark mode and the chosen accent colours); syncTheme() refreshes them.
ccColor3B kNavy{9, 12, 26}, kPanelC{19, 26, 50}, kAccentC{70, 214, 255}, kAccent2C{255, 94, 196};
ccColor3B kTextC{232, 238, 255}, kMutedC{128, 142, 184}, kGoodC{86, 214, 128}, kWarnC{255, 190, 84}, kBadC{255, 92, 92};

void syncTheme() {
    auto const& T = theme();
    kNavy = T.bg;
    kPanelC = T.card;
    kAccentC = T.a1;
    kAccent2C = T.a2;
    kTextC = T.text;
    kMutedC = T.muted;
    kGoodC = T.good;
    kWarnC = T.warn;
    kBadC = T.bad;
}

CCScale9Sprite* roundRect(float w, float h, ccColor3B c, GLubyte op) {
    auto* s = CCScale9Sprite::create("square02b_001.png");
    s->setContentSize({w, h});
    s->setColor(c);
    s->setOpacity(op);
    return s;
}

struct Btn {
    CCMenuItemSpriteExtra* item = nullptr;
    CCLabelBMFont* label = nullptr;
    CCScale9Sprite* bg = nullptr;
    float w = 0.f, h = 0.f;
};

Btn makeButton(std::string const& text, ccColor3B color, float w, float h, CCObject* target, SEL_MenuHandler cb) {
    Btn b;
    b.w = w;
    b.h = h;
    auto* node = CCNode::create();
    node->setContentSize({w, h});
    node->setAnchorPoint({0.5f, 0.5f});
    auto* shadow = roundRect(w, h, {0, 0, 0}, 110);
    shadow->setPosition({w / 2, h / 2 - 2.f});
    node->addChild(shadow, 0);
    b.bg = roundRect(w, h, color, 255);
    b.bg->setPosition({w / 2, h / 2});
    node->addChild(b.bg, 1);
    auto* gloss = roundRect(w - 8.f, h * 0.40f, {255, 255, 255}, 44);
    gloss->setPosition({w / 2, h * 0.73f});
    node->addChild(gloss, 2);
    b.label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    b.label->setPosition({w / 2, h / 2});
    node->addChild(b.label, 3);
    fitLabel(b.label, w - 14.f, h - 9.f, .5f);
    b.item = CCMenuItemSpriteExtra::create(node, target, cb);
    return b;
}

void setButton(Btn& b, std::string const& text, ccColor3B color) {
    b.label->setString(text.c_str());
    fitLabel(b.label, b.w - 14.f, b.h - 9.f, .5f);
    b.bg->setColor(color);
}

CCLabelBMFont* makeText(std::string const& s, char const* font, float scale, ccColor3B c, CCPoint pos, CCPoint anchor = {0.f, 0.5f}) {
    auto* l = CCLabelBMFont::create(s.c_str(), font);
    l->setScale(scale);
    l->setColor(c);
    l->setAnchorPoint(anchor);
    l->setPosition(pos);
    return l;
}

// Frames a popup in the 2.0 style (call after Popup::init). Returns the size of the main layer.
CCSize styleFrame(CCNode* bgSprite, CCNode* layer, std::string const& title, ccColor3B titleColor) {
    if (bgSprite) bgSprite->setVisible(false);
    const auto size = layer->getContentSize();
    const float W = size.width, H = size.height;
    auto* border = roundRect(W + 6.f, H + 6.f, kAccentC, 255);
    border->setPosition({W / 2, H / 2});
    layer->addChild(border, -4);
    auto* glow = roundRect(W + 6.f, H + 6.f, kAccent2C, 120);
    glow->setPosition({W / 2 + 2.f, H / 2 - 2.f});
    layer->addChild(glow, -5);
    auto* body = roundRect(W, H, kNavy, 255);
    body->setPosition({W / 2, H / 2});
    layer->addChild(body, -3);
    auto* strip = roundRect(W - 8.f, 38.f, kPanelC, 255);
    strip->setPosition({W / 2, H - 23.f});
    layer->addChild(strip, -2);
    auto* line = CCLayerGradient::create(ccc4(kAccentC.r, kAccentC.g, kAccentC.b, 255), ccc4(kAccent2C.r, kAccent2C.g, kAccent2C.b, 255));
    line->setContentSize({W - 24.f, 3.f});
    line->setPosition({12.f, H - 45.f});
    line->setVector({1.f, 0.f});
    layer->addChild(line, 0);
    auto* t = CCLabelBMFont::create(title.c_str(), "goldFont.fnt");
    t->setScale(.6f);
    t->setAnchorPoint({0.f, 0.5f});
    t->setPosition({28.f, H - 23.f});
    t->setColor(titleColor);
    layer->addChild(t, 1);
    return size;
}

}  // namespace

// The pause-menu button: a round green button the same size as the game's own round buttons (the % button above it),
// with the Windowpane logo on it.
static CCTexture2D* logoTexture() {
    static Ref<CCTexture2D> tex;
    if (tex) return tex;
    auto bytes = logoBytes();
    auto* img = new CCImage();
    if (!img->initWithImageData(const_cast<unsigned char*>(bytes.data), static_cast<int>(bytes.size), CCImage::kFmtPng, 0, 0, 8)) {
        img->release();
        return nullptr;
    }
    auto* t = new CCTexture2D();
    if (!t->initWithImage(img)) {
        img->release();
        t->release();
        return nullptr;
    }
    img->release();
    t->autorelease();
    tex = t;
    return tex;
}

CCNode* makePauseButtonNode() {
    // match the size of the % button the new one sits under
    float diameter = 30.f;
    if (auto* ref = CCSprite::createWithSpriteFrameName("GJ_percentBtn_001.png")) diameter = ref->getContentSize().width;
    const float R = diameter / 2.f;
    auto* node = CCNode::create();
    node->setContentSize({diameter, diameter});
    node->setAnchorPoint({0.5f, 0.5f});
    // Real circles (a many-sided polygon): drawDot rendered squares in this game's cocos2d.
    auto* dn = CCDrawNode::create();
    auto disc = [&](CCPoint c, float r, ccColor4F col) {
        constexpr int kSides = 72;
        CCPoint v[kSides];
        for (int i = 0; i < kSides; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / static_cast<float>(kSides);
            v[i] = CCPoint(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
        }
        dn->drawPolygon(v, kSides, col, 0.f, ccc4f(0.f, 0.f, 0.f, 0.f));
    };
    disc({R, R - R * 0.06f}, R, ccc4f(0.f, 0.f, 0.f, 0.30f));                    // soft shadow
    disc({R, R}, R, ccc4f(1.f, 1.f, 1.f, 1.f));                                    // white rim
    disc({R, R}, R * 0.925f, ccc4f(0.f, 0.f, 0.f, 1.f));                           // thin black stroke
    disc({R, R}, R * 0.86f, ccc4f(0.30f, 0.70f, 0.08f, 1.f));                      // dark green edge
    disc({R, R + R * 0.045f}, R * 0.80f, ccc4f(0.49f, 0.86f, 0.15f, 1.f));         // face
    disc({R, R + R * 0.14f}, R * 0.60f, ccc4f(0.60f, 0.94f, 0.24f, 1.f));          // highlight
    node->addChild(dn, 0);
    if (auto* tex = logoTexture()) {
        auto* logo = CCSprite::createWithTexture(tex);
        const float target = diameter * 0.66f;
        logo->setScale(target / std::max(1.f, logo->getContentSize().width));
        logo->setPosition({R, R});
        node->addChild(logo, 1);
    }
    return node;
}

// -----------------------------------------------------------------------------------------------------------------
// Edit windows: page through the measured clicks and change their frame window by hand.
// -----------------------------------------------------------------------------------------------------------------
class EditWindowsPopup : public Popup {
public:
    static EditWindowsPopup* create() {
        auto* p = new EditWindowsPopup();
        if (p->init()) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }

protected:
    std::vector<size_t> m_list;
    size_t m_pos = 0;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_value = nullptr;
    CCLabelBMFont* m_sub = nullptr;
    CCLabelBMFont* m_hint = nullptr;

    bool init() {
        if (!Popup::init(380.f, 250.f)) return false;
        syncTheme();
        m_list = Session::get().editableInputs();
        if (m_list.empty()) return false;
        const auto size = styleFrame(m_bgSprite, m_mainLayer, "EDIT FRAME WINDOWS", kTextC);
        const float W = size.width, H = size.height, cx = W / 2;

        m_info = makeText("", "chatFont.fnt", .6f, kMutedC, {cx, H - 66.f}, {0.5f, 0.5f});
        m_mainLayer->addChild(m_info, 2);
        auto* card = roundRect(W - 40.f, 78.f, kPanelC, 255);
        card->setPosition({cx, H - 124.f});
        m_mainLayer->addChild(card, 0);
        m_value = makeText("", "bigFont.fnt", 1.1f, kTextC, {cx, H - 118.f}, {0.5f, 0.5f});
        m_mainLayer->addChild(m_value, 2);
        m_sub = makeText("", "chatFont.fnt", .55f, kMutedC, {cx, H - 146.f}, {0.5f, 0.5f});
        m_mainLayer->addChild(m_sub, 2);

        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu, 5);
        auto add = [&](std::string const& text, ccColor3B col, float x, float y, float w, SEL_MenuHandler cb) {
            auto b = makeButton(text, col, w, 26.f, this, cb);
            b.item->setPosition({x, y});
            menu->addChild(b.item);
            return b;
        };
        add("<", kPanelC, 34.f, H - 124.f, 30.f, menu_selector(EditWindowsPopup::onPrev));
        add(">", kPanelC, W - 34.f, H - 124.f, 30.f, menu_selector(EditWindowsPopup::onNext));
        add("-1", {196, 70, 90}, cx - 120.f, 78.f, 54.f, menu_selector(EditWindowsPopup::onM1));
        add("-0.1", {160, 80, 120}, cx - 60.f, 78.f, 56.f, menu_selector(EditWindowsPopup::onM01));
        add("+0.1", {60, 140, 170}, cx + 60.f, 78.f, 56.f, menu_selector(EditWindowsPopup::onP01));
        add("+1", {50, 170, 110}, cx + 120.f, 78.f, 54.f, menu_selector(EditWindowsPopup::onP1));
        add("Reset", {120, 110, 60}, cx - 70.f, 40.f, 120.f, menu_selector(EditWindowsPopup::onReset));
        add("Done", {40, 120, 200}, cx + 70.f, 40.f, 120.f, menu_selector(EditWindowsPopup::onDone));
        m_hint = makeText("Edits update the sidebar, circles and sounds at once. Save macro keeps them.", "chatFont.fnt", .42f, kMutedC, {cx, 18.f}, {0.5f, 0.5f});
        m_mainLayer->addChild(m_hint, 2);
        this->refresh();
        return true;
    }

    void refresh() {
        auto& S = Session::get();
        S.refresh();
        const size_t idx = m_list[m_pos];
        auto res = S.result();
        auto m = S.macro();
        if (!res || !m || idx >= res->inputs.size() || idx >= m->inputs.size()) return;
        auto const& r = res->inputs[idx];
        auto const& e = m->inputs[idx];
        m_info->setString(fmt::format("Click {} of {}  -  {}  at tick {}  ({:.1f}%)", m_pos + 1, m_list.size(), e.down ? "press" : "release", e.tick, r.percent).c_str());
        fitLabel(m_info, 330.f, 14.f, .6f);
        m_value->setString(fmt::format("{:g}", r.window).c_str());
        ccColor3B col = kTextC;
        const int b = S.bucketOf(idx);
        if (b >= 0 && static_cast<size_t>(b) < cfg().buckets.size()) {
            auto const& lc = cfg().buckets[b].labelColor;
            col = ccColor3B{static_cast<GLubyte>(lc.r), static_cast<GLubyte>(lc.g), static_cast<GLubyte>(lc.b)};
        }
        m_value->setColor(col);
        m_sub->setString(r.edited ? fmt::format("frames  -  edited (analysis said {:g})", r.analyzedWindow).c_str() : "frames  -  as measured");
        m_sub->setColor(r.edited ? kWarnC : kMutedC);
    }
    void adjust(double d) {
        auto& S = Session::get();
        auto res = S.result();
        if (!res) return;
        const size_t idx = m_list[m_pos];
        S.editWindow(idx, res->inputs[idx].window + d);
        this->refresh();
    }
    void onPrev(CCObject*) { if (m_pos > 0) --m_pos; else m_pos = m_list.size() - 1; this->refresh(); }
    void onNext(CCObject*) { m_pos = (m_pos + 1) % m_list.size(); this->refresh(); }
    void onM1(CCObject*) { adjust(-1.0); }
    void onM01(CCObject*) { adjust(-0.1); }
    void onP01(CCObject*) { adjust(0.1); }
    void onP1(CCObject*) { adjust(1.0); }
    void onReset(CCObject*) { Session::get().resetWindow(m_list[m_pos]); this->refresh(); }
    void onDone(CCObject*) { this->onClose(nullptr); }
};

void openEditWindowsPopup() {
    if (Session::get().editableInputs().empty()) {
        alert("Nothing to edit yet", "Analyze a macro first (or load a saved macro that has its analysis attached), then edit the frame windows of its clicks.");
        return;
    }
    if (auto* p = EditWindowsPopup::create()) p->show();
}

// -----------------------------------------------------------------------------------------------------------------
// Import screen: which bot recorded the macro? The answer decides which readers may open the file and how its timing
// is interpreted (see MacroParser.cpp / Analyzer.cpp).
// -----------------------------------------------------------------------------------------------------------------
class MacroTypePopup : public Popup {
public:
    static MacroTypePopup* create(std::function<void()> done) {
        auto* p = new MacroTypePopup();
        if (p->init(std::move(done))) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }

public:
    // The popup's own open animation sets the scale back to full size, which is why the scale above did not stick. Apply it again
    // once the animation has run.
    void show() override {
        Popup::show();
        this->stopAllActions();
        m_mainLayer->stopAllActions();
        this->setScale(1.f);
        m_mainLayer->setScale(m_finalScale);
    }

protected:
    std::function<void()> m_done;
    float m_finalScale = 1.f;

    bool init(std::function<void()> done) {
        if (!Popup::init(420.f, 430.f)) return false;
        m_done = std::move(done);
        syncTheme();
        const auto size = styleFrame(m_bgSprite, m_mainLayer, "IMPORT MACRO", kTextC);
        const float W = size.width, H = size.height, cx = W / 2;
        m_mainLayer->addChild(makeText("Which bot recorded this macro?", "chatFont.fnt", .62f, kMutedC, {cx, H - 62.f}, {0.5f, 0.5f}), 2);

        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu, 5);

        struct Row { MacroType t; ccColor3B col; };
        const Row rows[] = {
            {MacroType::XdBot, {60, 140, 200}},
            {MacroType::ZBot, {70, 160, 110}},
            {MacroType::Eclipse, {150, 90, 200}},
            {MacroType::Windowpane, {40, 120, 210}},
            {MacroType::MegaHack, {200, 110, 60}},
            {MacroType::Auto, {95, 95, 110}},
        };
        float y = H - 100.f;
        for (auto const& r : rows) {
            auto b = makeButton(macroTypeName(r.t), r.col, r.t == MacroType::Auto ? 170.f : 240.f, 28.f, this, menu_selector(MacroTypePopup::onPick));
            b.item->setTag(static_cast<int>(r.t));
            b.item->setPosition({cx, y});
            menu->addChild(b.item);
            auto* sub = makeText(macroTypeSummary(r.t), "chatFont.fnt", .4f, kMutedC, {cx, y - 24.f}, {0.5f, 0.5f});
            fitLabel(sub, W - 30.f, 12.f, .4f);
            m_mainLayer->addChild(sub, 2);
            y -= 52.f;
        }
        auto cancel = makeButton("Cancel", {120, 60, 70}, 120.f, 24.f, this, menu_selector(MacroTypePopup::onCancel));
        cancel.item->setPosition({cx, 26.f});
        menu->addChild(cancel.item);
        // The frame is 430 tall, taller than the screen: scale the whole popup down about its centre so it fits with a margin.
        // beta.22: 15% smaller than before, and fitted to the real frame size so it no longer runs off the screen.
        {
            auto const win = CCDirector::get()->getWinSize();
            float fit = std::min(win.height * 0.84f / H, win.width * 0.9f / W);
            m_finalScale = std::clamp(fit, 0.4f, 1.f) * 0.85f;
            m_mainLayer->setScale(m_finalScale);
        }
        return true;
    }

    void onPick(CCObject* s) {
        const auto type = static_cast<MacroType>(static_cast<CCNode*>(s)->getTag());
        auto done = m_done;
        this->onClose(nullptr);
        pickMacroOfType(type, std::move(done));
    }
    void onCancel(CCObject*) { this->onClose(nullptr); }
};

void pickMacro(std::function<void()> done) {
    if (auto* p = MacroTypePopup::create(std::move(done))) p->show();
}

// The main menu lives in Menu.cpp.

}  // namespace wp::ui
