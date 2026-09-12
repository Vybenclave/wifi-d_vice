#pragma once
#include <stdint.h>

// Universal accent color -- independent of theme.h's Basic/Vice choice.
// Switching theme does not change the accent, and switching accent does
// not change the theme; each is its own persisted setting (NVS "accent"
// key here, "theme" key in theme.cpp). Any theme's rendering can read
// these to tint itself: Vice uses accentFill/Edge/Bevel/Emboss for its
// pill buttons and accentTitleBar() for the top bar; accentLabel() (the
// app-wide field-label/heading text color -- SSID:, MAC:, section
// headers, etc.) applies under BOTH themes, since that one really is a
// "universal ui variable" rather than a Vice-specific look.
//
// A future SD-loadable custom theme (a /vice_themes/ folder -- see the
// project roadmap) would reference these same accessors for "the user's
// chosen color" rather than hardcoding its own.
enum { ACCENT_CYAN = 0, ACCENT_AMBER = 1, ACCENT_GREEN = 2, ACCENT_GREY = 3, ACCENT_N = 4 };

void        accentLoad();          // read from NVS -- call once at boot
void        accentSet(int id);     // set + persist
int         accentGet();
const char *accentName(int id);

uint16_t accentFill();          // vivid tone -- Vice button fill
uint16_t accentEdge();          // darker border
uint16_t accentBevel();         // light top highlight
uint16_t accentEmboss();        // 1px light offset behind the button label
uint16_t accentTitleBar();      // dim/near-black tone -- Vice's top bar
uint16_t accentLabel();         // field-label/heading text -- Basic AND Vice

// Fill/edge for an ARBITRARY accent id, not just the active one -- for a
// picker that needs to preview every color swatch at once regardless of
// which one is currently selected.
uint16_t accentFillFor(int id);
uint16_t accentEdgeFor(int id);
