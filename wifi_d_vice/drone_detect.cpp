// Drone Remote ID detector (Wireless Wizard "Drone Detect"). Passive decode
// of the ASTM F3411 / open-drone-id broadcast that FAA / EU rules now
// require most drones to transmit in the clear. Two transports, picked with
// the WiFi / BLE tab (the WROOM-32 cannot run promiscuous WiFi and the BLE
// stack at once):
//
//   WiFi  -- beacon vendor IE, OUI FA:0B:BC (ASTM) or the DJI OUI
//            26:37:12, carrying a Remote ID message pack. Runs on the
//            shared wifi_ids promiscuous core.
//   BLE   -- service data for UUID 0xFFFA (ASTM) with a 25-byte Remote ID
//            message, or DJI's 0xFFE0 manufacturer data.
//
// Decodes the Basic ID message (the UAS / serial id) and, when the Location
// message is in the same advert, the operator-reported lat/lon. Everything
// is receive-only.
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <string.h>
#include "ui.h"
#include "wifi_ids.h"
#include "theme.h"

struct Drone {
  char    id[21];       // UAS ID / serial (ASCII, from a Basic ID message)
  char    src;          // 'W' wifi, 'B' ble
  int     rssi;
  double  lat, lon;     // 0 = not reported in this advert
  uint16_t hits;
  uint32_t seen;
};
static const int DR_N = 6;
static Drone dr[DR_N];
static uint32_t gMsgs;

enum Tab { T_WIFI, T_BLE };
static Tab tab = T_WIFI;
static Btn wifiTab, bleTab;

// ---- shared decode ------------------------------------------------
static Drone *drFind(const char *id) {
  for (int i = 0; i < DR_N; i++) if (dr[i].hits && strcmp(dr[i].id, id) == 0) return &dr[i];
  return nullptr;
}
static Drone *drSlot() {
  int lru = 0;
  for (int i = 1; i < DR_N; i++) { if (!dr[i].hits) return &dr[i]; if (dr[i].seen < dr[lru].seen) lru = i; }
  return &dr[lru];
}

