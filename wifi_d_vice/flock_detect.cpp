#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include "ui.h"
#include "known_signatures.h"

static uint32_t lastScan = 0;
static const int MAX_HITS = 8;
struct Hit { String kind; String name; int rssi; };
static Hit hits[MAX_HITS];
static int hitCount = 0;
static BLEScan *pBLEScan = nullptr;
static bool wifiOn = true, bleOn = true;
static Btn wifiTab, bleTab;

// Scanning is now ASYNC -- a blocking WiFi.scanNetworks()/BLE start() froze
// the UI (no back button) for 2-5s every cycle. WiFi runs via
// scanNetworks(true,...) + scanComplete() polling; BLE via the start(dur,
// callback) overload. flockLoop() just advances the state machine.
enum Phase { P_IDLE, P_WIFI, P_BLE };
static Phase phase = P_IDLE;
static volatile bool bleDone = false;

static void draw();

static void addHit(const char *kind, const String &name, int rssi) {
  if (hitCount >= MAX_HITS) return;
  hits[hitCount] = {kind, name, rssi};
  hitCount++;
}

static void harvestWifi(int n) {
  for (int i = 0; i < n && hitCount < MAX_HITS; i++) {
    String s = WiFi.SSID(i);
    String lower = s; lower.toLowerCase();
    if (matchesAnyPattern(lower, kFlockNamePatterns, kFlockNamePatternCount))
      addHit("WiFi", s, WiFi.RSSI(i));
  }
  WiFi.scanDelete();
}

// Runs on the BLE host task when the scan finishes.
static void bleScanDone(BLEScanResults r) {
  for (int i = 0; i < (int)r.getCount() && hitCount < MAX_HITS; i++) {
    BLEAdvertisedDevice d = r.getDevice(i);
    if (!d.haveName()) continue;
    String name = String(d.getName().c_str());
    String lower = name; lower.toLowerCase();
    if (matchesAnyPattern(lower, kFlockNamePatterns, kFlockNamePatternCount))
      addHit("BLE", name, d.getRSSI());
  }
  bleDone = true;
}

static void startBle() {
  if (!pBLEScan) {
    Serial.printf("[flock] BLE init, free heap %u, largest block %u\n",
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
  hitCount = 0;
  if (wifiOn)     { WiFi.mode(WIFI_STA); WiFi.scanNetworks(true, true); phase = P_WIFI; }
  else if (bleOn) { startBle(); }
  else            { phase = P_IDLE; draw(); lastScan = millis(); }
}

// A second row just below the standard top bar.
static const int TABS_Y = 29, TABS_H = 26, CONTENT_Y = TABS_Y + TABS_H + 1;

static void drawTabs() {
  // Clear the whole row first -- the Vice pill buttons are rounded rects
  // that don't paint their corners, so without this the previous screen
  // shows through around them.
  uiClearRect(0, TABS_Y, tft.width(), TABS_H);
  wifiTab = {0, TABS_Y, tft.width() / 2, TABS_H, "WiFi"};
  bleTab = {tft.width() / 2, TABS_Y, tft.width() - tft.width() / 2, TABS_H, "BLE"};
  uiDrawMenuButton(wifiTab);
  uiDrawMenuButton(bleTab);
  tft.fillRect(wifiTab.x + 2, wifiTab.y + TABS_H - 5, wifiTab.w - 4, 3, wifiOn ? ILI9341_GREEN : ILI9341_RED);
  tft.fillRect(bleTab.x + 2, bleTab.y + TABS_H - 5, bleTab.w - 4, 3, bleOn ? ILI9341_GREEN : ILI9341_RED);
}

static void draw() {
  uiClearBelow(CONTENT_Y);
  tft.setTextSize(1);
  if (!wifiOn && !bleOn) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(4, CONTENT_Y + 8);
    tft.print("Both radios off -- nothing to scan.");
  } else if (phase != P_IDLE && hitCount == 0) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(4, CONTENT_Y + 8);
    tft.print("Scanning...");
  } else if (hitCount == 0) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(4, CONTENT_Y + 8);
    tft.print("No Flock-pattern devices seen.");
  } else {
    int y = CONTENT_Y + 4;
    for (int i = 0; i < hitCount; i++) {
      tft.setTextColor(ILI9341_RED);
      tft.setCursor(4, y);
      tft.printf("[%s] %-16.16s %ddBm", hits[i].kind.c_str(), hits[i].name.c_str(), hits[i].rssi);
      y += 16;
    }
  }
}

static int bestHitRssi() {
  int best = -127;
  for (int i = 0; i < hitCount; i++) if (hits[i].rssi > best) best = hits[i].rssi;
  return best;
}

static int prevHits = 0;   // for the detection edge

// End of a full sweep: 3 fast beeps + green flash if a new hit turned up.
static void flockSweepDone() {
  if (hitCount > prevHits) alertDetected();
  prevHits = hitCount;
}

void flockEnter() {
  uiDrawTopBar("Flock Detect");
  beepHold(true);          // hits chirp repeatedly -- keep the amp warm
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
        ledSet(false);
      } else {
        ledSet(true);
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
  }
}

void flockExit() {
  beepHold(false);
  ledSet(false);
  ledGreen(false);
  if (pBLEScan) { pBLEScan->stop(); pBLEScan = nullptr; }
  if (phase == P_WIFI) WiFi.scanDelete();
  phase = P_IDLE;
  // Free BLE RAM so the next WiFi screen fits on the WROOM-32. flockEnter's
  // BLE path re-inits via !pBLEScan.
  BLEDevice::deinit(false);
}
