#pragma once
#include <stdint.h>

// Universal accent color. The setting lives in NVS. Themes read these values to tint themselves.
enum {
  ACCENT_CYAN = 0, ACCENT_AMBER, ACCENT_GREEN, ACCENT_GREY,
  ACCENT_RED, ACCENT_ORANGE, ACCENT_YELLOW, ACCENT_LIME,
  ACCENT_BLUE, ACCENT_INDIGO, ACCENT_PURPLE, ACCENT_MAGENTA,
  ACCENT_PINK, ACCENT_SKY, ACCENT_WHITE, ACCENT_ROSE,
  ACCENT_N,
};

void        accentLoad();
void        accentSet(int id);
int         accentGet();
const char *accentName(int id);

uint16_t accentFill();
uint16_t accentEdge();
uint16_t accentBevel();
uint16_t accentEmboss();
uint16_t accentTitleBar();
uint16_t accentLabel();

// Return colors for any accent ID. The picker previews all swatches at once.
uint16_t accentFillFor(int id);
uint16_t accentEdgeFor(int id);

// Convert RGB565 to three 0-255 channels. The function drives PWM hardware.
void accentRGB(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b);
