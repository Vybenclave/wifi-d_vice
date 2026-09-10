// BLE advertisement-spam watch (Wireless Wizard "Spam Detect"). Passive: a
// continuous duplicate-allowing BLE scan, counting advertisement rate,
// distinct source addresses, and the specific payload shapes the common
// spam tools flood -- Apple Continuity proximity-pairing / nearby-action
// (Flipper "BLE Spam" / Sour Apple), Microsoft SwiftPair, Google Fast Pair.
// A short window feeds one severity verdict + banner, like the WiFi IDS.
//
// This is the "spoofed / flooded beacon" half of the roadmap's Guardian
// "BLE anomaly" monitor. Receive-only: nothing here advertises.
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <string.h>
#include <math.h>
#include "ui.h"

static BLEScan *pScan = nullptr;
static bool running = false;

// Callback-side counters (BLE host task). Plain uint32 increments; read and
// zeroed from the main task once per window -- a lost count at the edge
// doesn't matter for a rate estimate.
static volatile uint32_t cAdv, cApple, cContSpam, cMs, cGfp;
static const int ADDR_BLOOM = 64;          // 512-bit distinct-address estimate
static uint8_t addrBloom[ADDR_BLOOM];

static void bloomAdd(const uint8_t *k, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) { h ^= k[i]; h *= 16777619u; }
  uint32_t h2 = (h ^ (h >> 13)) * 0x9E3779B1u, h3 = (h2 ^ (h2 >> 15)) * 0x85EBCA77u;
  uint32_t idx[3] = { h % 512u, h2 % 512u, h3 % 512u };
  for (int i = 0; i < 3; i++) addrBloom[idx[i] >> 3] |= (uint8_t)(1u << (idx[i] & 7));
}
static int bloomEst() {
  int pop = 0;
  for (int i = 0; i < ADDR_BLOOM; i++) pop += __builtin_popcount(addrBloom[i]);
  if (pop <= 0) return 0;
  if (pop >= 510) return 999;
  return (int)(-(512.0 / 3.0) * log(1.0 - (double)pop / 512.0) + 0.5);
}

class Cb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    cAdv++;
    uint8_t mac[6]; memcpy(mac, d.getAddress().getNative(), 6);
    bloomAdd(mac, 6);
    if (!d.haveManufacturerData()) return;
    String md = d.getManufacturerData();
    if (md.length() < 3) return;
    uint16_t cid = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
    uint8_t  t   = (uint8_t)md[2];
    if (cid == 0x004C) {                 // Apple
      cApple++;
      // 0x07 proximity pairing, 0x0F nearby action, 0x10 nearby info --
      // 0x07/0x0F at volume is the AirPods / "Sour Apple" popup flood.
      if (t == 0x07 || t == 0x0F) cContSpam++;
    } else if (cid == 0x0006) {          // Microsoft -- SwiftPair beacon (type 0x03)
      if (t == 0x03) cMs++;
    } else if (cid == 0x00E0) {          // Google
      cGfp++;
    }
  }
};
static Cb cb;

// ---- window / verdict --------------------------------------------
static const uint32_t WIN_MS = 2000;
static uint32_t winStart, lastDraw;
enum { SV_OK = 0, SV_WATCH = 1, SV_ALERT = 2 };
static uint8_t sev = SV_OK, lastSev = 255;
static uint32_t vRate, vCont, vMs, vGfp; static int vAddr;
static bool alerted = false;

