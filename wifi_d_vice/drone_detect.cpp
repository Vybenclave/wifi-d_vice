// The WROOM-32 chip cannot run promiscuous WiFi and the BLE stack at once.
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <string.h>
#include "ui.h"
#include "wifi_ids.h"
#include "accent.h"
#include "devtime.h"
#include "flagfinding.h"
#include "demomode.h"
#include <esp_random.h>

struct Drone {
  char    id[21];
  char    src;
  int     rssi;
  double  lat, lon;
  uint16_t hits;
  uint32_t seen;
};
static const int DR_N = 6;
static Drone dr[DR_N];
static uint32_t gMsgs;

enum Tab { T_WIFI, T_BLE };
static Tab tab = T_WIFI;
static Btn wifiTab, bleTab;

static Drone *drFind(const char *id) {
  for (int i = 0; i < DR_N; i++) if (dr[i].hits && strcmp(dr[i].id, id) == 0) return &dr[i];
  return nullptr;
}
static Drone *drSlot() {
  int lru = 0;
  for (int i = 1; i < DR_N; i++) { if (!dr[i].hits) return &dr[i]; if (dr[i].seen < dr[lru].seen) lru = i; }
  return &dr[lru];
}

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
        decodeRid(v + 4, l - 4, 'W', rssi);
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
  // NaN Remote ID uses public action frames.
  // Full parsing is complex.
  // We search for the service name.
  // We pass the trailing bytes to the decoder.
  if (f.rawLen < 32) return;
  for (int i = 24; i + 11 < f.rawLen; i++) {
    if (memcmp(f.raw + i, "opendroneid", 11) == 0) {
      decodeRid(f.raw + i + 11, f.rawLen - (i + 11), 'W', f.rssi);
      return;
    }
  }
}

static BLEScan *pScan = nullptr;
class Cb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    int rssi = d.getRSSI();
    for (int i = 0; i < d.getServiceDataCount(); i++) {
      String u = d.getServiceDataUUID(i).toString(); u.toLowerCase();
      if (u.indexOf("fffa") < 0) continue;
      String sd = d.getServiceData(i);
      if (sd.length() >= 26)
        decodeRid((const uint8_t *)sd.c_str() + 1, sd.length() - 1, 'B', rssi);
    }
    if (d.haveManufacturerData()) {
      String md = d.getManufacturerData();
      if (md.length() >= 2) {
        uint16_t cid = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
        if (cid == 0xFFE0 || cid == 0x1AE8) {
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

// Demo mode mimics real decode paths.
// This keeps UI logic unchanged.
// The code skips the radio.
// It fabricates inputs instead.
static uint32_t lastDemoSpawn = 0;
static void spawnDemoDrone() {
  static const char *kIds[] = {"DJI-drone", "SIM-UAS-01", "Parrot-X"};
  const char *id = demoRandPick(kIds, 3);
  Drone *d = drFind(id);
  if (!d) { d = drSlot(); memset(d, 0, sizeof(*d)); strncpy(d->id, id, sizeof(d->id) - 1); }
  d->src = (tab == T_WIFI) ? 'W' : 'B';
  d->rssi = demoRandRssi();
  d->hits++;
  d->seen = millis();
  if ((esp_random() % 2) == 0) {
    d->lat = 37.7749 + (double)((int)(esp_random() % 1000) - 500) / 100000.0;
    d->lon = -122.4194 + (double)((int)(esp_random() % 1000) - 500) / 100000.0;
  }
  gMsgs++;
}

static void startWifi() {
  if (demoModeEnabled()) return;
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
  if (demoModeEnabled()) return;
  if (!pScan) {
    WiFi.disconnect(true, false);   // Radio coexistence requires this order.
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
  if (BLEDevice::getInitialized()) BLEDevice::deinit(false);        // radio coexistence -- see README
}

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

// Rows use variable heights.
// The code tracks a running y cursor.
// Touch handling needs exact row geometry.
// We store each row as a button.
// This matches the client picker convention.
static Btn hitRows[DR_N];
static int hitRowIdx[DR_N];
static int hitRowCount;

static void draw() {
  uiClearBelow(CY);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(2, CY);
  tft.printf("%s   %lu RID msgs", tab == T_WIFI ? "WiFi beacon/NaN" : "BLE 0xFFFA/DJI",
             (unsigned long)gMsgs);

  hitRowCount = 0;
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
    int rowH = (d.lat != 0 || d.lon != 0) ? 22 : 12;
    if (hitRowCount < DR_N) {
      hitRows[hitRowCount] = {4, y - 2, tft.width() - 8, rowH};
      hitRowIdx[hitRowCount] = i;
      hitRowCount++;
    }
    tft.setTextColor(fresh ? ILI9341_RED : accentLabel());
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
  lastDemoSpawn = 0;
  draw();
}

void droneLoop() {
  if (demoModeEnabled()) {
    uint32_t now = millis();
    if (now - lastDemoSpawn > 5000) { spawnDemoDrone(); lastDemoSpawn = now; }
  } else if (tab == T_WIFI) {
    wifiIdsLoop();
  }
  uint32_t now = millis();
  if (now - lastDraw > 900) { draw(); lastDraw = now; }
}

void droneTouch(const TouchPoint &t) {
  if (!t.isNewPress) return;
  Tab want = tab;
  if (uiTouchInButton(t, wifiTab)) want = T_WIFI;
  else if (uiTouchInButton(t, bleTab)) want = T_BLE;
  else {
  // Tapping a drone row opens its detail view.
  // This overrides the clear-list gesture.
    for (int k = 0; k < hitRowCount; k++) {
      if (uiTouchInButton(t, hitRows[k])) {
        const Drone &d = dr[hitRowIdx[k]];
        bool hasGps = (d.lat != 0 || d.lon != 0);
        char headline[32];
        snprintf(headline, sizeof(headline), "[%c] %s", d.src, d.id);
        showDetectionDetail(headline, UI_SEV_ALERT, devTimeNowString().c_str(),
                             nullptr, nullptr, hasGps, (float)d.lat, (float)d.lon, "drone");
        uiDrawTopBar("Drone Detect");
        drawTabs();
        draw();
        return;
      }
    }
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
