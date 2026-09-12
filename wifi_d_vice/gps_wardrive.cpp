// GPS-tagged WiFi wardriving logger. The GPS fix comes from the shared
// background reader (gps_shared.h) now -- it's been running since boot for
// the GPS time sync, so this screen just reads its live TinyGPSPlus state
// instead of opening a second reader on the same UART. Logs to SD (see
// sd_bus.h).
//
// The SD log format (plaintext engagement header, encrypted= marker, then
// encrypt-when-armed rows) is shared with the other scan screens and lives
// in wlog.{cpp,h} now -- this screen just formats rows and hands them over.
#include <SD.h>
#include <WiFi.h>
#include "ui.h"
#include "pins.h"
#include "sd_bus.h"
#include "gps_shared.h"
#include "devtime.h"
#include "engagement.h"
#include "wlog.h"

static bool sdOk = false;
static bool logging = false;
static uint32_t lastTick = 0;
static uint32_t rowsLogged = 0;
static double   lastLat = 0, lastLon = 0;   // last good fix, cached for no/stale-fix logging
static uint32_t lastFixMs = 0;              // 0 = never had a fix this session
static Btn toggleBtn;   // positioned in gpsEnter(), once tft is sized/rotated

// Session set of BSSIDs already logged, so the green LED blips only on a
// genuinely new contact. Fixed cap -- oldest entries just stop deduping.
static uint64_t seenBssid[256];
static int      seenN = 0;
static uint32_t greenOffAt = 0;

static bool bssidIsNew(const uint8_t *b) {
  uint64_t k = 0;
  for (int i = 0; i < 6; i++) k = (k << 8) | b[i];
  for (int i = 0; i < seenN; i++) if (seenBssid[i] == k) return false;
  if (seenN < (int)(sizeof(seenBssid) / sizeof(seenBssid[0]))) seenBssid[seenN++] = k;
  return true;
}

static void draw();   // defined below

void gpsEnter() {
  uiDrawTopBar("Wardrive");
  // GPS UART is opened once at boot by gpsSharedBegin() (it's also the GPS
  // time-sync source now, so it has to run whether or not this screen is
  // ever visited) -- nothing to open here.
  // SD check first: uiShowLoading() clears the content area (incl. the
  // action row), so drawing the button before this wiped it on the first
  // visit and nothing redrew it. Do the check, THEN the button, THEN draw().
  if (!sdOk) {
    uiShowLoading("Checking SD card...");
    sdBusBegin();
    sdOk = SD.begin(SD_CS, sdSPI);
  }
  // Per the UI rule (see ui.h): the top-bar row is back+title only; a
  // per-screen button goes in the action row below it.
  Btn row[1] = {{0, 0, 0, 0, "start / stop logging"}};
  uiDrawActionRow(row, 1);
  toggleBtn = row[0];
  logging = false;
  rowsLogged = 0;
  seenN = 0;
  greenOffAt = 0;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  lastTick = millis();
  draw();
}

static void draw() {
  uiClearBelow(UI_CONTENT_Y);
  tft.setTextSize(1);
  int y = UI_CONTENT_Y + 4;
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(4, y);
  const char *fixState = gpsShared().location.isValid() ? "yes"
                       : (lastFixMs && millis() - lastFixMs < 15000) ? "yes"
                       : lastFixMs ? "stale" : "none";
  tft.printf("Fix: %s  sats: %lu", fixState,
             gpsShared().satellites.isValid() ? (unsigned long)gpsShared().satellites.value() : 0UL);
  y += 16;
  tft.setCursor(4, y);
  if (gpsShared().location.isValid()) tft.printf("lat %.6f  lon %.6f", gpsShared().location.lat(), gpsShared().location.lng());
  else tft.print("lat --  lon --");
  y += 16;
  tft.setCursor(4, y);
  tft.printf("SD: %s", sdOk ? "ok" : "not found");
  y += 16;
  tft.setCursor(4, y);
  tft.setTextColor(engagementIsArmed() ? ILI9341_GREEN : ILI9341_YELLOW);
  tft.printf("engagement: %s", engagementIsArmed() ? "armed (encrypting)" : "not armed (plaintext!)");
  tft.setTextColor(ILI9341_WHITE);
  y += 16;
  tft.setCursor(4, y);
  tft.printf("logging: %s", logging ? "ON" : "off");
  y += 16;
  tft.setCursor(4, y);
  tft.printf("%lu rows -> %s", (unsigned long)rowsLogged, logging ? "SD" : "-");
  if (wlogEncFails()) {
    y += 16;
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(4, y);
    tft.printf("encrypt failures: %lu (rows dropped)", (unsigned long)wlogEncFails());
  }
}

// gpsGetLastFix() itself now lives in gps_shared.cpp -- it reads the same
// background parser this screen does, so there's no reason to keep a
// second copy here.

void gpsLoop() {
  // Byte draining + NMEA decode happens in gpsSharedLoop() (called from the
  // main loop() regardless of the active screen); this just reads the
  // result.
  if (greenOffAt && millis() >= greenOffAt) { ledGreen(false); greenOffAt = 0; }

  uint32_t interval = logging ? 5000 : 1000;
  if (millis() - lastTick < interval) return;
  lastTick = millis();

  // Cache the last good fix. A "static location" capture only needs one
  // fix (or none) -- keep logging AP data regardless, with a fix column
  // that's blank when we've never had one and flagged stale when it's old.
  if (gpsShared().location.isValid()) {
    lastLat = gpsShared().location.lat();
    lastLon = gpsShared().location.lng();
    lastFixMs = millis();
  }

  if (logging) {
    int n = WiFi.scanNetworks(false, true);
    bool sawNew = false;
    bool haveFix   = lastFixMs != 0;
    bool freshFix  = haveFix && (millis() - lastFixMs < 15000);
    for (int i = 0; i < n; i++) {
      char coords[40];
      if (!haveFix)       snprintf(coords, sizeof(coords), ",,nofix");
      else if (freshFix)  snprintf(coords, sizeof(coords), "%.6f,%.6f,", lastLat, lastLon);
      else                snprintf(coords, sizeof(coords), "%.6f,%.6f,stale", lastLat, lastLon);
      char line[192];
      snprintf(line, sizeof(line), "%s,%s,%s,%s,%d,%d",
               devTimeNowString().c_str(), coords,
               WiFi.SSID(i).c_str(), WiFi.BSSIDstr(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
      wlogRow(line);
      rowsLogged++;
      if (bssidIsNew(WiFi.BSSID(i))) sawNew = true;
    }
    if (sawNew) { ledGreen(true); greenOffAt = millis() + 60; }   // blip on a new contact
    wlogFlush();   // once per scan batch, not per row
    WiFi.scanDelete();
  }
  draw();
}

void gpsTouch(const TouchPoint &t) {
  if (!uiTouchInButton(t, toggleBtn)) return;
  if (!logging) {
    if (wlogOpen("wardrive", "utc,lat,lon,fix,ssid,bssid,rssi,channel")) {
      logging = true;
      rowsLogged = 0;
    }
  } else {
    logging = false;
    wlogClose();
  }
  draw();
}

void gpsExit() {
  wlogClose();
  logging = false;
  ledGreen(false);
  greenOffAt = 0;
}
