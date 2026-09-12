#include "tz.h"
#include <Preferences.h>
#include <time.h>
#include "devtime.h"

// DST rule shapes actually in use worldwide -- see tz.h for why this is
// hardcoded Gregorian-calendar math instead of an IANA database. NONE
// covers most of the world (most zones don't observe DST at all).
enum class DstRule : uint8_t {
  NONE,   // never
  US_CA,  // US/Canada: 2nd Sun Mar -> 1st Sun Nov (since 2007)
  EU,     // EU: last Sun Mar -> last Sun Oct
  AU,     // Australia: 1st Sun Oct -> 1st Sun Apr (wraps the new year)
  NZ,     // New Zealand: last Sun Sep -> 1st Sun Apr (wraps the new year)
};

struct TzEntry { const char *label; int16_t offsetMin; DstRule dst; };

// Whole-hour zones plus the handful of common half/quarter-hour ones.
// Ordered west to east; index 12 (UTC+0) is the default. DST rule picks
// the majority/typical case for zones some but not all of whose territory
// observes it (e.g. Arizona sits in DstRule::US_CA's UTC-7 slot but doesn't
// actually observe DST) -- a deliberate simplification, not a bug, same
// spirit as the fixed-offset list itself.
static const TzEntry TZS[] = {
  { "UTC-12",              -12 * 60,        DstRule::NONE },
  { "UTC-11",              -11 * 60,        DstRule::NONE },
  { "UTC-10 (Hawaii)",     -10 * 60,        DstRule::NONE },
  { "UTC-9 (Alaska)",       -9 * 60,        DstRule::US_CA },
  { "UTC-8 (US Pacific)",   -8 * 60,        DstRule::US_CA },
  { "UTC-7 (US Mountain)",  -7 * 60,        DstRule::US_CA },
  { "UTC-6 (US Central)",   -6 * 60,        DstRule::US_CA },
  { "UTC-5 (US Eastern)",   -5 * 60,        DstRule::US_CA },
  { "UTC-4 (Atlantic)",     -4 * 60,        DstRule::US_CA },
  { "UTC-3:30 (Newfound.)", -3 * 60 - 30,   DstRule::US_CA },
  { "UTC-3 (Brazil)",       -3 * 60,        DstRule::NONE },   // Brazil dropped DST in 2019
  { "UTC-1 (Azores)",       -1 * 60,        DstRule::EU },
  { "UTC+0 (UTC/London)",        0,         DstRule::EU },
  { "UTC+1 (Central EU)",    1 * 60,        DstRule::EU },
  { "UTC+2 (East. EU)",      2 * 60,        DstRule::EU },
  { "UTC+3 (Moscow)",        3 * 60,        DstRule::NONE },   // Russia dropped DST in 2014
  { "UTC+3:30 (Iran)",       3 * 60 + 30,   DstRule::NONE },   // Iran dropped DST in 2022
  { "UTC+4 (Gulf)",          4 * 60,        DstRule::NONE },
  { "UTC+5 (Pakistan)",      5 * 60,        DstRule::NONE },
  { "UTC+5:30 (India)",      5 * 60 + 30,   DstRule::NONE },
  { "UTC+5:45 (Nepal)",      5 * 60 + 45,   DstRule::NONE },
  { "UTC+6 (Bangladesh)",    6 * 60,        DstRule::NONE },
  { "UTC+7 (Indochina)",     7 * 60,        DstRule::NONE },
  { "UTC+8 (China/SGP)",     8 * 60,        DstRule::NONE },
  { "UTC+9 (Japan/Korea)",   9 * 60,        DstRule::NONE },
  { "UTC+9:30 (C. Aust.)",   9 * 60 + 30,   DstRule::AU },
  { "UTC+10 (E. Aust.)",    10 * 60,        DstRule::AU },
  { "UTC+12 (NZ)",          12 * 60,        DstRule::NZ },
};
static const int TZ_N = sizeof(TZS) / sizeof(TZS[0]);
static const int TZ_DEFAULT = 12;   // UTC+0

