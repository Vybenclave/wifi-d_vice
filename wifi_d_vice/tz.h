#pragma once
// Display-only timezone: a fixed UTC-offset picker (System > Display >
// Timezone), persisted to NVS ("tz" key). This is NOT a DST-aware IANA
// timezone -- there's no tz database on this MCU and DST rules drift too
// often to hardcode -- it's a plain minute offset from UTC, same approach
// most embedded tz pickers use. It only affects the bottom-bar clock
// (ui.cpp); devtime.h and every SD log stay UTC always, per the project's
// "device time is UTC" rule.
#include <stdint.h>

void        tzLoad();          // read from NVS -- call once at boot
int         tzCount();
const char *tzLabel(int idx);
void        tzSetIndex(int idx);   // set + persist
int         tzGetIndex();
int         tzOffsetMinutes();     // convenience: tzOffsetMinutes(tzGetIndex())

// 12-hour ("1:07pm") vs 24-hour ("13:07") display for the bottom-bar clock
// (same System > Display > Timezone screen). Defaults to 24h. Display-only,
// same as the offset above -- devtime.h and every log are unaffected.
bool        tzUse24h();
void        tzSet24h(bool on);     // set + persist
