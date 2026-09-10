// Bluetooth skimmer detection: cheap HC-05/HC-06/HC-08 serial modules are
// the commodity part found in pump/ATM overlay skimmers -- name substring
// match (see known_signatures.h for sourcing).
//
// (An earlier version also drove a PN532 for an NFC/RFID card-UID audit,
// but that was dropped -- it didn't actually detect a hidden reader's field
// (not something a simple PN532 setup can do) and the "audit your own
// card's UID" use case was judged low-value. If NFC/RFID detection is
// wanted again later, note it needs its own real capability, not just
// re-adding this.)
#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include "ui.h"
#include "known_signatures.h"

static BLEScan *pBLEScan = nullptr;
static uint32_t lastBleScan = 0;
static const int MAX_HITS = 6;
struct Hit { String name; int rssi; };
static Hit hits[MAX_HITS];
static int hitCount = 0;

// ASYNC scan -- a blocking pBLEScan->start() froze the UI (no back button)
// for ~1s every cycle. Now uses the start(dur, callback) overload; the
// callback runs on the BLE host task and skimmerLoop() polls `bleDone`.
static volatile bool bleDone = false;
static bool scanning = false;
static int  prevHits = 0;   // for the detection edge

static void draw();

// The UART-bridge service UUID is a weaker signal than the module name (a
// renamed module still advertises it, but so do some legit serial gadgets),
// so it only counts when the device is ALSO nameless or generically named --
// a real HM-10 in a product usually carries the product's name.
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
    if (byName || byUuid)
      hits[hitCount++] = {byUuid && !byName ? name + " [UART]" : name, d.getRSSI()};
  }
  bleDone = true;
}

static void startScan() {
  if (!pBLEScan) {
    WiFi.disconnect(true, false);   // radio coexistence -- see README
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

static void draw() {
  uiClearBelow(29);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(4, 34);
  tft.print("BLE (HC-05/06/08 style modules):");
  if (scanning && hitCount == 0) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(4, 50);
    tft.print("  scanning...");
  } else if (hitCount == 0) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(4, 50);
    tft.print("  none seen");
  } else {
    int y = 50;
    for (int i = 0; i < hitCount; i++) {
      tft.setTextColor(ILI9341_RED);
      tft.setCursor(4, y);
      tft.printf("  %-16.16s %ddBm", hits[i].name.c_str(), hits[i].rssi);
      y += 14;
    }
  }
}

static int bestHitRssi() {
  int best = -127;
  for (int i = 0; i < hitCount; i++) if (hits[i].rssi > best) best = hits[i].rssi;
  return best;
}

void skimmerEnter() {
  uiDrawTopBar("Skimmer Detect");
  beepHold(true);          // hits chirp repeatedly -- keep the amp warm
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
    ledSet(false);
  } else {
    ledSet(true);
    rangeBeep(bestHitRssi());   // beep faster/higher the closer the nearest skimmer is
  }
  if (millis() - lastBleScan > 5000) startScan();
}

void skimmerTouch(const TouchPoint &t) { (void)t; }
void skimmerExit() {
  beepHold(false);
  ledSet(false);
  ledGreen(false);
  if (pBLEScan) { pBLEScan->stop(); pBLEScan = nullptr; }
  scanning = false;
  BLEDevice::deinit(false);   // radio coexistence -- see README; re-inits via the !pBLEScan guard
}
