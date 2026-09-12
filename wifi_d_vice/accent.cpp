#include "accent.h"
#include <Preferences.h>

static int s_id = ACCENT_CYAN;
static const char *NAMES[ACCENT_N] = { "Cyan", "Amber", "Green", "Grey" };

// fill/edge/bevel/emboss are the Vice pill-button colors, titleBar is
// uiDrawTopBar()'s fill, label is accentLabel() -- the field-label text
// color used everywhere, under Basic AND Vice. Cyan's values are exactly
// the original hardcoded Vice palette (including ILI9341_NAVY's 0x000F
// for the title bar and ILI9341_CYAN for the label) -- unchanged, so a
// stock cyan setup looks identical to before this file existed. Amber and
// green are tuned to read as glowing 80s-monochrome-monitor phosphor --
// bright and saturated, not a muted "material design" brown/forest tint
// -- so button fill and label share the same vivid hue, and the title
// bar is a dim, near-black shade of that same hue rather than an
// unrelated brown/dark-green.
struct AccentPalette { uint16_t fill, edge, bevel, emboss, titleBar, label; };
static const AccentPalette PAL[] = {
  // cyan:  teal buttons / navy bar / cyan label
  { 0x2E57, 0x0AC9, 0x8FBD, 0xAF7B, 0x000F, 0x07FF },
  // amber: P3-phosphor amber (~#FFB000) buttons + label / dim-amber bar
  { 0xFD60, 0x5A00, 0xFF13, 0xFF57, 0x3100, 0xFD60 },
  // green: P1-phosphor / Hercules green (~#33FF33) buttons + label / dim-green bar
  { 0x37E6, 0x12E2, 0xAFF5, 0xC7F8, 0x0140, 0x37E6 },
  // grey:  light grey buttons / dark grey bar / brighter grey label
  { 0xB5F8, 0x73AF, 0xEF9E, 0xF7DF, 0x2145, 0xDF1C },
};

static const AccentPalette &paletteFor(int id) {
  if (id < 0 || id >= ACCENT_N) id = ACCENT_CYAN;
  return PAL[id];
}
static const AccentPalette &active() { return paletteFor(s_id); }

void accentLoad() {
  Preferences p;
  p.begin("touchcal", true);
  s_id = p.getInt("accent", ACCENT_CYAN);
  p.end();
  if (s_id < 0 || s_id >= ACCENT_N) s_id = ACCENT_CYAN;
}

void accentSet(int id) {
  if (id < 0 || id >= ACCENT_N) return;
  s_id = id;
  Preferences p;
  p.begin("touchcal", false);
  p.putInt("accent", id);
  p.end();
}

int         accentGet()        { return s_id; }
const char *accentName(int id) { return (id >= 0 && id < ACCENT_N) ? NAMES[id] : "?"; }

uint16_t accentFill()      { return active().fill; }
uint16_t accentEdge()      { return active().edge; }
uint16_t accentBevel()     { return active().bevel; }
uint16_t accentEmboss()    { return active().emboss; }
uint16_t accentTitleBar()  { return active().titleBar; }
uint16_t accentLabel()     { return active().label; }   // universal -- Basic AND Vice

uint16_t accentFillFor(int id) { return paletteFor(id).fill; }
uint16_t accentEdgeFor(int id) { return paletteFor(id).edge; }
