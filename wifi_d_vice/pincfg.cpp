#include "pincfg.h"
#include "pins.h"
#include <Preferences.h>

struct PinDef { const char *name; const char *key; int def; };

static const PinDef DEFS[PIN_N] = {
  { "CC1101 CS",   "cc_cs",   CC1101_CS   },
  { "2.4GHz CS",   "r24_cs",  RADIO24_CS  },
  { "2.4GHz IRQ",  "r24_irq", RADIO24_IRQ },
  { "CC1101 GDO0", "cc_gdo0", CC1101_GDO0 },
};

static int  s_val[PIN_N];
static bool s_cc1101  = true;
static int  s_radio24 = RADIO24_NONE;

void pincfgLoad() {
  Preferences p;
  p.begin("pins", true);
  for (int i = 0; i < PIN_N; i++) s_val[i] = p.getInt(DEFS[i].key, DEFS[i].def);
  s_cc1101  = p.getBool("cc1101", true);
  s_radio24 = p.getInt("radio24", RADIO24_NONE);
  p.end();
  for (int i = 0; i < PIN_N; i++)
    if (s_val[i] < -1 || s_val[i] > 39) s_val[i] = DEFS[i].def;
  if (s_radio24 < RADIO24_NONE || s_radio24 > RADIO24_CC2500) s_radio24 = RADIO24_NONE;
}

int pincfgGet(int id)     { return (id >= 0 && id < PIN_N) ? s_val[id]     : -1; }
int pincfgDefault(int id) { return (id >= 0 && id < PIN_N) ? DEFS[id].def  : -1; }
const char *pincfgName(int id) { return (id >= 0 && id < PIN_N) ? DEFS[id].name : "?"; }

void pincfgSet(int id, int gpio) {
  if (id < 0 || id >= PIN_N || gpio < -1 || gpio > 39) return;
  s_val[id] = gpio;
  Preferences p;
  p.begin("pins", false);
  p.putInt(DEFS[id].key, gpio);
  p.end();
}

void pincfgResetAll() {
  Preferences p;
  p.begin("pins", false);
  p.clear();
  p.end();
  for (int i = 0; i < PIN_N; i++) s_val[i] = DEFS[i].def;
  s_cc1101  = true;
  s_radio24 = RADIO24_NONE;
}

// --- add-on radio presence ---
bool pincfgCC1101()  { return s_cc1101; }
int  pincfgRadio24() { return s_radio24; }

void pincfgSetCC1101(bool on) {
  s_cc1101 = on;
  Preferences p;
  p.begin("pins", false);
  p.putBool("cc1101", on);
  p.end();
}

void pincfgSetRadio24(int which) {
  if (which < RADIO24_NONE || which > RADIO24_CC2500) return;
  s_radio24 = which;
  Preferences p;
  p.begin("pins", false);
  p.putInt("radio24", which);
  p.end();
}

const char *pincfgRadio24Name(int which) {
  switch (which) {
    case RADIO24_NRF24:  return "nRF24L01+";
    case RADIO24_CC2500: return "CC2500";
    default:             return "none";
  }
}
