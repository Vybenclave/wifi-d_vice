#pragma once
// Display-only timezone: a fixed UTC-offset picker (System > Display >
// Timezone), persisted to NVS ("tz" key). It only affects the bottom-bar
// clock (ui.cpp); devtime.h and every SD log stay UTC always, per the
// project's "device time is UTC" rule.
//
// There's no IANA tz database on this MCU (way too much flash for what it
// buys here), so each zone instead carries a small hardcoded DST rule --
// "2nd Sunday of March to 1st Sunday of November" and the like -- good
// enough for the handful of rule shapes that actually exist (US/Canada,
// the EU, and the two southern-hemisphere variants), applied with plain
// Gregorian-calendar math (see tz.cpp). It needs a real UTC date to
// evaluate against, so it silently falls back to standard time (no +1h)
// until devtime.h has a synced clock.
#include <stdint.h>

void        tzLoad();          // read from NVS -- call once at boot
int         tzCount();
const char *tzLabel(int idx);
void        tzSetIndex(int idx);   // set + persist
int         tzGetIndex();
int         tzOffsetMinutes();     // convenience: tzOffsetMinutesFor(tzGetIndex())

// 12-hour ("1:07pm") vs 24-hour ("13:07") display for the bottom-bar clock
// (same System > Display > Timezone screen). Defaults to 24h. Display-only,
// same as the offset above -- devtime.h and every log are unaffected.
bool        tzUse24h();
void        tzSet24h(bool on);     // set + persist

// Automatic DST compensation -- defaults on. When on, a zone whose rule
// currently says "daylight time" gets +60 minutes added to its base UTC
// offset; a zone with no rule (most of the world doesn't observe DST) is
// unaffected either way, so leaving this on is always safe.
bool        tzAutoDst();
void        tzSetAutoDst(bool on);         // set + persist
bool        tzZoneHasDst(int idx);         // does this zone's rule ever apply DST at all
bool        tzDstActiveFor(int idx);       // is it in effect for that zone right now
int         tzOffsetMinutesFor(int idx);   // idx's base offset, +60 if tzAutoDst() && active
