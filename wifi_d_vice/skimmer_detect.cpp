// This module detects Bluetooth skimmers. Cheap HC-05, HC-06, and HC-08
// modules power most skimmers. The code matches module names. See
// known_signatures.h for source data.
#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include "ui.h"
#include "known_signatures.h"
#include "devtime.h"
#include "flagfinding.h"
#include "demomode.h"
#include <esp_random.h>

static BLEScan *pBLEScan = nullptr;
static uint32_t lastBleScan = 0;
static const int MAX_HITS = 6;
struct Hit { String name; int rssi; uint8_t mac[6]; };
static Hit hits[MAX_HITS];
static int hitCount = 0;

// A blocking scan freezes the user interface. The code now uses an
// asynchronous scan. The callback runs on the BLE host task. The main
// loop polls the done flag.
static volatile bool bleDone = false;
static bool scanning = false;
static int  prevHits = 0;   // for the detection edge

static void draw();

// The UART service UUID is a weak signal. Renamed modules still advertise
// it. Legitimate gadgets also use it. The code only flags it when the
// device lacks a name. Real products usually carry a specific name.
static bool hasSkimmerServiceUuid(BLEAdvertisedDevice &d) {
  for (int i = 0; i < d.getServiceUUIDCount(); i++) {
    String u = d.getServiceUUID(i).toString(); u.toLowerCase();
    if (matchesAnyPattern(u, kSkimmerServiceUuids, kSkimmerServiceUuidCount)) return true;
  }
  return false;
}

static void bleScanDone(BLEScanResults r) {
  hitCount = 0;
  for (int i = 0; i < (int)r.getCount() && hitCount < MAX_HITS; i++) {
    BLEAdvertisedDevice d = r.getDevice(i);
    String name = d.haveName() ? String(d.getName().c_str()) : String("(no name)");
    String lower = name; lower.toLowerCase();
    bool byName = d.haveName() && matchesAnyPattern(lower, kSkimmerNamePatterns, kSkimmerNamePatternCount);
    bool byUuid = hasSkimmerServiceUuid(d) &&
                  (!d.haveName() || lower.indexOf("bl") >= 0 || lower.indexOf("uart") >= 0 ||
                   lower.indexOf("serial") >= 0 || lower.indexOf("spp") >= 0);
    if (byName || byUuid) {
      Hit &h = hits[hitCount++];
      h.name = byUuid && !byName ? name + " [UART]" : name;
      h.rssi = d.getRSSI();
      memcpy(h.mac, d.getAddress().getNative(), 6);
    }
  }
  bleDone = true;
}

// Demo mode fabricates a hit list. It skips the BLE radio. The fake data
// uses the same structure. The draw and tap handlers remain unchanged.
static void spawnDemoHits() {
  static const char *kNames[] = {"HC-05", "HC-06 [UART]", "BT-Module"};
  hitCount = 1 + (int)(esp_random() % 2);   // 1..2 fake hits
  if (hitCount > MAX_HITS) hitCount = MAX_HITS;
  for (int i = 0; i < hitCount; i++) {
    Hit &h = hits[i];
    h.name = demoRandPick(kNames, 3);
    h.rssi = demoRandRssi();
    demoRandMac(h.mac);
  }
  bleDone = true;
}

static void startScan() {
  if (demoModeEnabled()) { scanning = true; spawnDemoHits(); return; }
  if (!pBLEScan) {
    WiFi.disconnect(true, false);   // The radio requires coexistence handling. See the README.
    WiFi.mode(WIFI_OFF);
    delay(50);
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setActiveScan(true);
  }
  bleDone = false;
  scanning = true;
  pBLEScan->start(2, bleScanDone, false);
}

// These variables track the last drawn signatures. The UI functions check
// them to avoid redraws. The enter function clears the screen once. It
// resets these variables.
static char prevHitRow[MAX_HITS][UI_LIST_SIG_LEN];
static char prevMsg[48] = "";
static const int LIST_Y = 50, ROW_H = 14;