// One 25-byte ASTM message. Returns via *idOut / *lat / *lon what it found
// (Basic ID fills idOut; Location fills lat/lon).
static void astmMessage(const uint8_t *m, char *idOut, size_t idN, double *lat, double *lon) {
  uint8_t type = m[0] >> 4;
  if (type == 0x0 && idOut && idN) {       // Basic ID: bytes 2..21 = UAS ID
    size_t k = 0;
    for (int i = 2; i < 22 && k < idN - 1; i++) {
      char c = (char)m[i];
      if (c == 0) break;
      idOut[k++] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    idOut[k] = 0;
  } else if (type == 0x1) {                // Location: lat @ 9, lon @ 13 (int32 LE, deg*1e7)
    int32_t la = (int32_t)(m[9] | (m[10] << 8) | (m[11] << 16) | ((uint32_t)m[12] << 24));
    int32_t lo = (int32_t)(m[13] | (m[14] << 8) | (m[15] << 16) | ((uint32_t)m[16] << 24));
    *lat = la / 1e7;
    *lon = lo / 1e7;
  }
}

// Decode a Remote ID payload that is either a single 25-byte message or a
// message pack (type 0xF: [0]=0xF., [1]=msg size, [2]=count, then messages).
static void decodeRid(const uint8_t *p, int len, char src, int rssi) {
  if (len < 25) return;
  char id[21] = "";
  double lat = 0, lon = 0;

  if ((p[0] >> 4) == 0xF && len >= 3 + 25) {
    int msz = p[1] ? p[1] : 25, cnt = p[2];
    const uint8_t *q = p + 3;
    for (int i = 0; i < cnt && (q + msz) <= (p + len); i++, q += msz)
      astmMessage(q, id[0] ? nullptr : id, id[0] ? 0 : sizeof id, &lat, &lon);
  } else {
    astmMessage(p, id, sizeof id, &lat, &lon);
  }
  if (!id[0]) snprintf(id, sizeof id, "%s-drone", src == 'B' ? "BLE" : "WiFi");

  gMsgs++;
  Drone *d = drFind(id);
  if (!d) { d = drSlot(); memset(d, 0, sizeof(*d)); strncpy(d->id, id, sizeof d->id - 1); }
  d->src = src;
  d->rssi = rssi;
  d->hits++;
  d->seen = millis();
  if (lat != 0 || lon != 0) { d->lat = lat; d->lon = lon; }
}

// ---- WiFi path (wifi_ids core) ----------------------------------
static int hBeacon = -1, hAction = -1;
static const uint8_t OUI_ASTM[3] = { 0xFA, 0x0B, 0xBC };
static const uint8_t OUI_DJI[3]  = { 0x26, 0x37, 0x12 };

static void scanIesForRid(const uint8_t *ie, int n, int rssi) {
  int i = 0;
  while (i + 2 <= n) {
    uint8_t id = ie[i], l = ie[i + 1];
    if (i + 2 + l > n) break;
    if (id == 0xDD && l >= 4) {
      const uint8_t *v = ie + i + 2;
      if (!memcmp(v, OUI_ASTM, 3))
        decodeRid(v + 4, l - 4, 'W', rssi);          // vendor type at v[3], payload after
      else if (!memcmp(v, OUI_DJI, 3)) {
        Drone *d = drFind("DJI-drone");
        gMsgs++;
        if (!d) { d = drSlot(); memset(d, 0, sizeof(*d)); strcpy(d->id, "DJI-drone"); }
        d->src = 'W'; d->rssi = rssi; d->hits++; d->seen = millis();
      }
    }
    i += 2 + l;
  }
}
static void onBeacon(const WifiIdsFrame &f, void *) {
  if (f.rawLen < 38) return;
  scanIesForRid(f.raw + 36, f.rawLen - 36, f.rssi);
}
static void onAction(const WifiIdsFrame &f, void *) {
  // NaN Remote ID: a public action frame carrying the open-drone-id service.
  // Full NaN SDF parsing is deep; key on the service name that rides in the
  // clear, then hand the trailing bytes to the RID decoder.
  if (f.rawLen < 32) return;
  for (int i = 24; i + 11 < f.rawLen; i++) {
    if (memcmp(f.raw + i, "opendroneid", 11) == 0) {
      decodeRid(f.raw + i + 11, f.rawLen - (i + 11), 'W', f.rssi);
      return;
    }
  }
}

// ---- BLE path -------------------------------------------------
static BLEScan *pScan = nullptr;
class Cb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    int rssi = d.getRSSI();
    for (int i = 0; i < d.getServiceDataCount(); i++) {
      String u = d.getServiceDataUUID(i).toString(); u.toLowerCase();
      if (u.indexOf("fffa") < 0) continue;
      String sd = d.getServiceData(i);
      if (sd.length() >= 26)                       // 1 msg-counter byte + 25-byte message
        decodeRid((const uint8_t *)sd.c_str() + 1, sd.length() - 1, 'B', rssi);
    }
    if (d.haveManufacturerData()) {
      String md = d.getManufacturerData();
      if (md.length() >= 2) {
        uint16_t cid = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
        if (cid == 0xFFE0 || cid == 0x1AE8) {     // DJI
          Drone *dr2 = drFind("DJI-drone");
          gMsgs++;
          if (!dr2) { dr2 = drSlot(); memset(dr2, 0, sizeof(*dr2)); strcpy(dr2->id, "DJI-drone"); }
          dr2->src = 'B'; dr2->rssi = rssi; dr2->hits++; dr2->seen = millis();
        }
      }
    }
  }
};
static Cb cb;

