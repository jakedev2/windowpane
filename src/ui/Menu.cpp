// The Windowpane menu: a sidebar with six pages (Home, Macro, Record, Analyze, Theme, Options), cards, and buttons in
// several sizes grouped under small category labels. Light / dark mode and the two accent colours come from Theme.hpp.
//
// Every change (page, theme, a toggle) rebuilds the whole page one frame later: the menu is small, and rebuilding
// from the current state means a label can never show stale data.

#include "Ui.hpp"
#include "Theme.hpp"
#include "PickFile.hpp"

#include "../core/Cfg.hpp"
#include "../core/Session.hpp"
#include "../import/MacroParser.hpp"
#include "../sim/Analyzer.hpp"

#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/TextInput.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
#include <fmt/format.h>

using namespace geode::prelude;

namespace wp::ui {

namespace {

constexpr float kW = 520.f, kH = 300.f;
constexpr float kCX = 120.f;      // left edge of the page area
constexpr float kCW = 390.f;      // width of the page area
constexpr float kTop = 252.f;     // top of the page area (the header sits above it)
constexpr int kPages = 6;

void toast(std::string const& text, NotificationIcon icon = NotificationIcon::Success) {
    Notification::create(text, icon, 2.5f)->show();
}
void alert(std::string const& title, std::string const& body) {
    FLAlertLayer::create(title.c_str(), body, "OK")->show();
}

enum IconKind { IHome, IMacro, IRec, IBars, IGrid, ISliders, ISun, IHalf };

// Small flat icons drawn from polygons, so the menu needs no image files.
CCNode* makeIcon(IconKind kind, float s, ccColor3B c) {
    auto* n = CCNode::create();
    n->setContentSize({s, s});
    n->setAnchorPoint({0.5f, 0.5f});
    auto* d = CCDrawNode::create();
    const ccColor4F f = ccc4f(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
    auto poly = [&](std::vector<CCPoint> v) {
        {   // drop repeated points (zero-length edges make drawPolygon draw spikes)
            std::vector<CCPoint> u;
            for (auto const& p : v) {
                if (u.empty() || std::fabs(u.back().x - p.x) > 0.01f || std::fabs(u.back().y - p.y) > 0.01f) u.push_back(p);
            }
            while (u.size() > 1 && std::fabs(u.back().x - u.front().x) <= 0.01f && std::fabs(u.back().y - u.front().y) <= 0.01f) u.pop_back();
            v.swap(u);
        }
        if (v.size() < 3) return;
        d->drawPolygon(v.data(), static_cast<unsigned int>(v.size()), f, 0.f, ccc4f(0.f, 0.f, 0.f, 0.f));
    };
    auto rect = [&](float x, float y, float w, float h) {
        poly({CCPoint(x, y), CCPoint(x + w, y), CCPoint(x + w, y + h), CCPoint(x, y + h)});
    };
    auto disc = [&](float cx, float cy, float r, float a0 = 0.f, float a1 = 6.2831853f) {
        std::vector<CCPoint> v;
        if (a1 - a0 < 6.f) v.push_back(CCPoint(cx, cy));
        const bool full = a1 - a0 >= 6.f;   // a full circle: the last point would equal the first
        for (int i = 0; i <= (full ? 27 : 28); ++i) {
            const float a = a0 + (a1 - a0) * static_cast<float>(i) / 28.f;
            v.push_back(CCPoint(cx + std::cos(a) * r, cy + std::sin(a) * r));
        }
        poly(v);
    };
    switch (kind) {
        case IHome:
            poly({CCPoint(0.f, s * .52f), CCPoint(s * .5f, s), CCPoint(s, s * .52f)});
            rect(s * .14f, 0.f, s * .72f, s * .56f);
            break;
        case IMacro:
            rect(0.f, s * .72f, s, s * .2f);
            rect(0.f, s * .40f, s * .78f, s * .2f);
            rect(0.f, s * .08f, s, s * .2f);
            break;
        case IRec: disc(s / 2, s / 2, s * .45f); break;
        case IBars:
            rect(s * .04f, 0.f, s * .22f, s * .40f);
            rect(s * .39f, 0.f, s * .22f, s * .68f);
            rect(s * .74f, 0.f, s * .22f, s);
            break;
        case IGrid:
            rect(0.f, 0.f, s * .45f, s * .45f);
            rect(s * .55f, 0.f, s * .45f, s * .45f);
            rect(0.f, s * .55f, s * .45f, s * .45f);
            rect(s * .55f, s * .55f, s * .45f, s * .45f);
            break;
        case ISliders:
            rect(0.f, s * .72f, s, s * .12f);
            rect(0.f, s * .44f, s, s * .12f);
            rect(0.f, s * .16f, s, s * .12f);
            disc(s * .30f, s * .78f, s * .16f);
            disc(s * .70f, s * .50f, s * .16f);
            disc(s * .42f, s * .22f, s * .16f);
            break;
        case ISun:
            disc(s / 2, s / 2, s * .24f);
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * 0.78539816f;
                disc(s / 2 + std::cos(a) * s * .40f, s / 2 + std::sin(a) * s * .40f, s * .07f);
            }
            break;
        case IHalf: disc(s / 2, s / 2, s * .42f, 1.5707963f, 4.712389f); break;   // left half of a disc
    }
    n->addChild(d);
    return n;
}

enum class Style { Accent, Solid, Good, Danger };

class ControlPopup : public Popup {
public:
    static ControlPopup* create() {
        auto* p = new ControlPopup();
        if (p->init()) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }

