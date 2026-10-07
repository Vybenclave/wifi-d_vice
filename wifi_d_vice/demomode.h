#pragma once
#include <Arduino.h>
// Global demo mode toggle. The system injects synthetic hits on a timer.
// The UI exercises without real radio activity. Preferences store the state.
void demoModeLoad();                 // call once at boot, alongside themeLoad()/accentLoad()
bool demoModeEnabled();
void demoModeSetEnabled(bool on);

// Shared fake-data helpers. Each screen uses these to avoid duplication.
// The data stays simple to exercise the UI.
void   demoRandMac(uint8_t mac[6]);   // Set bit 0x02. The MAC never collides with a real OUI.
String demoRandMacStr(const uint8_t mac[6]);
int    demoRandRssi();
const char *demoRandPick(const char *const *pool, int n);
