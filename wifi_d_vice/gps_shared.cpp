// Shared GPS reader + GPS-driven time sync. See gps_shared.h for why this
// is one owner of the UART instead of every screen opening its own.
#include "gps_shared.h"
#include <HardwareSerial.h>
#include "pins.h"
#include "devtime.h"

static HardwareSerial gpsSerial(1);
static TinyGPSPlus gps;

TinyGPSPlus &gpsShared() { return gps; }

void gpsSharedBegin() {
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, -1);
}

// Sync cadence: every 15s until the first fix-derived time lands, then
// once an hour after that to correct local-oscillator drift. Tracked here
// (not off devTimeSynced()) because SNTP or Meshtastic may have already
// set the clock by the time GPS gets its first fix -- GPS still wants to
// run on its OWN 15s/1hr schedule rather than never trying.
static bool gpsHasSyncedOnce = false;
static bool everAttempted = false;
static uint32_t lastSyncAttempt = 0;
static const uint32_t SYNC_RETRY_MS  = 15UL * 1000UL;
static const uint32_t SYNC_RESYNC_MS = 60UL * 60UL * 1000UL;
// A fix TinyGPSPlus is still holding from minutes ago (isValid() never
// expires on its own) doesn't count as fresh for a resync -- age() is
// milliseconds since that field last actually updated from a sentence.
static const uint32_t FRESH_MS = 2500;

static void tryGpsSync() {
  uint32_t interval = gpsHasSyncedOnce ? SYNC_RESYNC_MS : SYNC_RETRY_MS;
  uint32_t now = millis();
  if (everAttempted && now - lastSyncAttempt < interval) return;
  everAttempted = true;
  lastSyncAttempt = now;

  if (!gps.date.isValid() || !gps.time.isValid()) return;        // no fix yet -- retry next interval
  if (gps.date.age() > FRESH_MS || gps.time.age() > FRESH_MS) return;  // stale -- GPS dropped out

  struct tm t = {};
  t.tm_year = gps.date.year() - 1900;
  t.tm_mon  = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour();
  t.tm_min  = gps.time.minute();
  t.tm_sec  = gps.time.second();
  // NMEA date/time is UTC and the device clock is kept in UTC (see
  // devtime.h) -- mktime() here is exactly the same "tm is really UTC"
  // trick devTimeSetFromCTS() already uses, not a timezone conversion.
  time_t epoch = mktime(&t);
  if (epoch <= 0) return;
  devTimeSyncFromGps((uint32_t)epoch);
  gpsHasSyncedOnce = true;
}

void gpsSharedLoop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  tryGpsSync();
}

bool gpsGetLastFix(int &year, int &month, int &day, int &hour, int &minute) {
  if (!gps.date.isValid() || !gps.time.isValid()) return false;
  year = gps.date.year(); month = gps.date.month(); day = gps.date.day();
  hour = gps.time.hour(); minute = gps.time.minute();
  return true;
}
