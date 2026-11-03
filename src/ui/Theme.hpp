#pragma once
// Look of the Windowpane menu: light / dark mode, two accent colours the user can change, and the small drawing helpers
// (rounded shapes, gradients) the menu is built from. Nothing here touches the macro or the game state.

#include <Geode/Geode.hpp>
#include <string>

namespace wp::ui {

struct Theme {
    bool dark = true;
    cocos2d::ccColor3B a1{34, 200, 255}, a2{168, 64, 255};   // the two accent colours (gradients run a1 -> a2)
    cocos2d::ccColor3B bg{}, card{}, cardHi{}, pill{}, line{}, text{}, muted{}, good{}, warn{}, bad{};
};

Theme const& theme();                       // current theme (loaded from the saved settings on first use)
void reloadTheme();                         // re-read the saved settings
void setThemeDark(bool dark);
void setThemeAccent(int which, int paletteIndex);   // which: 0 = main colour, 1 = second colour
void setThemePreset(int preset);
int themeAccentIndex(int which);

constexpr int kPaletteCount = 10;
constexpr int kPresetCount = 6;
cocos2d::ccColor3B paletteColor(int i);
char const* paletteName(int i);
char const* presetName(int i);
void presetColors(int i, int& a, int& b);   // palette indices of a preset

// Centered nodes (anchor 0.5, 0.5) of the given size; position them by their centre.
cocos2d::CCNode* shapeRound(float w, float h, float r, cocos2d::ccColor3B c, GLubyte a = 255);
cocos2d::CCNode* shapeGrad(float w, float h, float r, cocos2d::ccColor3B c1, cocos2d::ccColor3B c2, bool horizontal, GLubyte a = 255);

// Scales `l` down (never above maxScale) so its text fits in maxW x maxH.
void fitText(cocos2d::CCLabelBMFont* l, float maxW, float maxH, float maxScale);

}  // namespace wp::ui