static void draw() {
  tft.setTextSize(1);
  const char *msg = nullptr;
  uint16_t msgCol = ILI9341_YELLOW;
  if (scanning && hitCount == 0) msg = "  scanning...";
  else if (hitCount == 0)      { msg = "  none seen"; msgCol = ILI9341_GREEN; }

  if (msg) {
    uiDrawListIfChanged(4, LIST_Y, tft.width() - 4, ROW_H, 0, MAX_HITS, prevHitRow,
      [](int, char *, size_t) {}, [](int) {});   // just clears any rows left from a previous hit list
    if (uiFieldChanged(prevMsg, sizeof(prevMsg), msg)) {
      uiClearRect(4, LIST_Y, tft.width() - 4, ROW_H);
      tft.setTextColor(msgCol);
      tft.setCursor(4, LIST_Y);
      tft.print(msg);
    }
    return;
  }
  prevMsg[0] = '\0';   // forget it so the message reprints if the list empties out again later

  uiDrawListIfChanged(4, LIST_Y, tft.width() - 4, ROW_H, hitCount, MAX_HITS, prevHitRow,
    [&](int i, char *sig, size_t cap) { snprintf(sig, cap, "%s|%d", hits[i].name.c_str(), hits[i].rssi); },
    [&](int i) {
      tft.setTextColor(ILI9341_RED);
      tft.setCursor(4, LIST_Y + i * ROW_H);
      tft.printf("  %-16.16s %ddBm", hits[i].name.c_str(), hits[i].rssi);
    });
}

static int bestHitRssi() {
  int best = -127;
  for (int i = 0; i < hitCount; i++) if (hits[i].rssi > best) best = hits[i].rssi;
  return best;
}

void skimmerEnter() {
  uiDrawTopBar("Skimmer Detect");
  beepHold(true);          // hits chirp repeatedly -- keep the amp warm
  uiClearBelow(29);
  memset(prevHitRow, 0, sizeof(prevHitRow));
  prevMsg[0] = '\0';
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(4, 34);
  tft.print("BLE (HC-05/06/08 style modules):");
  hitCount = 0;
  prevHits = 0;
  startScan();
  draw();
  lastBleScan = millis();
}

void skimmerLoop() {
  if (scanning) {
    if (!bleDone) return;
    scanning = false;
    if (pBLEScan) pBLEScan->clearResults();
    if (hitCount > prevHits) alertDetected();   // 3 fast beeps + green flash
    prevHits = hitCount;
    draw();
    lastBleScan = millis();
    return;
  }
  if (hitCount == 0) {
    ledAlert(false);
  } else {
    ledAlert(true);
    rangeBeep(bestHitRssi());   // beep faster/higher the closer the nearest skimmer is
  }
  if (millis() - lastBleScan > 5000) startScan();
}

void skimmerTouch(const TouchPoint &t) {
  if (!t.isNewPress || hitCount == 0) return;
  for (int i = 0; i < hitCount; i++) {
    int rowY = LIST_Y + i * ROW_H;
    if (t.y >= rowY && t.y < rowY + ROW_H) {
      showDetectionDetail(hits[i].name.c_str(), UI_SEV_ALERT, devTimeNowString().c_str(),
                           hits[i].mac, nullptr, false, 0, 0, "skimmer");
      // The detail view repaints the entire screen. This code redraws the local
      // header. It clears the list redraw cache. The draw function now repaints
      // every row.
      uiDrawTopBar("Skimmer Detect");
      uiClearBelow(29);
      tft.setTextSize(1);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(4, 34);
      tft.print("BLE (HC-05/06/08 style modules):");
      memset(prevHitRow, 0, sizeof(prevHitRow));
      prevMsg[0] = '\0';
      draw();
      return;
    }
  }
}
void skimmerExit() {
  beepHold(false);
  ledAlert(false);
  ledGreen(false);
  if (pBLEScan) { pBLEScan->stop(); pBLEScan = nullptr; }
  scanning = false;
  // The radio requires coexistence handling. See the README. The code
  // reinitializes the radio through a guard check.
  if (BLEDevice::getInitialized()) BLEDevice::deinit(false);
}
