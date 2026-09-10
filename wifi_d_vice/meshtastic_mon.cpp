// Meshtastic mesh monitor -- BLE central link to a node's phone API.
//
// Flow: scan for advertisers of the mesh service (or name "Meshtastic*"),
// pick one, enter its BLE PIN, bond, then write ToRadio{want_config_id}
// and drain FromRadio. A tiny hand-rolled protobuf walker pulls out the
// bits we show: my node num, NodeInfo (num/names/snr/hops/last-heard),
// and MeshPacket payloads for TEXT_MESSAGE_APP (1) and TELEMETRY_APP (67).
// Nothing is written onto the mesh -- receive/display only.
#include "meshtastic_mon.h"
#include "theme.h"
#include "keyboard.h"
#include "devtime.h"
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <esp_gap_ble_api.h>
#include <string.h>

// ------- Meshtastic BLE API UUIDs -------
static const char *MESH_SVC   = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
static const char *UUID_TORAD = "f75c76d2-129e-4dad-a1dd-7866124401e7";
static const char *UUID_FROMR = "2c55e69e-4993-11ed-b878-0242ac120002";
static const char *UUID_FROMN = "ed9da18c-a800-4f66-a670-aa7547e34453";

// ------- state -------
enum MState { MS_SCAN, MS_PICK, MS_CONNECT, MS_LIVE, MS_ERR };
static MState st = MS_SCAN;
static const char *errMsg = "";

struct Cand { String name; String addr; int rssi; };
static Cand cand[8];
static int  candN = 0;
static int  candSel = 0;

// Scan runs ASYNC now (continuous BLEScan + advertised-device callback,
// stopped from meshLoop after SCAN_MS) so the UI / back button stay live
// while it runs -- it used to block meshEnter() for the whole 6 s.
static uint32_t scanStartMs = 0, lastScanDraw = 0;
static const uint32_t SCAN_MS = 6000;
static uint32_t s_pin = 123456;
static Btn forgetBtn;   // "forget BLE bond" on the pick screen

static BLEClient *cli = nullptr;
static BLERemoteCharacteristic *chToRadio = nullptr, *chFromRadio = nullptr, *chFromNum = nullptr;
static volatile bool linkUp = false;
static volatile int  authResult = 0;   // 0 = pending, 1 = paired OK, -1 = pairing rejected (wrong PIN)
static volatile bool dataWaiting = true;   // poll once on connect
static uint32_t lastPoll = 0;

// ------- mesh data model -------
struct Node {
  uint32_t num = 0;
  char sName[6] = "";
  char lName[20] = "";
  float snr = 0;
  int  hops = -1;
  int  batt = -1;
  float volt = 0;
  uint32_t seen = 0;   // millis() of last update
};
static Node nodes[32];
static int  nodeN = 0;
static uint32_t myNum = 0;
static bool cfgDone = false;

static const int FEED_MAX = 36;
static char feed[FEED_MAX][46];
static int  feedHead = 0, feedCount = 0;

static void feedAdd(const char *s) {
  strncpy(feed[feedHead], s, sizeof(feed[0]) - 1);
  feed[feedHead][sizeof(feed[0]) - 1] = 0;
  feedHead = (feedHead + 1) % FEED_MAX;
  if (feedCount < FEED_MAX) feedCount++;
}

static Node *nodeFor(uint32_t num) {
  for (int i = 0; i < nodeN; i++) if (nodes[i].num == num) return &nodes[i];
  if (nodeN < (int)(sizeof(nodes) / sizeof(nodes[0]))) {
    Node &n = nodes[nodeN++];
    n = Node();
    n.num = num;
    return &n;
  }
  return nullptr;
}

static const char *nameFor(uint32_t num) {
  Node *n = nodeFor(num);
  if (n && n->sName[0]) return n->sName;
  static char hx[10];
  snprintf(hx, sizeof(hx), "!%06x", (unsigned)(num & 0xFFFFFF));
  return hx;
}