    // Called when a macro finished loading (the file dialog is asynchronous).
    void requestRebuild() {
        if (m_pending) return;
        m_pending = true;
        Loader::get()->queueInMainThread([self = Ref<ControlPopup>(this)] {
            self->m_pending = false;
            self->rebuild();
        });
    }

protected:
    CCNode* m_root = nullptr;
    CCMenu* m_menu = nullptr;
    bool m_pending = false;
    inline static int s_page = 0;   // remembered while the game is open

    bool init() {
        if (!Popup::init(kW, kH)) return false;
        if (m_bgSprite) m_bgSprite->setVisible(false);
        if (m_closeBtn) m_closeBtn->setVisible(false);
        build();
        return true;
    }

    void rebuild() {
        if (m_root) m_root->removeFromParent();
        m_root = nullptr;
        m_menu = nullptr;
        build();
    }

    // ---- small building blocks ----------------------------------------------------------------------------------
    void add(CCNode* n, float x, float y, int z = 2) {
        n->setPosition({x, y});
        m_root->addChild(n, z);
    }
    void addTL(CCNode* n, float left, float top, int z = 1) {
        auto s = n->getContentSize();
        add(n, left + s.width / 2, top - s.height / 2, z);
    }
    void addBtn(CCMenuItem* it, float x, float y) {
        it->setPosition({x, y});
        m_menu->addChild(it);
    }
    CCLabelBMFont* txt(std::string const& s, char const* font, float scale, ccColor3B c, float x, float y,
                       CCPoint anchor = CCPoint(0.f, .5f), float maxW = 0.f, int z = 6) {
        auto* l = CCLabelBMFont::create(s.c_str(), font);
        l->setColor(c);
        l->setAnchorPoint(anchor);
        l->setScale(scale);
        if (maxW > 0.f) fitText(l, maxW, 1000.f, scale);
        l->setPosition({x, y});
        m_root->addChild(l, z);
        return l;
    }
    // small grey category title above a group of buttons
    void category(std::string const& s, float x, float y) {
        txt(s, "bigFont.fnt", .27f, theme().muted, x, y);
    }
    CCNode* mkCard(float w, float h, float r = 14.f) {
        auto const& T = theme();
        auto* n = CCNode::create();
        n->setContentSize({w, h});
        n->setAnchorPoint({0.5f, 0.5f});
        auto* b = shapeRound(w + 2.f, h + 2.f, r + 1.f, T.line);
        b->setPosition({w / 2, h / 2});
        n->addChild(b, 0);
        auto* g = shapeGrad(w, h, r, T.cardHi, T.card, false);
        g->setPosition({w / 2, h / 2});
        n->addChild(g, 1);
        return n;
    }
    CCMenuItemSpriteExtra* mkBtn(std::string const& text, float w, float h, Style st, SEL_MenuHandler cb, int tag = 0,
                                 float fontScale = .5f, float press = 1.08f) {
        auto const& T = theme();
        const float r = h / 2.f;
        auto* node = CCNode::create();
        node->setContentSize({w, h});
        node->setAnchorPoint({0.5f, 0.5f});
        auto* sh = shapeRound(w, h, r, {0, 0, 0}, T.dark ? 90 : 38);
        sh->setPosition({w / 2, h / 2 - 2.f});
        node->addChild(sh, 0);
        ccColor3B tc{255, 255, 255};
        CCNode* body = nullptr;
        switch (st) {
            case Style::Accent: body = shapeGrad(w, h, r, T.a1, T.a2, true); break;
            case Style::Good: body = shapeGrad(w, h, r, {52, 208, 120}, {36, 170, 150}, true); break;
            case Style::Danger: body = shapeGrad(w, h, r, {255, 92, 110}, {214, 52, 120}, true); break;
            default: body = shapeRound(w, h, r, T.pill); tc = T.text; break;
        }
        body->setPosition({w / 2, h / 2});
        node->addChild(body, 1);
        if (st != Style::Solid) {   // soft highlight on the upper half of the coloured buttons
            auto* gl = shapeRound(w - h * .7f, h * .34f, h * .17f, {255, 255, 255}, 38);
            gl->setPosition({w / 2, h * .74f});
            node->addChild(gl, 2);
        }
        auto* l = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        l->setColor(tc);
        l->setPosition({w / 2, h / 2});
        node->addChild(l, 3);
        fitText(l, w - 18.f, h - 10.f, fontScale);
        auto* it = CCMenuItemSpriteExtra::create(node, this, cb);
        it->setTag(tag);
        it->m_scaleMultiplier = press;
        return it;
    }

    // ---- frame, header, sidebar -------------------------------------------------------------------------------------
    void build() {
        auto const& T = theme();
        m_root = CCNode::create();
        m_mainLayer->addChild(m_root, 1);
        add(shapeGrad(kW + 4.f, kH + 4.f, 26.f, T.a1, T.a2, true), kW / 2, kH / 2, -3);   // accent border
        add(shapeRound(kW, kH, 24.f, T.bg), kW / 2, kH / 2, -2);
        m_menu = CCMenu::create();
        m_menu->setPosition({0.f, 0.f});
        m_root->addChild(m_menu, 10);

        buildHeader();
        buildSidebar();
        switch (s_page) {
            case 0: pageHome(); break;
            case 1: pageMacro(); break;
            case 2: pageRecord(); break;
            case 3: pageAnalyze(); break;
            case 4: pageTheme(); break;
            default: pageOptions(); break;
        }
    }

