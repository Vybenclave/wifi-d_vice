#pragma once
#include <stdint.h>

// Two UI families. "Basic" = the original flat black + white-rect buttons.
// "Vice" = synthwave: pill buttons, a purple gradient behind the home
// menu, cyan accents -- now split into four color schemes that recolor
// just the buttons and the title bar (everything else -- the gradient,
// the accent stripe, the button label color -- stays the same across all
// four, since that's the shared "Vice" identity). Persisted to NVS
// ("theme" key).
enum {
  THEME_BASIC      = 0,
  THEME_VICE_CYAN  = 1,   // the original scheme -- teal buttons, navy title bar
  THEME_VICE_AMBER = 2,   // amber buttons, brown title bar
  THEME_VICE_GREEN = 3,   // green buttons, dark green title bar
  THEME_VICE_GREY  = 4,   // light grey buttons, dark grey title bar
  THEME_N          = 5,
};

void        themeLoad();            // read from NVS -- call once at boot
void        themeSet(int id);      // set + persist
int         themeGet();
const char *themeName(int id);
bool        themeIsVice();          // true for any THEME_VICE_* scheme

// Vice palette (RGB565); Basic doesn't use any of this. The button fill /
// edge / highlight / label-emboss and the title-bar fill vary by the
// active color scheme (see theme.cpp) -- everything else below is shared
// across all four schemes.
uint16_t thBtnFill();
uint16_t thBtnEdge();
uint16_t thBtnBevel();
uint16_t thBtnEmboss();   // 1px light offset behind the button label
uint16_t thTitleBar();    // top-bar fill (ILI9341_NAVY for the cyan scheme)
static const uint16_t TH_BTN_TEXT  = 0x2965;   // dark grey label, all schemes
static const uint16_t TH_ACCENT    = 0x5F1A;   // cyan accent, all schemes
static const uint16_t TH_GRAD_TOP  = 0x0844;   // deep indigo, all schemes
static const uint16_t TH_GRAD_BOT  = 0x60EF;   // magenta-purple, all schemes
