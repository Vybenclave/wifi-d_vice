#include "accent.h"
#include <Preferences.h>

static int s_id = ACCENT_CYAN;
static const char *NAMES[ACCENT_N] = {
  "Cyan", "Amber", "Green", "Grey", "Red", "Orange", "Yellow", "Lime",
  "Blue", "Indigo", "Purple", "Magenta", "Pink", "Sky", "White", "Rose",
};

// Each row has one fill hue. The other fields derive from it by this
// formula: edge = fill*0.36, bevel = fill*0.38 + white*0.62,
// emboss = fill*0.25 + white*0.75, titleBar = fill*0.18, label = fill.
// Use the same formula to add a new color.
struct AccentPalette { uint16_t fill, edge, bevel, emboss, titleBar, label; };
static const AccentPalette PAL[] = {
  { 0x2E57, 0x0AC9, 0x8FBD, 0xAF7B, 0x000F, 0x07FF },
  { 0xFD60, 0x5A00, 0xFF13, 0xFF57, 0x3100, 0xFD60 },
  { 0x37E6, 0x12E2, 0xAFF5, 0xC7F8, 0x0140, 0x37E6 },
  { 0xB5F8, 0x73AF, 0xEF9E, 0xF7DF, 0x2145, 0xDF1C },
  { 0xF945, 0x5882, 0xFD75, 0xFE58, 0x3041, 0xF945 },
  { 0xFB20, 0x5920, 0xFE13, 0xFEB7, 0x3080, 0xFB20 },
  { 0xFEC0, 0x5A80, 0xFF93, 0xFFB7, 0x3140, 0xFEC0 },
  { 0xAFE5, 0x3AE2, 0xDFF5, 0xE7F8, 0x2161, 0xAFE5 },
  { 0x2C1F, 0x118B, 0xAE7F, 0xC6FF, 0x08C6, 0x2C1F },
  { 0x5A3C, 0x20CA, 0xBDDE, 0xD69E, 0x1065, 0x5A3C },
  { 0xA9FB, 0x38AA, 0xDDBD, 0xE67E, 0x2065, 0xA9FB },
  { 0xE158, 0x5089, 0xF57C, 0xF65D, 0x2844, 0xE158 },
  { 0xFB75, 0x5947, 0xFE3B, 0xFEDC, 0x30A4, 0xFB75 },
  { 0x5E3F, 0x224B, 0xBF5F, 0xD79F, 0x1126, 0x5E3F },
  { 0xE75D, 0x52AB, 0xF7BE, 0xF7DF, 0x2945, 0xE75D },
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
uint16_t accentLabel()     { return active().label; }   // Apply this color to both Basic and Vice UI modes.

uint16_t accentFillFor(int id) { return paletteFor(id).fill; }
uint16_t accentEdgeFor(int id) { return paletteFor(id).edge; }

void accentRGB(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
  r = (r5 << 3) | (r5 >> 2);
  g = (g6 << 2) | (g6 >> 4);
  b = (b5 << 3) | (b5 >> 2);
}