// ---- radio bring-up / teardown per tab ------------------------
static void startWifi() {
  wifiIdsBegin();
  wifiIdsSetDwell(250);
  hBeacon = wifiIdsRegister(&onBeacon, nullptr, WIDS_BIT(WIDS_BEACON));
  hAction = wifiIdsRegister(&onAction, nullptr, WIDS_BIT(0xD));   // 0xD = mgmt action
}
static void stopWifi() {
  if (hBeacon >= 0) wifiIdsUnregister(hBeacon);
  if (hAction >= 0) wifiIdsUnregister(hAction);
  hBeacon = hAction = -1;
  if (wifiIdsActive()) wifiIdsEnd();
}
static void startBle() {
  if (!pScan) {
    WiFi.disconnect(true, false);   // radio coexistence -- see README
    WiFi.mode(WIFI_OFF);
    delay(50);
    BLEDevice::init("");
    pScan = BLEDevice::getScan();
  }
  pScan->setActiveScan(false);
  pScan->setInterval(160);
  pScan->setWindow(150);
  pScan->setAdvertisedDeviceCallbacks(&cb, true);
  pScan->start(0, nullptr, false);
}
static void stopBle() {
  if (pScan) { pScan->stop(); pScan->setAdvertisedDeviceCallbacks(nullptr); pScan = nullptr; }
  BLEDevice::deinit(false);        // radio coexistence -- see README
}

// ---- UI -----------------------------------------------------
static const int TABS_Y = 29, TABS_H = 24, CY = TABS_Y + TABS_H + 2;
static uint32_t lastDraw;

static void drawTabs() {
  uiClearRect(0, TABS_Y, tft.width(), TABS_H);
  wifiTab = {0, TABS_Y, tft.width() / 2, TABS_H, "WiFi"};
  bleTab  = {tft.width() / 2, TABS_Y, tft.width() - tft.width() / 2, TABS_H, "BLE"};
  uiDrawMenuButton(wifiTab);
  uiDrawMenuButton(bleTab);
  int ax = (tab == T_WIFI ? wifiTab.x : bleTab.x) + 2;
  int aw = (tab == T_WIFI ? wifiTab.w : bleTab.w) - 4;
  tft.fillRect(ax, TABS_Y + TABS_H - 4, aw, 3, ILI9341_GREEN);
}

static void draw() {
  uiClearBelow(CY);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(2, CY);
  tft.printf("%s   %lu RID msgs", tab == T_WIFI ? "WiFi beacon/NaN" : "BLE 0xFFFA/DJI",
             (unsigned long)gMsgs);

  int n = 0;
  for (int i = 0; i < DR_N; i++) if (dr[i].hits) n++;
  int y = CY + 16;
  if (n == 0) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(4, y + 4);
    tft.print("no Remote ID broadcasts seen");
    return;
  }
  for (int i = 0; i < DR_N; i++) {
    if (!dr[i].hits) continue;
    const Drone &d = dr[i];
    bool fresh = millis() - d.seen < 10000;
    tft.setTextColor(fresh ? ILI9341_RED : thLabel());
    tft.setCursor(4, y);
    tft.printf("[%c] %-20.20s %ddBm x%u", d.src, d.id, d.rssi, d.hits);
    if (d.lat != 0 || d.lon != 0) {
      tft.setTextColor(ILI9341_YELLOW);
      tft.setCursor(16, y + 10);
      tft.printf("op %.5f, %.5f", d.lat, d.lon);
      y += 22;
    } else {
      y += 12;
    }
    if (y + 12 > tft.height() - UI_STATUSBAR_H) break;
  }
}

void droneEnter() {
  uiDrawTopBar("Drone Detect");
  memset(dr, 0, sizeof dr);
  gMsgs = 0;
  tab = T_WIFI;
  drawTabs();
  startWifi();
  lastDraw = 0;
  draw();
}

void droneLoop() {
  if (tab == T_WIFI) wifiIdsLoop();
  uint32_t now = millis();
  if (now - lastDraw > 900) { draw(); lastDraw = now; }
}

void droneTouch(const TouchPoint &t) {
  if (!t.isNewPress) return;
  Tab want = tab;
  if (uiTouchInButton(t, wifiTab)) want = T_WIFI;
  else if (uiTouchInButton(t, bleTab)) want = T_BLE;
  else { // tap in body clears the list
    if (t.y > CY) { memset(dr, 0, sizeof dr); gMsgs = 0; draw(); }
    return;
  }
  if (want == tab) { uiWaitForRelease(); return; }

  if (tab == T_WIFI) stopWifi(); else stopBle();
  tab = want;
  uiShowLoading("switching radio...");
  if (tab == T_WIFI) startWifi(); else startBle();
  drawTabs();
  draw();
  uiWaitForRelease();
}

void droneExit() {
  if (tab == T_WIFI) stopWifi(); else stopBle();
}