    void buildHeader() {
        auto const& T = theme();
        auto& S = Session::get();
        const float y = kH - 24.f;
        add(shapeGrad(24.f, 24.f, 12.f, T.a1, T.a2, false), 26.f, y, 2);
        add(makeIcon(IBars, 12.f, {255, 255, 255}), 26.f, y, 3);
        auto* title = txt("Windowpane", "bigFont.fnt", .46f, T.text, 44.f, y, CCPoint(0.f, .5f), 120.f);
        txt(Mod::get()->getVersion().toVString(), "bigFont.fnt", .3f, T.a2, 44.f + title->getContentSize().width * title->getScale() + 6.f, y - 1.f);

        addBtn(mkBtn("X", 24.f, 24.f, Style::Solid, menu_selector(ControlPopup::onCloseBtn), 0, .4f, 1.15f), kW - 22.f, y);

        auto* mode = CCNode::create();
        mode->setContentSize({24.f, 24.f});
        mode->setAnchorPoint({0.5f, 0.5f});
        auto* mbg = shapeGrad(24.f, 24.f, 12.f, T.a1, T.a2, false);
        mbg->setPosition({12.f, 12.f});
        mode->addChild(mbg, 0);
        auto* mic = makeIcon(T.dark ? ISun : IHalf, 13.f, {255, 255, 255});
        mic->setPosition({12.f, 12.f});
        mode->addChild(mic, 1);
        auto* mit = CCMenuItemSpriteExtra::create(mode, this, menu_selector(ControlPopup::onToggleMode));
        mit->m_scaleMultiplier = 1.15f;
        addBtn(mit, kW - 50.f, y);

        std::string pill;
        ccColor3B pc = T.muted;
        if (S.recording()) { pill = fmt::format("REC {:g}x", S.recordSpeed()); pc = T.bad; }
        else if (S.playbackOn()) { pill = "PLAYBACK ON"; pc = T.good; }
        else pill = "PLAYBACK OFF";
        const float px = kW - 128.f;
        add(shapeRound(116.f, 20.f, 10.f, T.pill), px, y, 2);
        add(shapeRound(7.f, 7.f, 3.5f, pc), px - 46.f, y, 3);
        txt(pill, "bigFont.fnt", .3f, pc, px - 36.f, y, CCPoint(0.f, .5f), 84.f);
    }

    CCMenuItemSpriteExtra* mkNav(int page, IconKind icon, char const* name) {
        auto const& T = theme();
        const bool sel = page == s_page;
        const float w = 88.f, h = 26.f;
        auto* node = CCNode::create();
        node->setContentSize({w, h});
        node->setAnchorPoint({0.5f, 0.5f});
        if (sel) {
            auto* b = shapeGrad(w, h, 13.f, T.a1, T.a2, true);
            b->setPosition({w / 2, h / 2});
            node->addChild(b, 0);
        }
        const ccColor3B ic = sel ? ccColor3B{255, 255, 255} : T.muted;
        auto* i = makeIcon(icon, 13.f, ic);
        i->setPosition({16.f, h / 2});
        node->addChild(i, 1);
        auto* l = CCLabelBMFont::create(name, "bigFont.fnt");
        l->setColor(ic);
        l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({29.f, h / 2});
        node->addChild(l, 1);
        fitText(l, 54.f, h - 8.f, .36f);
        auto* it = CCMenuItemSpriteExtra::create(node, this, menu_selector(ControlPopup::onPage));
        it->setTag(page);
        it->m_scaleMultiplier = 1.05f;
        return it;
    }

    void buildSidebar() {
        static char const* names[kPages] = {"Home", "Macro", "Record", "Analyze", "Theme", "Options"};
        static const IconKind icons[kPages] = {IHome, IMacro, IRec, IBars, IGrid, ISliders};
        addTL(mkCard(100.f, 242.f, 18.f), 10.f, kTop, 0);
        for (int i = 0; i < kPages; ++i) addBtn(mkNav(i, icons[i], names[i]), 60.f, kTop - 20.f - static_cast<float>(i) * 32.f);
        addBtn(mkBtn("All settings", 84.f, 22.f, Style::Solid, menu_selector(ControlPopup::onAllSettings), 0, .34f, 1.08f), 60.f, 28.f);
    }

