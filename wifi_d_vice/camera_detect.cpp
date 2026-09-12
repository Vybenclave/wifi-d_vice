// WiFi surveillance-camera detector (Wireless Wizard "Camera Detector").
// Passive: a repeating WiFi.scanNetworks() pass, each beacon's BSSID OUI
// checked against the camera-vendor table in known_signatures.h, plus an
// SSID-substring pass for cameras that name themselves. Tap a hit to
// direction-find it (RSSI bar + range chirp) the same way Tracker Detect
// homes in on a tag.
//
// Scan only -- it never associates or transmits. It does NOT drop an active
// WiFi station link (scanNetworks coexists with STA), unlike the
// promiscuous Recon screens.
#include <WiFi.h>
#include "ui.h"
#include "known_signatures.h"
#include "theme.h"

struct Hit { char ssid[20]; uint8_t bssid[6]; const char *vendor; int rssi; uint8_t ch; };
static const int MAX_HITS = 10;
static Hit hits[MAX_HITS];
static int hitCount = 0, prevHits = 0;
static uint32_t lastScan = 0;
static bool scanning = false;

enum Sub { LIST, LOCATE };
static Sub sub = LIST;
static int locIdx = -1;
static Btn backRow;
static uint32_t lastLoc = 0;
static int lastShownRssi = -999;

static const char *ssidCameraHint(const String &lower) {
  static const char *k[] = { "ipcam", "ip-cam", "-cam", "camera", "cctv", "hikvision",
                             "dahua", "reolink", "ezviz", "wyzecam", "amcrest", "lorex",
                             "tapo_c", "nestcam", "doorbell", "surveil" };
  for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++)
    if (lower.indexOf(k[i]) >= 0) return "SSID";
  return nullptr;
}

static void harvest(int n) {
  hitCount = 0;
  for (int i = 0; i < n && hitCount < MAX_HITS; i++) {
    const uint8_t *b = WiFi.BSSID(i);
    if (!b) continue;
    const char *v = cameraVendorForBssid(b);
    if (!v) {
      String lo = WiFi.SSID(i); lo.toLowerCase();
      v = ssidCameraHint(lo);
    }
    if (!v) continue;
    Hit &h = hits[hitCount++];
    snprintf(h.ssid, sizeof h.ssid, "%s", WiFi.SSID(i).length() ? WiFi.SSID(i).c_str() : "(hidden)");
    memcpy(h.bssid, b, 6);
    h.vendor = v;
    h.rssi = WiFi.RSSI(i);
    h.ch = WiFi.channel(i);
  }
  WiFi.scanDelete();
}

static void drawList() {
  uiClearBelow(29);
  tft.setTextSize(1);
  if (scanning && hitCount == 0) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(4, 40); tft.print("Scanning...");
    return;
  }
  if (hitCount == 0) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(4, 40); tft.print("No camera-vendor APs seen.");
    return;
  }
  int y = 34;
  for (int i = 0; i < hitCount && y + 24 <= tft.height() - UI_STATUSBAR_H; i++) {
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(4, y);
    tft.printf("%-9s %02X%02X%02X ch%-2d %ddBm", hits[i].vendor,
               hits[i].bssid[3], hits[i].bssid[4], hits[i].bssid[5], hits[i].ch, hits[i].rssi);
    tft.setTextColor(thLabel());
    tft.setCursor(12, y + 10);
    tft.printf("%-.24s", hits[i].ssid);
    y += 24;
  }
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(4, tft.height() - UI_STATUSBAR_H - 10);
  tft.print("tap a row to direction-find");
}

static void drawLocateChrome() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn r[1] = {{0, 0, 0, 0, "< list"}};
  uiDrawActionRow(r, 1);
  backRow = r[0];
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_RED);
  tft.setCursor(6, UI_CONTENT_Y + 4);
  tft.printf("%s", hits[locIdx].vendor);
  tft.setTextSize(1);
  tft.setTextColor(thLabel());
  tft.setCursor(6, UI_CONTENT_Y + 24);
  tft.printf("%02X%02X%02X  ch%d  %-.20s", hits[locIdx].bssid[3], hits[locIdx].bssid[4],
             hits[locIdx].bssid[5], hits[locIdx].ch, hits[locIdx].ssid);
  tft.drawRect(4, UI_CONTENT_Y + 74, tft.width() - 8, 24, ILI9341_WHITE);
  lastShownRssi = -999;
}

static void updateLocate() {
  if (millis() - lastLoc < 400) return;
  lastLoc = millis();
  // Single-channel scan -- fast enough to chirp a useful DF rate.
  int n = WiFi.scanNetworks(false, true, false, 200, hits[locIdx].ch);
  int rssi = -127;
  for (int i = 0; i < n; i++) {
    const uint8_t *b = WiFi.BSSID(i);
    if (b && !memcmp(b, hits[locIdx].bssid, 6)) { rssi = WiFi.RSSI(i); break; }
  }
  WiFi.scanDelete();

  beepHold(rssi > -127);
  if (rssi > -127) rangeBeep(rssi);
  if (rssi == lastShownRssi) return;
  lastShownRssi = rssi;

  tft.fillRect(4, UI_CONTENT_Y + 40, tft.width() - 8, 32, ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(rssi > -127 ? ILI9341_RED : ILI9341_DARKGREY);
  tft.setCursor(6, UI_CONTENT_Y + 40);
  if (rssi > -127) tft.printf("%4d dBm", rssi); else tft.print(" -- lost");
  int barW = rssi > -127 ? map(constrain(rssi, -95, -35), -95, -35, 0, tft.width() - 10) : 0;
  tft.fillRect(5, UI_CONTENT_Y + 75, tft.width() - 10, 22, ILI9341_BLACK);
  tft.fillRect(5, UI_CONTENT_Y + 75, barW, 22,
               rssi > -55 ? ILI9341_GREEN : (rssi > -75 ? ILI9341_YELLOW : ILI9341_RED));
}

void cameraEnter() {
  uiDrawTopBar("Camera Detect");
  beepHold(true);
  sub = LIST;
  hitCount = prevHits = 0;
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true, true);   // async
  scanning = true;
  lastScan = millis();
  drawList();
}

void cameraLoop() {
  if (sub == LOCATE) { updateLocate(); return; }

  if (scanning) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    if (n >= 0) harvest(n); else WiFi.scanDelete();
    scanning = false;
    if (hitCount > prevHits) alertDetected();
    prevHits = hitCount;
    drawList();
    lastScan = millis();
    return;
  }
  if (hitCount == 0) ledSet(false);
  else {
    int best = -127;
    for (int i = 0; i < hitCount; i++) if (hits[i].rssi > best) best = hits[i].rssi;
    ledSet(true);
    rangeBeep(best);
  }
  if (millis() - lastScan > 4000) { WiFi.scanNetworks(true, true); scanning = true; }
}

void cameraTouch(const TouchPoint &t) {
  if (sub == LOCATE) {
    if (uiTouchInButton(t, backRow)) {
      beepHold(false);
      sub = LIST;
      drawList();
      uiWaitForRelease();
    }
    return;
  }
  if (!t.isNewPress) return;
  int y = 34;
  for (int i = 0; i < hitCount && y + 24 <= tft.height() - UI_STATUSBAR_H; i++) {
    if (t.y >= y - 2 && t.y < y + 22) {
      locIdx = i;
      sub = LOCATE;
      lastLoc = 0;
      drawLocateChrome();
      uiWaitForRelease();
      return;
    }
    y += 24;
  }
}

void cameraExit() {
  beepHold(false);
  ledSet(false);
  ledGreen(false);
  WiFi.scanDelete();
}
