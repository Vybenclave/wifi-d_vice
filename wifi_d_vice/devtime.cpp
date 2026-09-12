#include "devtime.h"
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>
#if __has_include(<esp_sntp.h>)
#include <esp_sntp.h>
#define HAVE_ESP_SNTP 1
#endif

static bool synced = false;
static bool sntpStarted = false;
static const char *syncSource = "none";   // last source that set the clock

bool devTimeSynced() { return synced; }

void devTimeBeginNet() {
  if (synced || sntpStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;
  sntpStarted = true;

#ifdef HAVE_ESP_SNTP
  // Prefer an NTP server advertised by the DHCP server (option 42), if the
  // network sends one and the lwIP build captured it.
  esp_sntp_servermode_dhcp(1);
#endif

  // Explicit fallbacks in priority order: the gateway (home routers commonly
  // answer NTP), then NIST, then the public pool. Device time is UTC
  // (offset 0) -- the rest of the firmware uses gmtime_r().
  static String gw;
  gw = WiFi.gatewayIP().toString();
  if ((uint32_t)WiFi.gatewayIP() != 0)
    configTime(0, 0, gw.c_str(), "time.nist.gov", "pool.ntp.org");
  else
    configTime(0, 0, "time.nist.gov", "pool.ntp.org");
}

void devTimePoll() {
  if (synced || !sntpStarted) return;
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  if (t.tm_year + 1900 >= 2024) { synced = true; syncSource = "sntp"; }   // a real reply has landed
}

// Lower-priority, WiFi-independent bootstrap (see devtime.h). Never disturbs
// an existing sync; never touches the SNTP state, so devTimeBeginNet() still
// works if WiFi comes up first.
void devTimeSetEpoch(uint32_t epoch, const char *source) {
  if (epoch < 1704067200UL || epoch > 4102444800UL) return;  // 2024-01-01 .. 2100
  if (synced) return;
  struct timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  synced = true;
  syncSource = (source && *source) ? source : "ext";
}

void devTimeSyncFromGps(uint32_t epoch) {
  if (epoch < 1704067200UL || epoch > 4102444800UL) return;  // 2024-01-01 .. 2100
  struct timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  synced = true;
  syncSource = "gps";
}

void devTimeSetFromCTS(uint16_t year, uint8_t month, uint8_t day, uint8_t hh, uint8_t mm, uint8_t ss) {
  struct tm tmv = {};
  tmv.tm_year = year - 1900;
  tmv.tm_mon = month - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hh;
  tmv.tm_min = mm;
  tmv.tm_sec = ss;
  time_t epoch = mktime(&tmv);
  struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
  settimeofday(&tv, nullptr);
  synced = true;
  syncSource = "cts";
}

uint32_t devTimeNow() { return synced ? (uint32_t)time(nullptr) : 0; }

String devTimeNowString() {
  if (!synced) return "unsynced";
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

String devDateString() {
  if (!synced) return "unsynced";
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(buf);
}