// ---------------- minimal protobuf walker ----------------
struct Pb {
  const uint8_t *p, *end;
  bool eof() const { return p >= end; }
  uint64_t varint() {
    uint64_t v = 0; int s = 0;
    while (p < end && s < 64) { uint8_t b = *p++; v |= (uint64_t)(b & 0x7F) << s; if (!(b & 0x80)) break; s += 7; }
    return v;
  }
  bool tag(uint32_t &field, uint32_t &wire) {
    if (p >= end) return false;
    uint64_t t = varint();
    field = (uint32_t)(t >> 3); wire = (uint32_t)(t & 7);
    return true;
  }
  const uint8_t *lenPref(size_t &n) { uint64_t l = varint(); n = (size_t)l; const uint8_t *b = p; p += (l <= (uint64_t)(end - p) ? l : (end - p)); return b; }
  float f32() { if (end - p < 4) { p = end; return 0; } uint32_t u; memcpy(&u, p, 4); p += 4; float f; memcpy(&f, &u, 4); return f; }
  int32_t i32() { if (end - p < 4) { p = end; return 0; } int32_t v; memcpy(&v, p, 4); p += 4; return v; }
  void skip(uint32_t wire) {
    if (wire == 0) varint();
    else if (wire == 1) p += (end - p >= 8 ? 8 : end - p);
    else if (wire == 5) p += (end - p >= 4 ? 4 : end - p);
    else if (wire == 2) { size_t n; lenPref(n); }
    else p = end;
  }
};

static void parseUser(const uint8_t *b, size_t len, Node *n) {
  Pb r{b, b + len};
  uint32_t f, w;
  while (r.tag(f, w)) {
    if (w == 2) {
      size_t sn; const uint8_t *s = r.lenPref(sn);
      if (f == 2 && n) { size_t k = sn < sizeof(n->lName) - 1 ? sn : sizeof(n->lName) - 1; memcpy(n->lName, s, k); n->lName[k] = 0; }
      else if (f == 3 && n) { size_t k = sn < sizeof(n->sName) - 1 ? sn : sizeof(n->sName) - 1; memcpy(n->sName, s, k); n->sName[k] = 0; }
    } else r.skip(w);
  }
}

static void parseNodeInfo(const uint8_t *b, size_t len) {
  Pb r{b, b + len};
  uint32_t f, w;
  uint32_t num = 0; float snr = 0; int hops = -1;
  const uint8_t *userB = nullptr; size_t userL = 0;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) num = (uint32_t)r.varint();
    else if (f == 2 && w == 2) { userB = r.lenPref(userL); }
    else if (f == 4 && w == 5) snr = r.f32();
    else if (f == 7 && w == 0) hops = (int)r.varint();
    else r.skip(w);
  }
  if (!num) return;
  Node *n = nodeFor(num);
  if (!n) return;
  if (userB) parseUser(userB, userL, n);
  if (snr != 0) n->snr = snr;
  if (hops >= 0) n->hops = hops;
  n->seen = millis();
}

static void parseTelemetry(const uint8_t *b, size_t len, uint32_t from) {
  Pb r{b, b + len};
  uint32_t f, w;
  while (r.tag(f, w)) {
    if (f == 2 && w == 2) {                 // device_metrics
      size_t dn; const uint8_t *d = r.lenPref(dn);
      Pb dr{d, d + dn};
      uint32_t df, dw; int batt = -1; float volt = 0;
      while (dr.tag(df, dw)) {
        if (df == 1 && dw == 0) batt = (int)dr.varint();
        else if (df == 2 && dw == 5) volt = dr.f32();
        else dr.skip(dw);
      }
      Node *n = nodeFor(from);
      if (n) { if (batt >= 0) n->batt = batt; if (volt != 0) n->volt = volt; n->seen = millis(); }
      char line[46];
      snprintf(line, sizeof(line), "%s  %d%%  %.2fV", nameFor(from), batt, volt);
      feedAdd(line);
    } else r.skip(w);
  }
}

