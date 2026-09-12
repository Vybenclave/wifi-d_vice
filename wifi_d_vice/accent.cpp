#include "accent.h"
#include <Preferences.h>

static int s_id = ACCENT_CYAN;
static const char *NAMES[ACCENT_N] = {
  "Cyan", "Amber", "Green", "Grey", "Red", "Orange", "Yellow", "Lime",
  "Blue", "Indigo", "Purple", "Magenta", "Pink", "Sky", "White", "Rose",
};

// fill/edge/bevel/emboss are the Vice pill-button colors, titleBar is
// uiDrawTopBar()'s fill, label is accentLabel() -- the field-label text
// color used everywhere, under Basic AND Vice. Cyan's values are exactly
// the original hardcoded Vice palette (including ILI9341_NAVY's 0x000F
// for the title bar and ILI9341_CYAN for the label) -- unchanged, so a
// stock cyan setup looks identical to before this file existed. Every
// other entry is generated from one "fill" hue by the same formula (see
// $CLAUDE_JOB_DIR/tmp/accent16.cpp if this table needs regenerating or
// extending): edge = fill*0.36 (a darker border, same hue), bevel =
// fill*0.38 + white*0.62 (light top highlight), emboss = fill*0.25 +
// white*0.75 (slightly lighter than bevel -- the 1px offset behind a
// button's label), titleBar = fill*0.18 (dim, near-black, same hue),
// label = fill (the field-label text is just the vivid color itself).
// Amber/green were tuned FIRST as actual 80s-monochrome-monitor phosphor
// (bright/saturated, not a muted "material design" tint); the formula
// above was reverse-engineered from their numbers so the rest of the
// palette keeps the same visual relationship.
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
  // red: fill(255, 40, 40)
  { 0xF945, 0x5882, 0xFD75, 0xFE58, 0x3041, 0xF945 },
  // orange: fill(255,100,  0)
  { 0xFB20, 0x5920, 0xFE13, 0xFEB7, 0x3080, 0xFB20 },
  // yellow: fill(255,220,  0)
  { 0xFEC0, 0x5A80, 0xFF93, 0xFFB7, 0x3140, 0xFEC0 },
  // lime: fill(170,255, 40)
  { 0xAFE5, 0x3AE2, 0xDFF5, 0xE7F8, 0x2161, 0xAFE5 },
  // blue: fill( 40,130,255)
  { 0x2C1F, 0x118B, 0xAE7F, 0xC6FF, 0x08C6, 0x2C1F },
  // indigo: fill( 90, 70,230)
  { 0x5A3C, 0x20CA, 0xBDDE, 0xD69E, 0x1065, 0x5A3C },
  // purple: fill(170, 60,220)
  { 0xA9FB, 0x38AA, 0xDDBD, 0xE67E, 0x2065, 0xA9FB },
  // magenta: fill(230, 40,200)
  { 0xE158, 0x5089, 0xF57C, 0xF65D, 0x2844, 0xE158 },
  // pink: fill(255,110,170)
  { 0xFB75, 0x5947, 0xFE3B, 0xFEDC, 0x30A4, 0xFB75 },
  // sky: fill( 90,200,255)
  { 0x5E3F, 0x224B, 0xBF5F, 0xD79F, 0x1126, 0x5E3F },
  // white: fill(230,235,240)
  { 0xE75D, 0x52AB, 0xF7BE, 0xF7DF, 0x2945, 0xE75D },
  // rose: fill(200, 20, 60)
  { 0xC0A7, 0x4843, 0xE536, 0xEE19, 0x2021, 0xC0A7 },
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