static void computeWindow() {
  uint32_t adv = cAdv, apple = cApple, cont = cContSpam, ms = cMs, gfp = cGfp;
  cAdv = cApple = cContSpam = cMs = cGfp = 0;
  int addr = bloomEst();
  memset(addrBloom, 0, sizeof addrBloom);

  vRate = adv * 1000 / WIN_MS;    // adverts/sec
  vCont = cont; vMs = ms; vGfp = gfp; vAddr = addr;
  (void)apple;

  // Ambient BLE even in a crowd is well under 100 adv/s and a handful of
  // SwiftPair/Continuity frames. Spam tools push hundreds/s from dozens of
  // rotating addresses.
  if (vRate >= 250 || cont >= 40 || (cont >= 15 && addr >= 20) || ms >= 40 || gfp >= 60)
    sev = SV_ALERT;
  else if (vRate >= 110 || cont >= 12 || ms >= 12 || gfp >= 20)
    sev = SV_WATCH;
  else
    sev = SV_OK;
}

static uint16_t sevCol(uint8_t s) {
  return s == SV_ALERT ? ILI9341_RED : s == SV_WATCH ? ILI9341_YELLOW : ILI9341_GREEN;
}

static void draw() {
  tft.setTextSize(1);
  tft.fillRect(0, 30, tft.width(), 70, ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(2, 30);
  tft.printf("adv/s %lu   distinct addr ~%d", (unsigned long)vRate, vAddr);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(2, 44);
  tft.printf("Apple popup-spam frames: %lu", (unsigned long)vCont);
  tft.setCursor(2, 56);
  tft.printf("SwiftPair: %lu   FastPair: %lu", (unsigned long)vMs, (unsigned long)vGfp);

  if (sev == lastSev) return;
  lastSev = sev;
  tft.fillRect(4, 104, tft.width() - 8, 46, sev == SV_ALERT ? ILI9341_RED : ILI9341_BLACK);
  if (sev == SV_ALERT) {
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 112);
    tft.print("BLE SPAM");
    tft.setTextSize(1);
    tft.setCursor(10, 134);
    tft.print(vCont >= 15 ? "Apple proximity popup flood" : "advertisement flood");
    if (!alerted) { alerted = true; ledSet(true); beep(350, 1400); }
  } else {
    tft.setTextColor(sevCol(sev));
    tft.setCursor(10, 120);
    tft.print(sev == SV_WATCH ? "elevated advertisement activity" : "no advertisement flood");
    if (alerted) { alerted = false; ledSet(false); }
  }
}

void bleSpamEnter() {
  uiDrawTopBar("BLE Spam Watch");
  uiClearBelow(29);
  uiShowLoading("Initializing radio...");
  cAdv = cApple = cContSpam = cMs = cGfp = 0;
  memset(addrBloom, 0, sizeof addrBloom);
  sev = SV_OK; lastSev = 255; alerted = false;

  if (!pScan) {
    WiFi.disconnect(true, false);   // radio coexistence -- see README
    WiFi.mode(WIFI_OFF);
    delay(50);
    BLEDevice::init("");
    pScan = BLEDevice::getScan();
  }
  pScan->setActiveScan(false);
  pScan->setInterval(80);
  pScan->setWindow(70);
  pScan->setAdvertisedDeviceCallbacks(&cb, true /* duplicates */);
  pScan->start(0, nullptr, false);

  running = true;
  winStart = lastDraw = millis();
  beepHold(true);
  uiClearBelow(29);
  draw();
}

void bleSpamLoop() {
  if (!running) return;
  uint32_t now = millis();
  if (now - winStart >= WIN_MS) {
    computeWindow();
    winStart = now;
    draw();
    lastDraw = now;
  } else if (now - lastDraw > 1000) {
    // keep the rate line lively between windows
    draw();
    lastDraw = now;
  }
}

void bleSpamTouch(const TouchPoint &t) {
  if (!t.isNewPress) return;
  alerted = false;
  ledSet(false);
  lastSev = 255;
  draw();
}

void bleSpamExit() {
  if (!running) return;
  running = false;
  beepHold(false);
  ledSet(false);
  if (pScan) { pScan->stop(); pScan->setAdvertisedDeviceCallbacks(nullptr); pScan = nullptr; }
  BLEDevice::deinit(false);   // radio coexistence -- see README
}
