#pragma once
#include <stdint.h>

// Two UI families -- STRUCTURE only, not color (see accent.h for that,
// a separate setting that applies under either theme). "Basic" = flat
// black + white-rect buttons. "Vice" = synthwave: pill buttons + a purple
// gradient behind the home menu. Persisted to NVS ("theme" key).
//
// A future SD-loadable custom theme (a /vice_themes/ folder on the SD
// card -- see the project roadmap) would add more THEME_* structural
// choices here, each free to reference accent.h's accessors for "the
// user's chosen color" the same way Vice does, rather than hardcoding
// its own palette.
enum { THEME_BASIC = 0, THEME_VICE = 1, THEME_N = 2 };

void        themeLoad();            // read from NVS -- call once at boot
void        themeSet(int id);      // set + persist
int         themeGet();
const char *themeName(int id);
bool        themeIsVice();

// Fixed structural constants for the Vice pill-button look -- NOT part of
// accent.h because they don't vary with the chosen color: every accent
// still uses a dark-grey button label and the same indigo/magenta home-
// menu gradient.
static const uint16_t TH_BTN_TEXT  = 0x2965;   // dark grey label, inside a Vice pill button
static const uint16_t TH_GRAD_TOP  = 0x0844;   // deep indigo, Vice's home-menu gradient
static const uint16_t TH_GRAD_BOT  = 0x60EF;   // magenta-purple
