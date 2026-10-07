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
static Btn toggleBtn;
static bool scanPending = false;
// Track logged BSSIDs. The green LED blips only on new contacts.
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
static void resetGpsFields();   // defined below

void gpsEnter() {
  uiDrawTopBar("Wardrive");
  // uiShowLoading() clears the content area. Draw the button after the check to prevent erasure.
  if (!sdOk) {
    uiShowLoading("Checking SD card...");
    sdBusBegin();
    sdOk = SD.begin(SD_CS, sdSPI);
  }
  // The top bar shows only the back button and title. Place screen buttons in the action row below.
  Btn row[1] = {{0, 0, 0, 0, "start / stop logging"}};
  uiDrawActionRow(row, 1);
  toggleBtn = row[0];
  uiClearBelow(UI_CONTENT_Y);   // wipe whatever this screen's content area last showed (a previous visit's fields, or another screen if this is the first SD check) so draw()'s per-field diffing starts clean
  resetGpsFields();
  logging = false;
  rowsLogged = 0;
  seenN = 0;
  greenOffAt = 0;
  scanPending = false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  lastTick = millis();
  draw();
}

// Update only changed fields. This avoids clearing and reprinting the whole block every tick.
static char prevFix[40] = "", prevLatLon[48] = "", prevSd[24] = "", prevEng[48] = "",
            prevLogging[24] = "", prevRows[48] = "", prevFail[64] = "";
static const int GPS_FIELD_H = 14;

static void resetGpsFields() {
  prevFix[0] = prevLatLon[0] = prevSd[0] = prevEng[0] = prevLogging[0] = prevRows[0] = prevFail[0] = '\0';
}

static void draw() {
  tft.setTextSize(1);
  int y = UI_CONTENT_Y + 4;
  int fw = tft.width() - 8;

  const char *fixState = gpsShared().location.isValid() ? "yes"
                       : (lastFixMs && millis() - lastFixMs < 15000) ? "yes"
                       : lastFixMs ? "stale" : "none";
  uiDrawFieldIfChanged(4, y, fw, GPS_FIELD_H, ILI9341_WHITE, 1, prevFix, sizeof(prevFix),
                        "Fix: %s  sats: %lu", fixState,
                        gpsShared().satellites.isValid() ? (unsigned long)gpsShared().satellites.value() : 0UL);
  y += 16;

  if (gpsShared().location.isValid())
    uiDrawFieldIfChanged(4, y, fw, GPS_FIELD_H, ILI9341_WHITE, 1, prevLatLon, sizeof(prevLatLon),
                          "lat %.6f  lon %.6f", gpsShared().location.lat(), gpsShared().location.lng());
  else
    uiDrawFieldIfChanged(4, y, fw, GPS_FIELD_H, ILI9341_WHITE, 1, prevLatLon, sizeof(prevLatLon), "lat --  lon --");
  y += 16;

  uiDrawFieldIfChanged(4, y, fw, GPS_FIELD_H, ILI9341_WHITE, 1, prevSd, sizeof(prevSd),
                        "SD: %s", sdOk ? "ok" : "not found");
  y += 16;

  uiDrawFieldIfChanged(4, y, fw, GPS_FIELD_H, engagementIsArmed() ? ILI9341_GREEN : ILI9341_YELLOW, 1,
                        prevEng, sizeof(prevEng),
                        "engagement: %s", engagementIsArmed() ? "armed (encrypting)" : "not armed (plaintext!)");
  y += 16;

  uiDrawFieldIfChanged(4, y, fw, GPS_FIELD_H, ILI9341_WHITE, 1, prevLogging, sizeof(prevLogging),
                        "logging: %s", logging ? "ON" : "off");
  y += 16;

  uiDrawFieldIfChanged(4, y, fw, GPS_FIELD_H, ILI9341_WHITE, 1, prevRows, sizeof(prevRows),
                        "%lu rows -> %s", (unsigned long)rowsLogged, logging ? "SD" : "-");
  y += 16;

  if (wlogEncFails()) {
    uiDrawFieldIfChanged(4, y, fw, GPS_FIELD_H, ILI9341_RED, 1, prevFail, sizeof(prevFail),
                          "encrypt failures: %lu (rows dropped)", (unsigned long)wlogEncFails());
  } else if (prevFail[0]) {
    uiClearRect(4, y, fw, GPS_FIELD_H);
    prevFail[0] = '\0';
  }
}

void gpsLoop() {
  // gpsSharedLoop() handles byte draining and NMEA decoding. This function only reads the result.
  if (greenOffAt && millis() >= greenOffAt) { ledGreen(false); greenOffAt = 0; }

  // Poll the pending async scan every tick. scanComplete() reports status without blocking touch input.
  if (scanPending) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;   // not done yet -- check again next tick
    if (n < 0) n = 0;                     // WIFI_SCAN_FAILED or similar
    scanPending = false;
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
    lastTick = millis();   // pace the NEXT scan from completion, not from when this one started
    draw();
    return;
  }

  uint32_t interval = logging ? 5000 : 1000;
  if (millis() - lastTick < interval) return;
  lastTick = millis();

  // Cache the last valid fix. Log access point data regardless of fix status. Mark the fix column blank or stale.
  if (gpsShared().location.isValid()) {
    lastLat = gpsShared().location.lat();
    lastLon = gpsShared().location.lng();
    lastFixMs = millis();
  }

  if (logging) {
    WiFi.scanNetworks(true, true);   // async -- picked up above once scanComplete() says it's done
    scanPending = true;
    return;
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
