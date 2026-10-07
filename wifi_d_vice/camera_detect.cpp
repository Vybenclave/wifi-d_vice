// This module scans only. It never associates or transmits. WiFi scanning coexists with the station link.
#include <WiFi.h>
#include "ui.h"
#include "known_signatures.h"
#include "accent.h"
#include "devtime.h"
#include "flagfinding.h"
#include "demomode.h"
#include "power.h"
#include <esp_random.h>

struct Hit { char ssid[20]; uint8_t bssid[6]; const char *vendor; int rssi; uint8_t ch; };
static const int MAX_HITS = 10;
static Hit hits[MAX_HITS];
static int hitCount = 0, prevHits = 0;
static uint32_t lastScan = 0;
static bool scanning = false;

enum Sub { LIST, LOCATE };
static Sub sub = LIST;
static int locIdx = -1;
static Btn backRow, flagBtn;
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

static void drawList();

// Demo mode fabricates a hit list. It uses the same Hit fields as the real scanner. The UI code remains unchanged.
static void spawnDemoHits() {
  static const char *kSsids[] = {"Nestcam_Living", "TAPO_C210", "Hikvision-DVR"};
  hitCount = 1 + (int)(esp_random() % 3);   // 1..3 fake hits
  if (hitCount > MAX_HITS) hitCount = MAX_HITS;
  for (int i = 0; i < hitCount; i++) {
    Hit &h = hits[i];
    snprintf(h.ssid, sizeof(h.ssid), "%s", demoRandPick(kSsids, 3));
    demoRandMac(h.bssid);
    h.vendor = "Demo";
    h.rssi = demoRandRssi();
    h.ch = (uint8_t)(1 + esp_random() % 11);
  }
  scanning = false;
  if (hitCount > prevHits) alertDetected();
  prevHits = hitCount;
  drawList();
  lastScan = millis();
}

static void startScan() {
  if (demoModeEnabled()) { spawnDemoHits(); return; }
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true, true);   // async
  scanning = true;
  lastScan = millis();
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
    tft.setTextColor(accentLabel());
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
  Btn r[2] = {{0, 0, 0, 0, "< list"}, {0, 0, 0, 0, "Flag"}};
  uiDrawActionRow(r, 2);
  backRow = r[0];
  flagBtn = r[1];
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_RED);
  tft.setCursor(6, UI_CONTENT_Y + 4);
  tft.printf("%s", hits[locIdx].vendor);
  tft.setTextSize(1);
  tft.setTextColor(accentLabel());
  tft.setCursor(6, UI_CONTENT_Y + 24);
  tft.printf("%02X%02X%02X  ch%d  %-.20s", hits[locIdx].bssid[3], hits[locIdx].bssid[4],
             hits[locIdx].bssid[5], hits[locIdx].ch, hits[locIdx].ssid);
  tft.drawRect(4, UI_CONTENT_Y + 75, tft.width() - 8, 26, ILI9341_WHITE);
  lastShownRssi = -999;
}

static void updateLocate() {
  if (millis() - lastLoc < 400) return;
  lastLoc = millis();
  int rssi;
  if (demoModeEnabled()) {
    rssi = demoRandRssi();
  } else {
    // A single-channel scan runs fast enough to chirp a useful direction-finding rate.
    int n = WiFi.scanNetworks(false, true, false, 200, hits[locIdx].ch);
    rssi = -127;
    for (int i = 0; i < n; i++) {
      const uint8_t *b = WiFi.BSSID(i);
      if (b && !memcmp(b, hits[locIdx].bssid, 6)) { rssi = WiFi.RSSI(i); break; }
    }
    WiFi.scanDelete();
  }

  beepHold(rssi > -127);
  if (rssi > -127) rangeBeep(rssi);

  uiDrawLocateReading(4, UI_CONTENT_Y + 38, tft.width() - 8,
                       ILI9341_RED, -95, -35, &lastShownRssi, rssi);
}

void cameraEnter() {
  uiDrawTopBar("Camera Detect");
  beepHold(true);
  sub = LIST;
  hitCount = prevHits = 0;
  startScan();
  drawList();
}

void cameraLoop() {
  // This loop keeps the idle clock fresh. It prevents the display timeout from blanking the screen during tracking.
  if (sub == LOCATE) { powerNoteActivity(); updateLocate(); return; }

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
  if (hitCount == 0) ledAlert(false);
  else {
    int best = -127;
    for (int i = 0; i < hitCount; i++) if (hits[i].rssi > best) best = hits[i].rssi;
    ledAlert(true);
    rangeBeep(best);
  }
  if (millis() - lastScan > 4000) startScan();
}

void cameraTouch(const TouchPoint &t) {
  if (sub == LOCATE) {
    if (uiTouchInButton(t, backRow)) {
      beepHold(false);
      sub = LIST;
      drawList();
      uiWaitForRelease();
      return;
    }
    if (uiTouchInButton(t, flagBtn)) {
      uiWaitForRelease();
      flagDetectionShow("camera", UI_SEV_ALERT);   // The locate chrome already shows this screen's detail view.
      uiDrawTopBar("Camera Detect");
      drawLocateChrome();
      return;
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
  ledAlert(false);
  ledGreen(false);
  WiFi.scanDelete();
}
