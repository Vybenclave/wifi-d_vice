#include "demomode.h"
#include <Preferences.h>
#include <esp_random.h>

static bool s_on = false;

void demoModeLoad() {
  Preferences p;
  p.begin("demomode", true);
  s_on = p.getBool("on", false);
  p.end();
}

bool demoModeEnabled() { return s_on; }

void demoModeSetEnabled(bool on) {
  s_on = on;
  Preferences p;
  p.begin("demomode", false);
  p.putBool("on", on);
  p.end();
}

void demoRandMac(uint8_t mac[6]) {
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(esp_random() & 0xFF);
  mac[0] = (uint8_t)((mac[0] & 0xFC) | 0x02);   // Set the locally administered and unicast bits.
}

String demoRandMacStr(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

int demoRandRssi() { return -35 - (int)(esp_random() % 56); }   // Return a value between -35 and -90.

const char *demoRandPick(const char *const *pool, int n) {
  if (n <= 0) return "";
  return pool[esp_random() % n];
}
