#pragma once
#include <stdint.h>

// Two UI themes. "Basic" = the original flat black + white-rect buttons.
// "Vice" = synthwave: pill buttons in teal, a purple gradient behind the
// home menu, cyan accents. Persisted to NVS ("theme" key).
enum { THEME_BASIC = 0, THEME_VICE = 1, THEME_N = 2 };

void        themeLoad();            // read from NVS -- call once at boot
void        themeSet(int id);      // set + persist
int         themeGet();
const char *themeName(int id);
bool        themeIsVice();

// Vice palette (RGB565). Basic doesn't use these.
static const uint16_t TH_BTN_FILL  = 0x2E57;   // teal
static const uint16_t TH_BTN_EDGE  = 0x0AC9;   // dark teal border
static const uint16_t TH_BTN_BEVEL = 0x8FBD;   // light top highlight
static const uint16_t TH_BTN_TEXT  = 0x2965;   // dark grey label
static const uint16_t TH_BTN_EMBOSS= 0xAF7B;   // 1px light offset behind the label
static const uint16_t TH_ACCENT    = 0x5F1A;   // cyan
static const uint16_t TH_GRAD_TOP  = 0x0844;   // deep indigo
static const uint16_t TH_GRAD_BOT  = 0x60EF;   // magenta-purple