    // ---- shared pieces of the pages ---------------------------------------------------------------------------------
    // Bars for the timing buckets. (x, yBase) = left end and bottom of the bars; labels are drawn just below yBase.
    void drawHist(float x, float yBase, float areaW, float barH, bool detailed) {
        auto const& T = theme();
        auto& S = Session::get();
        auto const& buckets = cfg().buckets;
        auto const& totals = S.totals();
        if (!S.result() || buckets.empty() || totals.size() != buckets.size()) {
            txt("Run Analyze to see your timings", "chatFont.fnt", .5f, T.muted, x + areaW / 2, yBase + barH / 2, CCPoint(.5f, .5f), areaW);
            return;
        }
        int mx = 1;
        for (int c : totals) mx = std::max(mx, c);
        const size_t n = buckets.size();
        const float step = areaW / static_cast<float>(n);
        const float bw = std::min(detailed ? 28.f : 12.f, step - 3.f);
        for (size_t k = 0; k < n; ++k) {
            const size_t bi = n - 1 - k;   // lowest window on the left
            auto const& b = buckets[bi];
            const float cx = x + step * static_cast<float>(k) + step / 2;
            const float h = totals[bi] == 0 ? 3.f : std::max(5.f, barH * static_cast<float>(totals[bi]) / static_cast<float>(mx));
            add(shapeRound(bw, h, std::min(bw / 2, 4.f), ccColor3B{b.labelColor.r, b.labelColor.g, b.labelColor.b}, totals[bi] == 0 ? 80 : 255), cx, yBase + h / 2, 3);
            txt(b.label, "chatFont.fnt", .36f, T.muted, cx, yBase - 8.f, CCPoint(.5f, .5f), step - 1.f);
            if (detailed && totals[bi] > 0) txt(std::to_string(totals[bi]), "chatFont.fnt", .42f, T.text, cx, yBase + h + 7.f, CCPoint(.5f, .5f));
        }
    }

    std::string statusLine() {
        auto& S = Session::get();
        if (S.recording()) return cfg().recordStitch ? "Recording - checkpoints keep the earlier part" : "Recording - a death restarts the level and the recording";
        if (!S.lastMessage.empty()) return S.lastMessage;
        return S.macro() ? "Ready. Press Analyze." : "Load a macro or press Record in practice mode.";
    }

    // ---- pages --------------------------------------------------------------------------------------------------------
    void pageHome() {
        auto const& T = theme();
        auto& S = Session::get();
        S.refresh();
        auto m = S.macro();
        auto res = S.result();
        size_t measured = 0;
        if (res) for (auto const& r : res->inputs) if (r.status == InputStatus::Measured || r.status == InputStatus::Capped) ++measured;

        // macro card
        addTL(mkCard(190.f, 118.f), kCX, kTop);
        const float ax = kCX + 34.f, ay = kTop - 34.f;
        add(shapeGrad(40.f, 40.f, 20.f, T.a1, T.a2, false), ax, ay, 3);
        std::string initial = m && !m->name.empty() ? std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(m->name[0])))) : "?";
        txt(initial, "bigFont.fnt", .55f, ccColor3B{255, 255, 255}, ax, ay, CCPoint(.5f, .5f));
        add(shapeRound(14.f, 14.f, 7.f, T.card), ax + 15.f, ay + 15.f, 4);
        add(shapeRound(10.f, 10.f, 5.f, m ? T.good : T.muted), ax + 15.f, ay + 15.f, 5);
        txt(m ? m->name : "No macro loaded", "bigFont.fnt", .42f, T.a1, kCX + 62.f, kTop - 26.f, CCPoint(0.f, .5f), 120.f);
        txt(m ? fmt::format("{}  -  {} inputs  -  {} FPS", m->format, m->inputs.size(), static_cast<int>(std::lround(m->sourceTps))) : "load one, or record in practice", "chatFont.fnt", .45f, T.muted,
            kCX + 62.f, kTop - 44.f, CCPoint(0.f, .5f), 120.f);
        txt(m ? std::to_string(m->inputs.size()) : "-", "bigFont.fnt", .6f, T.text, kCX + 48.f, kTop - 82.f, CCPoint(.5f, .5f), 80.f);
        txt("Inputs", "chatFont.fnt", .42f, T.muted, kCX + 48.f, kTop - 104.f, CCPoint(.5f, .5f));
        txt(res ? std::to_string(measured) : "-", "bigFont.fnt", .6f, T.text, kCX + 142.f, kTop - 82.f, CCPoint(.5f, .5f), 80.f);
        txt("Measured", "chatFont.fnt", .42f, T.muted, kCX + 142.f, kTop - 104.f, CCPoint(.5f, .5f));

        // rating card
        const float bx = kCX + 198.f;
        addTL(mkCard(192.f, 118.f), bx, kTop);
        category("RATING", bx + 12.f, kTop - 14.f);
        const double sg = S.sigmaPerSec();
        txt(res && sg > 0.0 ? fmt::format("{:.1f}", sg) : "-", "bigFont.fnt", .85f, res && sg > 0.0 ? T.a1 : T.muted, bx + 50.f, kTop - 56.f, CCPoint(.5f, .5f), 84.f);
        txt("sigma / s", "chatFont.fnt", .45f, T.muted, bx + 50.f, kTop - 82.f, CCPoint(.5f, .5f));
        drawHist(bx + 100.f, kTop - 96.f, 84.f, 54.f, false);

        // status strip
        const float sy = kTop - 126.f - 17.f;
        addTL(mkCard(kCW, 34.f, 17.f), kCX, kTop - 126.f);
        add(shapeRound(8.f, 8.f, 4.f, S.recording() ? T.bad : (S.playbackOn() ? T.good : T.muted)), kCX + 16.f, sy, 3);
        txt(statusLine(), "chatFont.fnt", .5f, T.text, kCX + 30.f, sy, CCPoint(0.f, .5f), 346.f);

