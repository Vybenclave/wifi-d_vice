#include "tz.h"
#include <Preferences.h>

struct TzEntry { const char *label; int16_t offsetMin; };

// Whole-hour zones plus the handful of common half/quarter-hour ones.
// Ordered west to east; index 12 (UTC+0) is the default.
static const TzEntry TZS[] = {
  { "UTC-12",              -12 * 60 },
  { "UTC-11",              -11 * 60 },
  { "UTC-10 (Hawaii)",     -10 * 60 },
  { "UTC-9 (Alaska)",       -9 * 60 },
  { "UTC-8 (US Pacific)",   -8 * 60 },
  { "UTC-7 (US Mountain)",  -7 * 60 },
  { "UTC-6 (US Central)",   -6 * 60 },
  { "UTC-5 (US Eastern)",   -5 * 60 },
  { "UTC-4 (Atlantic)",     -4 * 60 },
  { "UTC-3:30 (Newfound.)", -3 * 60 - 30 },
  { "UTC-3 (Brazil)",       -3 * 60 },
  { "UTC-1 (Azores)",       -1 * 60 },
  { "UTC+0 (UTC/London)",        0 },
  { "UTC+1 (Central EU)",    1 * 60 },
  { "UTC+2 (East. EU)",      2 * 60 },
  { "UTC+3 (Moscow)",        3 * 60 },
  { "UTC+3:30 (Iran)",       3 * 60 + 30 },
  { "UTC+4 (Gulf)",          4 * 60 },
  { "UTC+5 (Pakistan)",      5 * 60 },
  { "UTC+5:30 (India)",      5 * 60 + 30 },
  { "UTC+5:45 (Nepal)",      5 * 60 + 45 },
  { "UTC+6 (Bangladesh)",    6 * 60 },
  { "UTC+7 (Indochina)",     7 * 60 },
  { "UTC+8 (China/SGP)",     8 * 60 },
  { "UTC+9 (Japan/Korea)",   9 * 60 },
  { "UTC+9:30 (C. Aust.)",   9 * 60 + 30 },
  { "UTC+10 (E. Aust.)",    10 * 60 },
  { "UTC+12 (NZ)",          12 * 60 },
};
static const int TZ_N = sizeof(TZS) / sizeof(TZS[0]);
static const int TZ_DEFAULT = 12;   // UTC+0

static int s_idx = TZ_DEFAULT;

void tzLoad() {
  Preferences p;
  p.begin("touchcal", true);
  s_idx = p.getInt("tz", TZ_DEFAULT);
  p.end();
  if (s_idx < 0 || s_idx >= TZ_N) s_idx = TZ_DEFAULT;
}

void tzSetIndex(int idx) {
  if (idx < 0 || idx >= TZ_N) return;
  s_idx = idx;
  Preferences p;
  p.begin("touchcal", false);
  p.putInt("tz", idx);
  p.end();
}

int         tzCount()             { return TZ_N; }
const char *tzLabel(int idx)      { return (idx >= 0 && idx < TZ_N) ? TZS[idx].label : "?"; }
int         tzGetIndex()          { return s_idx; }
int         tzOffsetMinutes()     { return TZS[s_idx].offsetMin; }
