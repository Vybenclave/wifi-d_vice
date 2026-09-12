#include "theme.h"
#include <Preferences.h>

static int s_id = THEME_BASIC;
static const char *NAMES[THEME_N] = {
  "Basic", "Vice (cyan)", "Vice (amber)", "Vice (green)", "Vice (grey)",
};

// One row per THEME_VICE_* scheme (indexed by id - THEME_VICE_CYAN below).
// fill/edge/bevel/emboss are the button colors; titleBar is uiDrawTopBar()'s
// fill. Cyan's values are exactly the original hardcoded Vice palette
// (including ILI9341_NAVY's 0x000F for the title bar) -- unchanged, per
// "keep current cyan buttons as an option".
struct VicePalette { uint16_t fill, edge, bevel, emboss, titleBar; };
static const VicePalette VICE_PAL[] = {
  { 0x2E57, 0x0AC9, 0x8FBD, 0xAF7B, 0x000F },   // cyan:  teal / navy
  { 0xCC40, 0x7240, 0xFEF3, 0xFF56, 0x3922 },   // amber: amber / brown
  { 0x2C67, 0x1203, 0xA735, 0xC7B9, 0x0962 },   // green: green / dark green
  { 0xB5F8, 0x73AF, 0xEF9E, 0xF7DF, 0x2145 },   // grey:  light grey / dark grey
};

static const VicePalette &activeVice() {
  int i = s_id - THEME_VICE_CYAN;
  if (i < 0 || i >= (int)(sizeof(VICE_PAL) / sizeof(VICE_PAL[0]))) i = 0;
  return VICE_PAL[i];
}

void themeLoad() {
  Preferences p;
  p.begin("touchcal", true);
  s_id = p.getInt("theme", THEME_BASIC);
  p.end();
  if (s_id < 0 || s_id >= THEME_N) s_id = THEME_BASIC;
}

void themeSet(int id) {
  if (id < 0 || id >= THEME_N) return;
  s_id = id;
  Preferences p;
  p.begin("touchcal", false);
  p.putInt("theme", id);
  p.end();
}

int         themeGet()          { return s_id; }
const char *themeName(int id)   { return (id >= 0 && id < THEME_N) ? NAMES[id] : "?"; }
bool        themeIsVice()       { return s_id != THEME_BASIC; }

uint16_t thBtnFill()   { return activeVice().fill; }
uint16_t thBtnEdge()   { return activeVice().edge; }
uint16_t thBtnBevel()  { return activeVice().bevel; }
uint16_t thBtnEmboss() { return activeVice().emboss; }
uint16_t thTitleBar()  { return activeVice().titleBar; }