static void parseData(const uint8_t *b, size_t len, uint32_t from) {
  Pb r{b, b + len};
  uint32_t f, w;
  uint32_t port = 0;
  const uint8_t *pl = nullptr; size_t plL = 0;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) port = (uint32_t)r.varint();
    else if (f == 2 && w == 2) { pl = r.lenPref(plL); }
    else r.skip(w);
  }
  if (!pl) return;
  if (port == 1) {                          // TEXT_MESSAGE_APP
    char txt[40];
    size_t k = plL < sizeof(txt) - 1 ? plL : sizeof(txt) - 1;
    memcpy(txt, pl, k); txt[k] = 0;
    char line[46];
    snprintf(line, sizeof(line), "%s: %s", nameFor(from), txt);
    feedAdd(line);
  } else if (port == 67) {                  // TELEMETRY_APP
    parseTelemetry(pl, plL, from);
  }
}

static void parseMeshPacket(const uint8_t *b, size_t len) {
  Pb r{b, b + len};
  uint32_t f, w;
  uint32_t from = 0; float rxSnr = 0; uint32_t rxTime = 0;
  const uint8_t *dataB = nullptr; size_t dataL = 0;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) from = (uint32_t)r.varint();
    else if (f == 4 && w == 2) { dataB = r.lenPref(dataL); }
    else if (f == 7 && w == 5) rxTime = (uint32_t)r.i32();   // MeshPacket.rx_time: fixed32 epoch secs
    else if (f == 8 && w == 5) rxSnr = r.f32();
    else r.skip(w);
  }
  if (from) {
    Node *n = nodeFor(from);
    if (n) { if (rxSnr != 0) n->snr = rxSnr; n->seen = millis(); }
  }
  // rx_time is stamped by the connected node only when it has a real time
  // source (GPS / NTP / phone); 0 or absent otherwise. i32() is length-checked
  // against this packet's buffer, and devTimeSetEpoch() ignores anything
  // outside 2024..2100, so a garbled offset just no-ops.
  if (rxTime) devTimeSetEpoch(rxTime, "meshtastic");
  if (dataB) parseData(dataB, dataL, from);
}

static void parseFromRadio(const uint8_t *b, size_t len) {
  Pb r{b, b + len};
  uint32_t f, w;
  while (r.tag(f, w)) {
    if (f == 1 && w == 2) { size_t n; const uint8_t *p = r.lenPref(n); parseMeshPacket(p, n); }
    else if (f == 3 && w == 2) {            // my_info
      size_t n; const uint8_t *p = r.lenPref(n);
      Pb mr{p, p + n}; uint32_t mf, mw;
      while (mr.tag(mf, mw)) { if (mf == 1 && mw == 0) myNum = (uint32_t)mr.varint(); else mr.skip(mw); }
    }
    else if (f == 4 && w == 2) { size_t n; const uint8_t *p = r.lenPref(n); parseNodeInfo(p, n); }
    else if (f == 7 && w == 0) { r.varint(); cfgDone = true; }
    else r.skip(w);
  }
}

// ---------------- BLE plumbing ----------------
class MeshSec : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return s_pin; }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    authResult = cmpl.success ? 1 : -1;
    linkUp = cmpl.success;
  }
};
static MeshSec meshSec;

// Drop every stored BLE bond. A wrong-PIN attempt can leave a half-bond
// whose bad key gets reused on later tries, so the right PIN then still
// fails -- and a reboot doesn't clear NVS bonds. Same call ble_2fa uses.
static void meshForgetBonds() {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return;
  esp_ble_bond_dev_t *list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (!list) return;
  esp_ble_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  free(list);
}

class MeshCliCB : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {}
  void onDisconnect(BLEClient *) override { linkUp = false; }
};
static MeshCliCB meshCliCB;

static void fromNumCB(BLERemoteCharacteristic *, uint8_t *, size_t, bool) {
  dataWaiting = true;
}

