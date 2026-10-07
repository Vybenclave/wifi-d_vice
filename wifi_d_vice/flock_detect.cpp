#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include "ui.h"
#include "known_signatures.h"
#include "debuglog.h"
#include "devtime.h"
#include "flagfinding.h"
#include "demomode.h"

static uint32_t lastScan = 0;
static const int MAX_HITS = 8;
struct Hit { String kind; String name; int rssi; uint8_t mac[6]; bool hasMac; };
static Hit hits[MAX_HITS];
static int hitCount = 0;
static BLEScan *pBLEScan = nullptr;
static bool wifiOn = true, bleOn = true;
static Btn wifiTab, bleTab;

enum Phase { P_IDLE, P_WIFI, P_BLE };
static Phase phase = P_IDLE;
static volatile bool bleDone = false;

static void draw();
static void spawnDemoHit();

static void addHit(const char *kind, const String &name, int rssi, const uint8_t *mac) {
  if (hitCount >= MAX_HITS) return;
  Hit &h = hits[hitCount++];
  h.kind = kind; h.name = name; h.rssi = rssi;
  h.hasMac = (mac != nullptr);
  if (h.hasMac) memcpy(h.mac, mac, 6);
}

static void harvestWifi(int n) {
  for (int i = 0; i < n && hitCount < MAX_HITS; i++) {
    String s = WiFi.SSID(i);
    String lower = s; lower.toLowerCase();
    if (matchesAnyPattern(lower, kFlockNamePatterns, kFlockNamePatternCount))
      addHit("WiFi", s, WiFi.RSSI(i), WiFi.BSSID(i));
  }
  WiFi.scanDelete();
}

// This function runs on the BLE host task.
static void bleScanDone(BLEScanResults r) {
  for (int i = 0; i < (int)r.getCount() && hitCount < MAX_HITS; i++) {
    BLEAdvertisedDevice d = r.getDevice(i);
    if (!d.haveName()) continue;
    String name = String(d.getName().c_str());
    String lower = name; lower.toLowerCase();
    if (matchesAnyPattern(lower, kFlockNamePatterns, kFlockNamePatternCount))
      addHit("BLE", name, d.getRSSI(), d.getAddress().getNative());
  }
  bleDone = true;
}

