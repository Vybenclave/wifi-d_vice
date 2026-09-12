#include "theme.h"
#include <Preferences.h>

static int s_id = THEME_BASIC;
static const char *NAMES[THEME_N] = {
  "Basic", "Vice (cyan)", "Vice (amber)", "Vice (green)", "Vice (grey)",
};

// = ILI9341_CYAN (0x07FF). Basic's fixed label color, and also the cyan
// scheme's -- kept as a literal instead of pulling in the display header
// just for one macro.
static const uint16_t BASIC_LABEL = 0x07FF;

// One row per THEME_VICE_* scheme (indexed by id - THEME_VICE_CYAN below).
// fill/edge/bevel/emboss are the button colors, titleBar is uiDrawTopBar()'s
// fill, label is the field-label/heading text color used all over the app
// (thLabel() -- every screen that used to hardcode ILI9341_CYAN for that
// calls it now). Cyan's values are exactly the original hardcoded Vice
// palette (including ILI9341_NAVY's 0x000F for the title bar and
// ILI9341_CYAN for the label) -- unchanged, per "keep current cyan
// buttons as an option". Amber and green are tuned to read as glowing
// phosphor -- an actual 80s amber/green monochrome monitor's single color
// is bright and saturated, not a muted "material design" brown/forest
// tint -- so button fill and label share the same vivid hue, and the
// title bar is a dim, near-black shade of that same hue rather than an
// unrelated brown/dark-green.
struct VicePalette { uint16_t fill, edge, bevel, emboss, titleBar, label; };
static const VicePalette VICE_PAL[] = {
  // cyan:  teal buttons / navy bar / cyan label (unchanged)
  { 0x2E57, 0x0AC9, 0x8FBD, 0xAF7B, 0x000F, 0x07FF },
  // amber: P3-phosphor amber (~#FFB000) buttons + label / dim-amber bar
  { 0xFD60, 0x5A00, 0xFF13, 0xFF57, 0x3100, 0xFD60 },
  // green: P1-phosphor / Hercules green (~#33FF33) buttons + label / dim-green bar
  { 0x37E6, 0x12E2, 0xAFF5, 0xC7F8, 0x0140, 0x37E6 },
  // grey:  light grey buttons / dark grey bar / brighter grey label
  { 0xB5F8, 0x73AF, 0xEF9E, 0xF7DF, 0x2145, 0xDF1C },
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
uint16_t thLabel()     { return themeIsVice() ? activeVice().label : BASIC_LABEL; }