static void drainFromRadio() {
  if (!chFromRadio) return;
  for (int i = 0; i < 24; i++) {
    String v = chFromRadio->readValue();
    if (v.length() == 0) break;
    parseFromRadio((const uint8_t *)v.c_str(), v.length());
  }
  dataWaiting = false;
}

static bool connectTo(const String &addr) {
  authResult = 0;
  linkUp = false;
  BLEDevice::setSecurityCallbacks(&meshSec);
  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  sec->setCapability(ESP_IO_CAP_IN);
  sec->setKeySize(16);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  cli = BLEDevice::createClient();
  cli->setClientCallbacks(&meshCliCB);
  if (!cli->connect(BLEAddress(addr.c_str()))) { errMsg = "connect failed"; return false; }

  BLERemoteService *svc = cli->getService(BLEUUID(MESH_SVC));
  if (!svc) { errMsg = "no mesh service (BLE off on node?)"; cli->disconnect(); return false; }
  chToRadio   = svc->getCharacteristic(BLEUUID(UUID_TORAD));
  chFromRadio = svc->getCharacteristic(BLEUUID(UUID_FROMR));
  chFromNum   = svc->getCharacteristic(BLEUUID(UUID_FROMN));
  if (!chToRadio || !chFromRadio) { errMsg = "missing characteristics"; cli->disconnect(); return false; }

  if (chFromNum && chFromNum->canNotify()) chFromNum->registerForNotify(fromNumCB);

  // ToRadio { want_config_id = 0x2a }  -- field 3, varint
  uint8_t hs[3] = { (3 << 3) | 0, 0x2a };
  chToRadio->writeValue(hs, 2, false);

  // Wait for the encrypted link to come up AND real config data to land.
  // A wrong PIN surfaces as authResult == -1; a silent timeout is almost
  // always the phone app still holding the node's one BLE slot.
  dataWaiting = true;
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    if (authResult == -1) {
      errMsg = "PIN rejected -- read it from the node's boot log / screen";
      meshForgetBonds();          // the rejected bond is poison; clear it
      cli->disconnect();
      return false;
    }
    drainFromRadio();
    if (myNum || nodeN || cfgDone) { linkUp = true; return true; }
    delay(120);
  }
  errMsg = (authResult == 1) ? "linked, no data -- phone app still connected?"
                             : "no response -- wrong PIN, or phone still connected";
  cli->disconnect();
  return false;
}

static void teardown() {
  if (cli) {
    if (cli->isConnected()) cli->disconnect();
    cli = nullptr;   // BLEDevice keeps ownership; don't delete mid-callback
  }
  chToRadio = chFromRadio = chFromNum = nullptr;
  linkUp = false;
}

// ---------------- UI ----------------
static const int TABS_Y = UI_ACTIONROW_Y, TABS_H = UI_ACTIONROW_H, BODY_Y = UI_CONTENT_Y;
static Btn tabNodes, tabFeed;
static int view = 0;   // 0 = nodes, 1 = feed

static void drawTabs() {
  uiClearRect(0, TABS_Y, tft.width(), TABS_H);
  tabNodes = {0, TABS_Y, tft.width() / 2, TABS_H, "Nodes"};
  tabFeed  = {tft.width() / 2, TABS_Y, tft.width() - tft.width() / 2, TABS_H, "Feed"};
  uiDrawMenuButton(tabNodes);
  uiDrawMenuButton(tabFeed);
  int sx = (view == 0) ? tabNodes.x : tabFeed.x;
  int sw = (view == 0) ? tabNodes.w : tabFeed.w;
  tft.fillRect(sx + 2, TABS_Y + TABS_H - 4, sw - 4, 3, TH_ACCENT);
}

