#include "modvis.h"
#include <Preferences.h>

static const char *NAMES[MOD_N] = {
  "WiFi scan", "Net stats", "WiFi IDS", "Wardrive",
  "BLE scan",  "Tracker detect",
  "Flock detect", "Skimmer detect", "SubGHz sweep", "Meshtastic",
  "Engagement",
  "Rogue AP",
};

static uint32_t s_hidden = 0;   // bit i set => module i hidden

void modvisLoad() {
  Preferences p;
  p.begin("touchcal", true);
  s_hidden = p.getUInt("modhide", 0);
  p.end();
}

bool modvisHidden(int id) {
  return id >= 0 && id < MOD_N && (s_hidden & (1u << id));
}

void modvisSetHidden(int id, bool hidden) {
  if (id < 0 || id >= MOD_N) return;
  if (hidden) s_hidden |= (1u << id);
  else        s_hidden &= ~(1u << id);
  Preferences p;
  p.begin("touchcal", false);
  p.putUInt("modhide", s_hidden);
  p.end();
}

const char *modvisName(int id) {
  return (id >= 0 && id < MOD_N) ? NAMES[id] : "?";
}