static int  s_idx     = TZ_DEFAULT;
static bool s_24h     = true;
static bool s_autoDst = true;

// ---- Gregorian-calendar helpers for the DST rules above -----------------

// Sakamoto's algorithm -- 0=Sunday..6=Saturday, no library/lookup-table
// dependency beyond the 12-entry month adjustment it's built on.
static int dow(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int daysInMonth(int y, int m) {
  static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && isLeap(y)) ? 29 : dm[m - 1];
}

static int nthSunday(int y, int m, int n) {
  int d = 1;
  while (dow(y, m, d) != 0) d++;
  return d + 7 * (n - 1);
}

static int lastSunday(int y, int m) {
  int d = daysInMonth(y, m);
  while (dow(y, m, d) != 0) d--;
  return d;
}

// Transition day-of-month itself counts as already in DST (a "spring
// forward at 2am local" is close enough to "midnight UTC" for a status
// clock -- see tz.h), and the end day counts as no longer in DST.
static bool isDstActive(DstRule rule, int year, int month, int day) {
  switch (rule) {
    case DstRule::US_CA: {
      if (month < 3 || month > 11) return false;
      if (month > 3 && month < 11) return true;
      if (month == 3) return day >= nthSunday(year, 3, 2);
      return day < nthSunday(year, 11, 1);            // month == 11
    }
    case DstRule::EU: {
      if (month < 3 || month > 10) return false;
      if (month > 3 && month < 10) return true;
      if (month == 3) return day >= lastSunday(year, 3);
      return day < lastSunday(year, 10);               // month == 10
    }
    case DstRule::AU: {   // wraps the new year: Oct..Mar is DST, May..Sep is not
      if (month > 10 || month < 4) return true;
      if (month == 10) return day >= nthSunday(year, 10, 1);
      if (month == 4)  return day < nthSunday(year, 4, 1);
      return false;                                     // May..Sep
    }
    case DstRule::NZ: {    // wraps the new year: Sep(end)..Mar is DST
      if (month > 9 || month < 4) return true;
      if (month == 9) return day >= lastSunday(year, 9);
      if (month == 4) return day < nthSunday(year, 4, 1);
      return false;                                      // May..Aug
    }
    default: return false;   // DstRule::NONE
  }
}

// ---- public API -----------------------------------------------------------

void tzLoad() {
  Preferences p;
  p.begin("touchcal", true);
  s_idx     = p.getInt("tz", TZ_DEFAULT);
  s_24h     = p.getBool("tz24h", true);
  s_autoDst = p.getBool("tzdst", true);
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

void tzSet24h(bool on) {
  s_24h = on;
  Preferences p;
  p.begin("touchcal", false);
  p.putBool("tz24h", on);
  p.end();
}

void tzSetAutoDst(bool on) {
  s_autoDst = on;
  Preferences p;
  p.begin("touchcal", false);
  p.putBool("tzdst", on);
  p.end();
}

int         tzCount()             { return TZ_N; }
const char *tzLabel(int idx)      { return (idx >= 0 && idx < TZ_N) ? TZS[idx].label : "?"; }
int         tzGetIndex()          { return s_idx; }
bool        tzUse24h()            { return s_24h; }
bool        tzAutoDst()           { return s_autoDst; }
bool        tzZoneHasDst(int idx) { return (idx >= 0 && idx < TZ_N) && TZS[idx].dst != DstRule::NONE; }

bool tzDstActiveFor(int idx) {
  if (idx < 0 || idx >= TZ_N || TZS[idx].dst == DstRule::NONE) return false;
  if (!devTimeSynced()) return false;   // no real UTC date to evaluate against yet
  time_t now = devTimeNow();
  struct tm t;
  gmtime_r(&now, &t);
  return isDstActive(TZS[idx].dst, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

int tzOffsetMinutesFor(int idx) {
  if (idx < 0 || idx >= TZ_N) idx = TZ_DEFAULT;
  int base = TZS[idx].offsetMin;
  return (s_autoDst && tzDstActiveFor(idx)) ? base + 60 : base;
}

int tzOffsetMinutes() { return tzOffsetMinutesFor(s_idx); }