static void drawBody() {
  uiClearBelow(BODY_Y);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  if (!linkUp) {
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(4, BODY_Y + 6);
    tft.print("link lost");
  }

  int y = BODY_Y + (linkUp ? 4 : 18);
  if (view == 0) {
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(4, y);
    tft.printf("my !%06x   nodes: %d", (unsigned)(myNum & 0xFFFFFF), nodeN);
    y += 14;
    for (int i = 0; i < nodeN && y + 12 <= tft.height() - 2; i++) {
      Node &n = nodes[i];
      tft.setTextColor(n.num == myNum ? ILI9341_GREEN : ILI9341_WHITE);
      tft.setCursor(4, y);
      int age = n.seen ? (int)((millis() - n.seen) / 1000) : -1;
      tft.printf("%-4.4s s%+.0f h%d", n.sName[0] ? n.sName : "?", n.snr, n.hops);
      tft.setCursor(110, y);
      if (n.batt >= 0) tft.printf("%d%% %.1fV", n.batt, n.volt);
      tft.setCursor(190, y);
      if (age >= 0) tft.printf("%ds", age);
      y += 12;
    }
  } else {
    int shown = 0;
    int start = feedCount - 1;
    for (int k = start; k >= 0 && y + 12 <= tft.height() - 2; k--, shown++) {
      int idx = (feedHead - 1 - (start - k) + FEED_MAX * 2) % FEED_MAX;
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(4, y);
      tft.print(feed[idx]);
      y += 12;
    }
    if (shown == 0) {
      tft.setTextColor(ILI9341_YELLOW);
      tft.setCursor(4, y);
      tft.print("waiting for mesh traffic...");
    }
  }
}

// ---------------- scan / pick ----------------
// Advertised-device callback (BLE task context): collect mesh advertisers as
// they arrive. Only plain-int candN is read by the main task while the scan
// runs; cand[] fields are read only after stopScan().
class MeshScanCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if (candN >= 8) return;
    bool mesh = d.isAdvertisingService(BLEUUID(MESH_SVC));
    String nm = d.haveName() ? String(d.getName().c_str()) : String("");
    if (!mesh) { String l = nm; l.toLowerCase(); if (l.startsWith("meshtastic")) mesh = true; }
    if (!mesh) return;
    String addr = d.getAddress().toString().c_str();
    for (int i = 0; i < candN; i++) if (cand[i].addr == addr) return;   // dedup
    int slot = candN;
    cand[slot].name = nm.length() ? nm : String("Meshtastic");
    cand[slot].addr = addr;
    cand[slot].rssi = d.getRSSI();
    candN = slot + 1;   // publish only after the slot is fully populated
  }
};
static MeshScanCB meshScanCb;

static void startScan() {
  candN = 0;
  BLEScan *s = BLEDevice::getScan();
  s->setAdvertisedDeviceCallbacks(&meshScanCb, true /* want dups */);
  s->setActiveScan(true);
  s->start(0, nullptr, false);   // continuous -- returns immediately
  scanStartMs = millis();
  lastScanDraw = 0;
}

static void stopScan() {
  BLEScan *s = BLEDevice::getScan();
  s->stop();
  delay(30);                              // let an in-flight onResult finish
  s->setAdvertisedDeviceCallbacks(nullptr);
  s->clearResults();
}

static void drawScanning() {
  tft.fillRect(0, 30, tft.width(), 44, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(6, 40);
  uint32_t el = millis() - scanStartMs;
  uint32_t left = el >= SCAN_MS ? 0 : (SCAN_MS - el) / 1000 + 1;
  tft.printf("Scanning for nodes...  %lus  (%d)", (unsigned long)left, candN);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(6, 56);
  tft.print("tap to stop early   back = leave");
}

static void drawPick() {
  uiDrawTopBar("Meshtastic");
  uiClearBelow(29);
  tft.setTextSize(1);
  if (candN == 0) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(6, 40);
    tft.print("No Meshtastic nodes found.");
    tft.setCursor(6, 56);
    tft.print("Tap (not the button) to rescan.");
    forgetBtn = {6, tft.height() - 28, tft.width() - 12, 24, "forget BLE bond"};
    uiDrawMenuButton(forgetBtn);
    return;
  }
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(6, 34);
  tft.print("Pick a node:");
  int y = 50;
  for (int i = 0; i < candN && y + 26 <= tft.height() - 34; i++) {
    Btn b = {6, y, tft.width() - 12, 26, cand[i].name.c_str()};
    uiDrawMenuButton(b);
    y += 30;
  }
  forgetBtn = {6, tft.height() - 28, tft.width() - 12, 24, "forget BLE bond"};
  uiDrawMenuButton(forgetBtn);
}