        // buttons: three sizes
        const float by = sy - 17.f - 8.f - 19.f;
        addBtn(mkBtn("Analyze", 172.f, 38.f, Style::Accent, menu_selector(ControlPopup::onAnalyze), 0, .6f), kCX + 86.f, by);
        addBtn(mkBtn("Load macro", 98.f, 38.f, Style::Solid, menu_selector(ControlPopup::onLoad), 0, .42f), kCX + 231.f, by);
        addBtn(mkBtn(S.recording() ? "Stop rec" : "Record", 98.f, 38.f, Style::Danger, menu_selector(ControlPopup::onRecord), 0, .42f), kCX + 339.f, by);
        const float qy = by - 19.f - 8.f - 14.f;
        // six small buttons: Play, Speed, FPS (physics tick rate), Clear, Edit, Save
        addBtn(mkBtn(S.playbackOn() ? "Play ON" : "Play OFF", 58.f, 28.f, S.playbackOn() ? Style::Good : Style::Solid, menu_selector(ControlPopup::onPlayback), 0, .32f), kCX + 29.f, qy);
        addBtn(mkBtn(fmt::format("Speed {:g}x", S.recordSpeed()), 58.f, 28.f, Style::Solid, menu_selector(ControlPopup::onSpeed), 0, .3f), kCX + 94.f, qy);
        addBtn(mkBtn(fmt::format("FPS: {}", S.fps()), 58.f, 28.f, S.macroFpsMismatch() ? Style::Danger : Style::Accent, menu_selector(ControlPopup::onFps), 0, .32f), kCX + 159.f, qy);
        addBtn(mkBtn("Clear", 58.f, 28.f, Style::Solid, menu_selector(ControlPopup::onClear), 0, .32f), kCX + 224.f, qy);
        addBtn(mkBtn("Edit", 58.f, 28.f, Style::Solid, menu_selector(ControlPopup::onEdit), 0, .32f), kCX + 289.f, qy);
        addBtn(mkBtn("Save", 58.f, 28.f, Style::Solid, menu_selector(ControlPopup::onSave), 0, .32f), kCX + 354.f, qy);
    }

    void pageMacro() {
        auto const& T = theme();
        auto& S = Session::get();
        auto m = S.macro();
        addTL(mkCard(kCW, 70.f), kCX, kTop);
        category("CURRENT MACRO", kCX + 14.f, kTop - 13.f);
        txt(m ? m->name : "No macro loaded", "bigFont.fnt", .5f, T.a1, kCX + 14.f, kTop - 33.f, CCPoint(0.f, .5f), kCW - 28.f);
        std::string sub = "Load one from a file, or record in practice mode.";
        if (m) {
            sub = fmt::format("{}  -  {} inputs  -  {} FPS  -  {}", m->format, m->inputs.size(), static_cast<int>(std::lround(m->sourceTps)), m->platformer ? "platformer" : "classic");
            if (!m->levelName.empty()) sub += fmt::format("  -  {}", m->levelName);
        }
        txt(sub, "chatFont.fnt", .5f, T.muted, kCX + 14.f, kTop - 54.f, CCPoint(0.f, .5f), kCW - 28.f);

        float y = kTop - 70.f - 14.f;
        category("FILE", kCX + 4.f, y);
        y -= 8.f + 17.f;
        addBtn(mkBtn("Load macro", 190.f, 34.f, Style::Accent, menu_selector(ControlPopup::onLoad), 0, .5f), kCX + 95.f, y);
        addBtn(mkBtn("Save macro", 190.f, 34.f, Style::Solid, menu_selector(ControlPopup::onSave), 0, .5f), kCX + 295.f, y);
        y -= 17.f + 18.f;
        category("MANAGE", kCX + 4.f, y);
        y -= 8.f + 15.f;
        addBtn(mkBtn("Edit windows", 190.f, 30.f, Style::Solid, menu_selector(ControlPopup::onEdit), 0, .45f), kCX + 95.f, y);
        addBtn(mkBtn("Clear macro", 190.f, 30.f, Style::Solid, menu_selector(ControlPopup::onClear), 0, .45f), kCX + 295.f, y);
        y -= 15.f + 18.f;
        category("EXPORT", kCX + 4.f, y);
        y -= 8.f + 15.f;
        addBtn(mkBtn("Save JSON", 190.f, 30.f, Style::Solid, menu_selector(ControlPopup::onSaveJson), 0, .45f), kCX + 95.f, y);
        addBtn(mkBtn("Copy summary", 190.f, 30.f, Style::Solid, menu_selector(ControlPopup::onCopy), 0, .45f), kCX + 295.f, y);
    }

    void pageRecord() {
        auto const& T = theme();
        auto& S = Session::get();
        addTL(mkCard(kCW, 58.f), kCX, kTop);
        category("RECORDER", kCX + 14.f, kTop - 13.f);
        txt(S.recording() ? (cfg().recordStitch ? "Recording - checkpoints keep the earlier part." : "Recording from 0% - a death restarts the level and the recording.")
                          : "Start recording turns on practice mode for you.",
            "chatFont.fnt", .5f, S.recording() ? T.bad : T.text, kCX + 14.f, kTop - 38.f, CCPoint(0.f, .5f), kCW - 28.f);

        float y = kTop - 58.f - 8.f - 20.f;
        addBtn(mkBtn(S.recording() ? "Stop recording" : "Start recording", kCW, 40.f, S.recording() ? Style::Danger : Style::Accent,
                     menu_selector(ControlPopup::onRecord), 0, .55f, 1.04f), kCX + kCW / 2, y);

        y -= 20.f + 20.f;
        category("RECORDING SPEED", kCX + 4.f, y);
        y -= 8.f + 14.f;
        static constexpr double steps[4] = {1.0, 0.5, 0.25, 0.1};
        for (int i = 0; i < 4; ++i) {
            const bool sel = std::abs(S.recordSpeed() - steps[i]) < 1e-6;
            addBtn(mkBtn(fmt::format("{:g}x", steps[i]), 88.f, 28.f, sel ? Style::Accent : Style::Solid, menu_selector(ControlPopup::onSpeedSet), i, .42f),
                   kCX + 44.f + static_cast<float>(i) * 100.67f, y);
        }
        y -= 14.f + 20.f;
        category("PLAYBACK", kCX + 4.f, y);
        y -= 8.f + 16.f;
        addBtn(mkBtn(S.playbackOn() ? "Playback: ON" : "Playback: OFF", 190.f, 32.f, S.playbackOn() ? Style::Good : Style::Solid,
                     menu_selector(ControlPopup::onPlayback), 0, .45f), kCX + 95.f, y);
        txt("The loaded macro plays itself while you play.", "chatFont.fnt", .45f, T.muted, kCX + 202.f, y, CCPoint(0.f, .5f), 186.f);
    }

    void pageAnalyze() {
        auto const& T = theme();
        auto& S = Session::get();
        S.refresh();
        auto res = S.result();
        float y = kTop - 22.f;
        addBtn(mkBtn("Analyze macro", kCW, 44.f, Style::Accent, menu_selector(ControlPopup::onAnalyze), 0, .65f, 1.04f), kCX + kCW / 2, y);
        y -= 22.f + 8.f + 15.f;
        addBtn(mkBtn("Edit windows", 190.f, 30.f, Style::Solid, menu_selector(ControlPopup::onEdit), 0, .45f), kCX + 95.f, y);
        addBtn(mkBtn("Analysis settings", 190.f, 30.f, Style::Solid, menu_selector(ControlPopup::onAllSettings), 0, .45f), kCX + 295.f, y);

        const float top = y - 15.f - 8.f;
        addTL(mkCard(kCW, top - 10.f), kCX, top);
        category("TIMINGS", kCX + 14.f, top - 14.f);
        size_t measured = 0, total = 0;
        if (res) for (auto const& r : res->inputs) { ++total; if (r.status == InputStatus::Measured || r.status == InputStatus::Capped) ++measured; }
        const double sg = S.sigmaPerSec();
        std::string right = res ? fmt::format("{} / {} measured", measured, total) : "not analyzed yet";
        if (res && sg > 0.0) right += fmt::format("   -   {:.1f} sigma/s", sg);
        txt(right, "chatFont.fnt", .5f, res ? T.good : T.muted, kCX + kCW - 14.f, top - 14.f, CCPoint(1.f, .5f), 230.f);
        drawHist(kCX + 20.f, 10.f + 24.f, kCW - 40.f, std::max(20.f, top - 10.f - 70.f), true);
    }

    void pageTheme() {
        auto const& T = theme();
        category("MODE", kCX + 4.f, 246.f);
        addBtn(mkBtn("Dark", 190.f, 32.f, T.dark ? Style::Accent : Style::Solid, menu_selector(ControlPopup::onMode), 0, .5f), kCX + 95.f, 222.f);
        addBtn(mkBtn("Light", 190.f, 32.f, T.dark ? Style::Solid : Style::Accent, menu_selector(ControlPopup::onMode), 1, .5f), kCX + 295.f, 222.f);

        for (int which = 0; which < 2; ++which) {
            const float ly = which == 0 ? 192.f : 142.f;
            category(fmt::format("{}  -  {}", which == 0 ? "MAIN COLOUR" : "SECOND COLOUR", paletteName(themeAccentIndex(which))), kCX + 4.f, ly);
            for (int i = 0; i < kPaletteCount; ++i) {
                const bool sel = themeAccentIndex(which) == i;
                auto* node = CCNode::create();
                node->setContentSize({30.f, 30.f});
                node->setAnchorPoint({0.5f, 0.5f});
                if (sel) {
                    auto* ring = shapeRound(30.f, 30.f, 15.f, T.text);
                    ring->setPosition({15.f, 15.f});
                    node->addChild(ring, 0);
                    auto* gap = shapeRound(26.f, 26.f, 13.f, T.bg);
                    gap->setPosition({15.f, 15.f});
                    node->addChild(gap, 1);
                }
                auto* dot = shapeRound(22.f, 22.f, 11.f, paletteColor(i));
                dot->setPosition({15.f, 15.f});
                node->addChild(dot, 2);
                auto* it = CCMenuItemSpriteExtra::create(node, this, menu_selector(ControlPopup::onAccent));
                it->setTag(which * 100 + i);
                it->m_scaleMultiplier = 1.15f;
                addBtn(it, kCX + 19.5f + 39.f * static_cast<float>(i), ly - 8.f - 15.f);
            }
        }

        category("PRESETS", kCX + 4.f, 92.f);
        for (int i = 0; i < kPresetCount; ++i) {
            int a = 0, b = 0;
            presetColors(i, a, b);
            auto* node = CCNode::create();
            node->setContentSize({60.f, 26.f});
            node->setAnchorPoint({0.5f, 0.5f});
            auto* g = shapeGrad(60.f, 26.f, 13.f, paletteColor(a), paletteColor(b), true);
            g->setPosition({30.f, 13.f});
            node->addChild(g, 0);
            auto* l = CCLabelBMFont::create(presetName(i), "bigFont.fnt");
            l->setPosition({30.f, 13.f});
            node->addChild(l, 1);
            fitText(l, 52.f, 16.f, .32f);
            auto* it = CCMenuItemSpriteExtra::create(node, this, menu_selector(ControlPopup::onPreset));
            it->setTag(i);
            it->m_scaleMultiplier = 1.1f;
            addBtn(it, kCX + 30.f + 66.f * static_cast<float>(i), 72.f);
        }
        add(shapeGrad(kCW, 12.f, 6.f, T.a1, T.a2, true), kCX + kCW / 2, 40.f, 2);
        txt("Any two colours: they blend in buttons, highlights and the menu border.", "chatFont.fnt", .45f, T.muted, kCX + kCW / 2, 22.f, CCPoint(.5f, .5f), kCW);
    }

    struct Opt { char const* label; char const* key; };
    static constexpr Opt kOpts[12] = {
        {"Sidebar counter", "sidebar-enabled"}, {"Click markers", "marker-enabled"},   {"Frame sounds", "frame-sounds"},
        {"Sigma line", "show-sigma"},           {"Run readout", "readout-enabled"},    {"NaN caption", "nan-enabled"},
        {"Fix imported timing", "auto-timing"}, {"Exact replay", "exact-replay"},      {"Keep hold length", "keep-hold"},
        {"Mute analysis", "mute-analysis"},     {"Auto-save JSON", "export-on-finish"}, {"Screenshot mode", "screenshot-mode"},
    };

    void pageOptions() {
        auto const& T = theme();
        for (int i = 0; i < 12; ++i) {
            const bool on = Mod::get()->getSettingValue<bool>(kOpts[i].key);
            const float w = 190.f, h = 30.f;
            auto* node = CCNode::create();
            node->setContentSize({w, h});
            node->setAnchorPoint({0.5f, 0.5f});
            auto* bg = shapeRound(w, h, 15.f, T.pill);
            bg->setPosition({w / 2, h / 2});
            node->addChild(bg, 0);
            auto* l = CCLabelBMFont::create(kOpts[i].label, "bigFont.fnt");
            l->setColor(T.text);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition({14.f, h / 2});
            node->addChild(l, 1);
            fitText(l, 116.f, h - 10.f, .34f);
            auto* track = on ? shapeGrad(36.f, 18.f, 9.f, T.a1, T.a2, true) : shapeRound(36.f, 18.f, 9.f, T.line);
            track->setPosition({158.f, h / 2});
            node->addChild(track, 1);
            auto* knob = shapeRound(14.f, 14.f, 7.f, {255, 255, 255});
            knob->setPosition({on ? 167.f : 149.f, h / 2});
            node->addChild(knob, 2);
            auto* it = CCMenuItemSpriteExtra::create(node, this, menu_selector(ControlPopup::onOpt));
            it->setTag(i);
            it->m_scaleMultiplier = 1.04f;
            const float x = kCX + (i < 6 ? 95.f : 295.f);
            addBtn(it, x, kTop - 17.f - static_cast<float>(i % 6) * 33.f);
        }

        // Timings to show: one full-width row under the two columns (so the toggles above stay even). The Geode settings box
        // would not accept commas or dashes; this one allows exactly the characters a timing list uses.
        const float rowY = kTop - 17.f - 6.f * 33.f;
        const float rowW = kCW, rowH = 30.f;   // as wide as the two columns together
        add(shapeRound(rowW, rowH, 15.f, T.pill), kCX + kCW / 2, rowY, 1);
        auto* cap = CCLabelBMFont::create("Timings to show", "bigFont.fnt");
        cap->setColor(T.text);
        cap->setAnchorPoint({0.f, 0.5f});
        fitText(cap, 110.f, rowH - 10.f, .34f);
        add(cap, kCX + 14.f, rowY, 3);
        auto* input = TextInput::create(250.f, "1,2,3,4,5-6,7-8,9+", "bigFont.fnt");   // the GD font
        input->setFilter("0123456789,-+; ");
        input->setMaxCharCount(64);
        if (auto* bgs = input->getBGSprite()) bgs->setOpacity(0);   // no dark box: the row's own colour shows through
        input->setString(Mod::get()->getSettingValue<std::string>("timings"), false);
        input->setCallback([](std::string const& text) {
            Mod::get()->setSettingValue<std::string>("timings", text);   // the live-settings listener reloads the sidebar
        });
        add(input, kCX + kCW - 10.f - 125.f, rowY, 4);
    }

    // ---- handlers -----------------------------------------------------------------------------------------------------
    void onCloseBtn(CCObject*) { this->onClose(nullptr); }
    void onPage(CCObject* s) {
        const int p = static_cast<CCNode*>(s)->getTag();
        if (p == s_page) return;
        s_page = p;
        requestRebuild();
    }
    void onToggleMode(CCObject*) { setThemeDark(!theme().dark); requestRebuild(); }
    void onAllSettings(CCObject*) { openSettingsPopup(Mod::get(), false); }

    void onLoad(CCObject*) {
        if (Session::get().recording()) { alert("Recording", "Stop the recording first."); return; }
        pickMacro([self = Ref<ControlPopup>(this)] { self->requestRebuild(); });   // the menu stays open
    }
    void onAnalyze(CCObject*) {
        auto& S = Session::get();
        if (S.recording()) { alert("Recording", "Stop the recording first, then analyze."); return; }
        if (!S.macro()) { alert("Load a macro", "Load a macro first, then press Analyze."); return; }
        if (sim::Analyzer::get().active()) { alert("Busy", "An analysis is already running."); return; }
        if (S.macroFpsMismatch()) {
            const int need = static_cast<int>(std::lround(S.macro()->sourceTps));
            alert("Wrong FPS for this macro", fmt::format("This macro was recorded at {0} FPS, but FPS is set to {1}. Press the FPS button until it says {0}, then analyze again.", need, S.fps()));
            return;
        }
        this->onClose(nullptr);
        openProgressPopup();
    }
    void onFps(CCObject*) {
        auto& S = Session::get();
        if (S.recording()) { alert("Recording", "Stop the recording first, then change the FPS."); return; }
        if (sim::Analyzer::get().active()) { alert("Busy", "An analysis is running."); return; }
        S.cycleFps();
        requestRebuild();
        toast(fmt::format("FPS {} - set your TPS mod to {} as well. Recording checks that they match.", S.fps(), S.fps()), NotificationIcon::Info);
    }
    void onRecord(CCObject*) {
        auto& S = Session::get();
        if (!S.recording()) {
            auto err = S.startRecording();
            if (!err.empty()) { alert("Recording", err); return; }
            toast("Recording started from 0.00% (practice mode on).", NotificationIcon::Info);
        } else {
            S.releaseRecordSpeed();
            auto err = S.stopRecording();
            if (!err.empty()) alert("Windowpane", err);
            else toast("Recording stored.");
        }
        requestRebuild();
    }
    void onPlayback(CCObject*) {
        const bool on = Session::get().togglePlayback();
        toast(on ? "Playback on." : "Playback off.", NotificationIcon::Info);
        requestRebuild();
    }
    void onSpeed(CCObject*) {
        auto& S = Session::get();
        const double v = S.cycleRecordSpeed();
        if (v < 0.999 && !S.recording()) toast("Slow-down applies while recording.", NotificationIcon::Info);
        requestRebuild();
    }
    void onSpeedSet(CCObject* s) {
        static constexpr double steps[4] = {1.0, 0.5, 0.25, 0.1};
        const int i = std::clamp(static_cast<CCNode*>(s)->getTag(), 0, 3);
        Mod::get()->setSavedValue<double>("record-speed", steps[i]);
        requestRebuild();
    }
    void onClear(CCObject*) {
        auto& S = Session::get();
        if (S.recording()) { alert("Recording", "Stop the recording first."); return; }
        S.setMacro(nullptr);
        S.lastMessage.clear();
        requestRebuild();
    }
    void onEdit(CCObject*) { openEditWindowsPopup(); }
    void onSaveJson(CCObject*) { saveJsonNow(); }
    void onCopy(CCObject*) { copySummary(); }

    void onSave(CCObject*) {
        auto& S = Session::get();
        auto m = S.macro();
        if (!m) { alert("Nothing to save", "Load or record a macro first."); return; }
        std::string base = m->name.empty() ? "macro" : m->name;
#ifdef GEODE_IS_WINDOWS
        // Async: the dialog runs on its own thread (see PickFile.hpp); the file is written once a place is chosen.
        const bool started = nativeSaveFileAsync(base, "wpmacro", "Windowpane macro (*.wpmacro)",
            [self = Ref<ControlPopup>(this)](std::optional<std::filesystem::path> picked) {
                if (!picked) return;   // cancelled
                self->writeMacro(*picked);
            });
        if (!started) toast("A file dialog is already open.", NotificationIcon::Warning);
#else
        auto path = Mod::get()->getSaveDir() / "macros" / (base + ".wpmacro");
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        writeMacro(path);
#endif
    }
    void writeMacro(std::filesystem::path const& path) {
        auto& S = Session::get();
        auto err = S.saveMacroFile(path);
        if (!err.empty()) { alert("Could not save", err); return; }
        toast(S.hasResult() ? "Saved " + path.filename().string() + " + analysis" : "Saved " + path.filename().string() + " (no analysis)");
    }

    void onMode(CCObject* s) {
        setThemeDark(static_cast<CCNode*>(s)->getTag() == 0);
        requestRebuild();
    }
    void onAccent(CCObject* s) {
        const int t = static_cast<CCNode*>(s)->getTag();
        setThemeAccent(t / 100, t % 100);
        requestRebuild();
    }
    void onPreset(CCObject* s) {
        setThemePreset(static_cast<CCNode*>(s)->getTag());
        requestRebuild();
    }
    void onOpt(CCObject* s) {
        const int i = std::clamp(static_cast<CCNode*>(s)->getTag(), 0, 11);
        auto* mod = Mod::get();
        mod->setSettingValue<bool>(kOpts[i].key, !mod->getSettingValue<bool>(kOpts[i].key));
        requestRebuild();
    }
};

}  // namespace

void openControlPopup() {
    if (!Session::get().pl()) return;
    if (auto* p = ControlPopup::create()) p->show();
}

}  // namespace wp::ui