static void startBle() {
  if (!pBLEScan) {
    DLOG("flock", "BLE init, free heap %u, largest block %u",
         (unsigned)ESP.getFreeHeap(),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setActiveScan(true);
  }
  bleDone = false;
  phase = P_BLE;
  pBLEScan->start(2, bleScanDone, false);
}

static void startScan() {
  if (demoModeEnabled()) { spawnDemoHit(); return; }
  hitCount = 0;
  if (wifiOn)     { WiFi.mode(WIFI_STA); WiFi.scanNetworks(true, true); phase = P_WIFI; }
  else if (bleOn) { startBle(); }
  else            { phase = P_IDLE; draw(); lastScan = millis(); }
}

static const int TABS_Y = 29, TABS_H = 26, CONTENT_Y = TABS_Y + TABS_H + 1;

static void drawTabs() {
  uiClearRect(0, TABS_Y, tft.width(), TABS_H);
  wifiTab = {0, TABS_Y, tft.width() / 2, TABS_H, "WiFi"};
  bleTab = {tft.width() / 2, TABS_Y, tft.width() - tft.width() / 2, TABS_H, "BLE"};
  uiDrawMenuButton(wifiTab);
  uiDrawMenuButton(bleTab);
  tft.fillRect(wifiTab.x + 2, wifiTab.y + TABS_H - 5, wifiTab.w - 4, 3, wifiOn ? ILI9341_GREEN : ILI9341_RED);
  tft.fillRect(bleTab.x + 2, bleTab.y + TABS_H - 5, bleTab.w - 4, 3, bleOn ? ILI9341_GREEN : ILI9341_RED);
}

static char prevHitRow[MAX_HITS][UI_LIST_SIG_LEN];
static char prevMsg[48] = "";
static const int ROW_H = 16;

static void draw() {
  tft.setTextSize(1);
  const char *msg = nullptr;
  uint16_t msgCol = ILI9341_YELLOW;
  if (!wifiOn && !bleOn)                     msg = "Both radios off -- nothing to scan.";
  else if (phase != P_IDLE && hitCount == 0) msg = "Scanning...";
  else if (hitCount == 0)                  { msg = "No Flock-pattern devices seen."; msgCol = ILI9341_GREEN; }

  if (msg) {
    uiDrawListIfChanged(4, CONTENT_Y + 4, tft.width() - 4, ROW_H, 0, MAX_HITS, prevHitRow,
      [](int, char *, size_t) {}, [](int) {});
    if (uiFieldChanged(prevMsg, sizeof(prevMsg), msg)) {
      uiClearRect(4, CONTENT_Y + 8, tft.width() - 4, ROW_H);
      tft.setTextColor(msgCol);
      tft.setCursor(4, CONTENT_Y + 8);
      tft.print(msg);
    }
    return;
  }
  prevMsg[0] = '\0';

  uiDrawListIfChanged(4, CONTENT_Y + 4, tft.width() - 4, ROW_H, hitCount, MAX_HITS, prevHitRow,
    [&](int i, char *sig, size_t cap) {
      snprintf(sig, cap, "%s|%s|%d", hits[i].kind.c_str(), hits[i].name.c_str(), hits[i].rssi);
    },
    [&](int i) {
      tft.setTextColor(ILI9341_RED);
      tft.setCursor(4, CONTENT_Y + 4 + i * ROW_H);
      tft.printf("[%s] %-16.16s %ddBm", hits[i].kind.c_str(), hits[i].name.c_str(), hits[i].rssi);
    });
}

static int bestHitRssi() {
  int best = -127;
  for (int i = 0; i < hitCount; i++) if (hits[i].rssi > best) best = hits[i].rssi;
  return best;
}

static int prevHits = 0;

// A full sweep ends. Play three fast beeps and flash green on a new hit.
static void flockSweepDone() {
  if (hitCount > prevHits) alertDetected();
  prevHits = hitCount;
}

// Demo mode fabricates view rows. It uses the same addVRow function as real scans. It skips WiFi entirely. It only runs in the view state. The learn state still uses real scans. A fake baseline would not match future scans.
static void spawnDemoHit() {
  static const char *kWifiNames[] = {"Axon FLEET-7", "FLOCK-CAM-14", "PD-ALPR-02"};
  static const char *kBleNames[]  = {"Flock Falcon", "Axon Body3", "Reveal RC3"};
  hitCount = 0;
  uint8_t mac[6];
  if (wifiOn) {
    demoRandMac(mac);
    addHit("WiFi", demoRandPick(kWifiNames, 3), demoRandRssi(), mac);
  }
  if (bleOn && hitCount < MAX_HITS) {
    demoRandMac(mac);
    addHit("BLE", demoRandPick(kBleNames, 3), demoRandRssi(), mac);
  }
  flockSweepDone();
  phase = P_IDLE;
  draw();
  lastScan = millis();
}

void flockEnter() {
  uiDrawTopBar("Flock Detect");
  beepHold(true);          // Hits chirp repeatedly. Keep the amplifier warm.
  uiClearBelow(CONTENT_Y);
  memset(prevHitRow, 0, sizeof(prevHitRow));
  prevMsg[0] = '\0';
  drawTabs();
  phase = P_IDLE;
  hitCount = 0;
  prevHits = 0;
  startScan();
  draw();
}

void flockLoop() {
  switch (phase) {
    case P_WIFI: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) return;
      if (n >= 0) harvestWifi(n); else WiFi.scanDelete();
      if (bleOn) startBle();
      else { phase = P_IDLE; flockSweepDone(); draw(); lastScan = millis(); }
      return;
    }
    case P_BLE:
      if (!bleDone) return;
      pBLEScan->clearResults();
      phase = P_IDLE;
      flockSweepDone();
      draw();
      lastScan = millis();
      return;
    default:   // P_IDLE
      if (hitCount == 0) {
        ledAlert(false);
      } else {
        ledAlert(true);
        rangeBeep(bestHitRssi());
      }
      if (millis() - lastScan > 5000) startScan();
      return;
  }
}

void flockTouch(const TouchPoint &t) {
  if (uiTouchInButton(t, wifiTab) || uiTouchInButton(t, bleTab)) {
    if (uiTouchInButton(t, wifiTab)) wifiOn = !wifiOn;
    else                             bleOn  = !bleOn;
    drawTabs();
    if (phase == P_IDLE) { startScan(); draw(); }
    uiWaitForRelease();
    return;
  }
  if (!t.isNewPress || hitCount == 0) return;
  for (int i = 0; i < hitCount; i++) {
    int rowY = CONTENT_Y + 4 + i * ROW_H;
    if (t.y >= rowY && t.y < rowY + ROW_H) {
      const Hit &h = hits[i];
      // WiFi hits store the SSID in the name field. BLE hits lack an SSID field.
      showDetectionDetail(h.name.c_str(), UI_SEV_ALERT, devTimeNowString().c_str(),
                           h.hasMac ? h.mac : nullptr,
                           h.kind == "WiFi" ? h.name.c_str() : nullptr,
                           false, 0, 0, "flock");
      // The detail function repaints the entire screen. Restore this screen's chrome and clear the redraw cache.
      uiDrawTopBar("Flock Detect");
      uiClearBelow(CONTENT_Y);
      memset(prevHitRow, 0, sizeof(prevHitRow));
      prevMsg[0] = '\0';
      drawTabs();
      draw();
      return;
    }
  }
}

void flockExit() {
  beepHold(false);
  ledAlert(false);
  ledGreen(false);
  // Free BLE memory to fit the next WiFi screen on the WROOM-32. The flockEnter function reinitializes BLE when needed.
  if (pBLEScan) {
    pBLEScan->stop();
    pBLEScan = nullptr;
    BLEDevice::deinit(false);
  }
  if (phase == P_WIFI) WiFi.scanDelete();
  phase = P_IDLE;
}