// ---------------- contract ----------------
void meshEnter() {
  uiDrawTopBar("Meshtastic");
  WiFi.disconnect(true, false);   // radio coexistence -- see README
  WiFi.mode(WIFI_OFF);
  delay(50);
  BLEDevice::init("");
  nodeN = 0; myNum = 0; cfgDone = false; feedHead = feedCount = 0;
  candSel = 0;
  st = MS_SCAN;
  uiClearBelow(29);
  startScan();
  drawScanning();
}

void meshLoop() {
  if (st == MS_SCAN) {
    uint32_t now = millis();
    if (now - scanStartMs >= SCAN_MS) {
      stopScan();
      st = MS_PICK;
      candSel = 0;
      drawPick();
    } else if (now - lastScanDraw > 500) {
      lastScanDraw = now;
      drawScanning();
    }
    return;
  }
  if (st != MS_LIVE) return;
  if (!linkUp) return;
  if (dataWaiting || millis() - lastPoll > 1500) {
    lastPoll = millis();
    drainFromRadio();
    drawTabs();
    drawBody();
  }
}

void meshTouch(const TouchPoint &t) {
  if (st == MS_SCAN) {
    if (t.isNewPress) {                     // tap anywhere = stop scanning now
      stopScan();
      st = MS_PICK;
      candSel = 0;
      drawPick();
      uiWaitForRelease();
    }
    return;
  }
  if (st == MS_PICK) {
    if (t.isNewPress && uiTouchInButton(t, forgetBtn)) {
      meshForgetBonds();
      uiWaitForRelease();
      uiShowLoading("BLE bonds cleared");
      delay(600);
      drawPick();
      return;
    }
    if (candN == 0) { uiWaitForRelease(); meshEnter(); return; }
    int y = 50;
    for (int i = 0; i < candN; i++) {
      if (t.isNewPress && t.x >= 6 && t.x < tft.width() - 6 && t.y >= y && t.y < y + 26) {
        candSel = i;
        uiWaitForRelease();
        String pin = uiTextInput("Node BLE PIN", "123456", false);
        s_pin = (uint32_t)pin.toInt();
        uiDrawTopBar("Meshtastic");
        uiShowLoading("Pairing / connecting...");
        st = MS_CONNECT;
        if (connectTo(cand[candSel].addr)) {
          st = MS_LIVE;
          view = 0;
          uiDrawTopBar("Meshtastic");
          drawTabs();
          drawBody();
        } else {
          st = MS_ERR;
          uiClearBelow(29);
          tft.setTextColor(ILI9341_RED);
          tft.setTextSize(1);
          tft.setCursor(6, 44);
          tft.print(errMsg);
          tft.setCursor(6, 64);
          tft.setTextColor(ILI9341_YELLOW);
          tft.print("Tap to try again.");
        }
        return;
      }
      y += 30;
    }
    return;
  }
  if (st == MS_ERR) {
    uiWaitForRelease();
    teardown();
    meshEnter();
    return;
  }
  if (st == MS_LIVE) {
    if (uiTouchInButton(t, tabNodes)) { view = 0; drawTabs(); drawBody(); uiWaitForRelease(); }
    else if (uiTouchInButton(t, tabFeed)) { view = 1; drawTabs(); drawBody(); uiWaitForRelease(); }
  }
}

void meshExit() {
  if (st == MS_SCAN) stopScan();
  teardown();
  BLEDevice::deinit(false);   // radio coexistence -- see README; free BLE for the next WiFi screen
  st = MS_SCAN;
}

bool meshHandleBack() {
  return false;   // one back press leaves the screen; meshExit() cleans up
}
