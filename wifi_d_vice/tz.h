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
