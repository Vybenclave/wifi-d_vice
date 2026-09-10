#pragma once
// Per-module visibility. Each feature screen has a stable id here (kept
// independent of the .ino's Screen enum so this compiles standalone). A
// hidden module is dropped from its submenu; if a whole category ends up
// empty the category button hides too. State is one NVS bitmask.

enum {
  MOD_WIFI_SCAN, MOD_NET_STATS, MOD_WIFI_IDS, MOD_GPS,
  MOD_BLE_SCAN,  MOD_TRACKER,
  MOD_FLOCK,     MOD_SKIMMER,   MOD_SUBGHZ,   MOD_MESHTASTIC,
  MOD_ENGAGEMENT,
  MOD_ROGUE_AP,   // appended (not inserted) so existing "modhide" NVS bits stay aligned
  MOD_PROBE_WATCH, MOD_CLIENT_MAP, MOD_CAMERA, MOD_DRONE, MOD_BLE_SPAM,   // likewise appended
  MOD_N
};

void        modvisLoad();                     // call once at boot
bool        modvisHidden(int id);
void        modvisSetHidden(int id, bool hidden);   // persists immediately
const char *modvisName(int id);
