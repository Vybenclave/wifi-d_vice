// Meshtastic mesh monitor. This code links to a node phone API via BLE.
//
// Scan for mesh service advertisers. Pick one device. Enter its BLE PIN. Bond the device. Write a config request. Drain the response stream. A custom protobuf parser extracts node numbers and packet payloads. The code only receives data. It never writes to the mesh.
#include "meshtastic_mon.h"
#include "accent.h"
#include "keyboard.h"
#include "devtime.h"
#include "wifi_ids.h"
#include "power.h"
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <esp_gap_ble_api.h>
#include <Preferences.h>
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

// Scan runs asynchronously. The main task stops the scan after six seconds. This keeps the user interface responsive.
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
  uint32_t seen = 0;   // millis() of last update -- drives "last heard"
};
static Node nodes[32];
static int  nodeN = 0;
static uint32_t myNum = 0;
static bool cfgDone = false;

// Store channel names for the alerts picker. The firmware reads one channel message per slot. Channel names stay under twelve bytes. An empty name means the node uses a default slot. The picker shows the slot index for empty names.
static char channelName[8][13];

static const int FEED_MAX = 36;
static char feed[FEED_MAX][46];
static int  feedHead = 0, feedCount = 0;

// ------- alert-channel commands (Guardian pre-work) -------
// Listen to a dedicated channel for remote commands. Meshtastic channels use PSK encryption. This encryption provides basic access control. The status command uses only channel encryption. Other commands require a separate code. This code prevents accidental triggers.
static int8_t s_alertChan = -1;        // -1 = no alert channel configured
static char   s_alertCode[24] = "";    // "" = no code set -- gated commands always refuse
static bool   s_idsArmed = false;      // background WiFi IDS raw-capture ref held via a command
static bool   s_guardianArmed = false; // placeholder flag only -- see meshSetGuardianArmed()

// Load alert settings once during setup. This function must run before the boot-time armed check.
void meshAlertLoad() {
  Preferences p;
  p.begin("meshalert", true);
  s_alertChan = (int8_t)p.getChar("chan", -1);
  String c = p.getString("code", "");
  strncpy(s_alertCode, c.c_str(), sizeof(s_alertCode) - 1);
  s_alertCode[sizeof(s_alertCode) - 1] = 0;
  s_idsArmed = p.getBool("idsarm", false);
  s_guardianArmed = p.getBool("gdnarm", false);
  p.end();
}
// Check if background WiFi capture is armed. The mesh link stays bound to this screen. The background capture runs independently. The main loop checks this flag to control the capture.
bool meshIdsArmed() { return s_idsArmed; }

static int  meshAlertChannelGet() { return s_alertChan; }
static void meshAlertSetChannel(int ch) {
  s_alertChan = (int8_t)ch;
  Preferences p; p.begin("meshalert", false); p.putChar("chan", (int8_t)ch); p.end();
}
static const char *meshAlertCodeGet() { return s_alertCode; }
static void meshAlertSetCode(const char *code) {
  strncpy(s_alertCode, code, sizeof(s_alertCode) - 1);
  s_alertCode[sizeof(s_alertCode) - 1] = 0;
  Preferences p; p.begin("meshalert", false); p.putString("code", s_alertCode); p.end();
}
// Manage the background WiFi sniffer reference. The main loop uses this flag to control capture. Detectors run only while their screen is open. This function keeps frames flowing into the ring buffer.
static void meshSetIdsArmed(bool on) {
  if (on == s_idsArmed) return;
  s_idsArmed = on;
  Preferences p; p.begin("meshalert", false); p.putBool("idsarm", on); p.end();
  if (on) wifiIdsBegin(); else wifiIdsEnd();
}
static void meshSetGuardianArmed(bool on) {
  if (on == s_guardianArmed) return;
  s_guardianArmed = on;
  Preferences p; p.begin("meshalert", false); p.putBool("gdnarm", on); p.end();
}

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
  uint32_t num = 0;
  const uint8_t *userB = nullptr; size_t userL = 0;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) num = (uint32_t)r.varint();
    else if (f == 2 && w == 2) { userB = r.lenPref(userL); }
    else r.skip(w);
  }
  if (!num) return;
  Node *n = nodeFor(num);
  if (!n) return;
  if (userB) parseUser(userB, userL, n);
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
      if (n) n->seen = millis();
      char line[46];
      snprintf(line, sizeof(line), "%s  %d%%  %.2fV", nameFor(from), batt, volt);
      feedAdd(line);
    } else r.skip(w);
  }
}

// Parse channel messages. The structure contains an index, settings, and a role. This code only extracts the channel name. It skips all other fields.
static void parseChannel(const uint8_t *b, size_t len) {
  Pb r{b, b + len};
  uint32_t f, w;
  int32_t idx = -1;
  const uint8_t *settingsB = nullptr; size_t settingsL = 0;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) idx = (int32_t)r.varint();
    else if (f == 2 && w == 2) { settingsB = r.lenPref(settingsL); }
    else r.skip(w);
  }
  if (idx < 0 || idx > 7 || !settingsB) return;
  Pb sr{settingsB, settingsB + settingsL};
  uint32_t sf, sw;
  while (sr.tag(sf, sw)) {
    if (sf == 3 && sw == 2) {   // ChannelSettings.name
      size_t nl; const uint8_t *nb = sr.lenPref(nl);
      size_t k = nl < sizeof(channelName[0]) - 1 ? nl : sizeof(channelName[0]) - 1;
      memcpy(channelName[idx], nb, k);
      channelName[idx][k] = 0;
    } else sr.skip(sw);
  }
}

// ---------------- outbound: alert-channel command replies ----------------
// Build alert command replies. This encoder mirrors the decoder above. Submessages use length prefixes. The code builds innermost messages first. It writes each layer into a separate stack buffer. The field numbers match the decoder. Verify replies on your specific node firmware.
struct PbW {
  uint8_t *p; size_t cap, n = 0;
  void varint(uint64_t v) {
    while (v >= 0x80) { if (n < cap) p[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    if (n < cap) p[n++] = (uint8_t)v;
  }
  void tag(uint32_t field, uint32_t wire) { varint(((uint64_t)field << 3) | wire); }
  void bytesField(uint32_t field, const uint8_t *b, size_t len) {
    tag(field, 2); varint(len);
    for (size_t i = 0; i < len && n < cap; i++) p[n++] = b[i];
  }
  void varintField(uint32_t field, uint64_t v) { tag(field, 0); varint(v); }
};

static void meshSendText(uint32_t channel, const char *text) {
  if (!chToRadio) return;

  uint8_t dataBuf[110];
  PbW d{dataBuf, sizeof(dataBuf)};
  d.varintField(1, 1);                                     // Data.portnum = TEXT_MESSAGE_APP
  d.bytesField(2, (const uint8_t *)text, strlen(text));     // Data.payload

  uint8_t pktBuf[150];
  PbW m{pktBuf, sizeof(pktBuf)};
  m.varintField(2, 0xFFFFFFFFu);    // MeshPacket.to = broadcast
  m.varintField(3, channel);        // MeshPacket.channel
  m.bytesField(4, dataBuf, d.n);    // MeshPacket.decoded
  m.varintField(9, 3);              // MeshPacket.hop_limit (Meshtastic's own default)

  uint8_t toBuf[170];
  PbW t{toBuf, sizeof(toBuf)};
  t.bytesField(1, pktBuf, m.n);     // ToRadio.packet

  chToRadio->writeValue(toBuf, t.n, false);
}

static const char *alertChanLabel(int i) {
  if (channelName[i][0]) return channelName[i];
  static const char *labels[8] = {"0", "1", "2", "3", "4", "5", "6", "7"};
  return labels[i];
}

// Handle alert channel commands. The status command needs no code. Other commands require a code. This split prevents stray messages from triggering dangerous actions.
static void meshHandleCommand(const char *text) {
  int chan = meshAlertChannelGet();
  if (chan < 0) return;   // no alert channel configured -- nothing to do

  String orig = String(text);
  orig.trim();
  String lower = orig; lower.toLowerCase();

  char reply[110] = "";

  if (lower == "status") {
    int pct = uiBatteryPct();
    char pwr[24];
    if (pct < 0) snprintf(pwr, sizeof(pwr), "USB");
    else         snprintf(pwr, sizeof(pwr), "%d%% (%.2fV)", pct, uiBatteryMv() / 1000.0f);
    uint32_t up = millis() / 1000;
    snprintf(reply, sizeof(reply), "status: %s up=%luh%02lum ids=%s guardian=%s",
             pwr, (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
             s_idsArmed ? "on" : "off", s_guardianArmed ? "on" : "off");
  } else {
    const char *code = meshAlertCodeGet();
    size_t codeLen = strlen(code);
    // Code match is case-sensitive (it's a shared secret, not a command
    // word) against `orig`, not the lowercased copy.
    bool codeOk = code[0] && orig.length() > codeLen &&
                  strncmp(orig.c_str(), code, codeLen) == 0 && orig.charAt(codeLen) == ' ';
    if (!codeOk) {
      snprintf(reply, sizeof(reply), code[0] ? "bad code" : "no alert code configured");
    } else {
      String cmd = orig.substring(codeLen + 1); cmd.trim(); cmd.toLowerCase();
      if (cmd == "reboot") {
        meshSendText(chan, "rebooting...");
        delay(300);   // give the write a moment to actually go out over BLE before the reset
        ESP.restart();
      } else if (cmd == "ids on" || cmd == "ids off") {
        bool on = (cmd == "ids on");
        meshSetIdsArmed(on);
        snprintf(reply, sizeof(reply), "ids: %s", on ? "on" : "off");
      } else if (cmd == "guardian on" || cmd == "guardian off") {
        bool on = (cmd == "guardian on");
        meshSetGuardianArmed(on);
        snprintf(reply, sizeof(reply), "guardian: %s (placeholder, no monitors wired up yet)", on ? "on" : "off");
      } else {
        snprintf(reply, sizeof(reply), "unknown command");
      }
    }
  }
  if (reply[0]) meshSendText(chan, reply);
}

static void parseData(const uint8_t *b, size_t len, uint32_t from, uint32_t channel) {
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
    if ((int)channel == meshAlertChannelGet()) meshHandleCommand(txt);
  } else if (port == 67) {                  // TELEMETRY_APP
    parseTelemetry(pl, plL, from);
  }
}

static void parseMeshPacket(const uint8_t *b, size_t len) {
  Pb r{b, b + len};
  uint32_t f, w;
  uint32_t from = 0; uint32_t rxTime = 0; uint32_t channel = 0;
  const uint8_t *dataB = nullptr; size_t dataL = 0;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) from = (uint32_t)r.varint();
    else if (f == 3 && w == 0) channel = (uint32_t)r.varint();   // MeshPacket.channel
    else if (f == 4 && w == 2) { dataB = r.lenPref(dataL); }
    else if (f == 7 && w == 5) rxTime = (uint32_t)r.i32();   // MeshPacket.rx_time: fixed32 epoch secs
    else r.skip(w);
  }
  if (from) {
    Node *n = nodeFor(from);
    if (n) n->seen = millis();
  }
  // rx_time is stamped by the connected node only when it has a real time
  // source (GPS / NTP / phone); 0 or absent otherwise. i32() is length-checked
  // against this packet's buffer, and devTimeSetEpoch() ignores anything
  // outside 2024..2100, so a garbled offset just no-ops.
  if (rxTime) devTimeSetEpoch(rxTime, "meshtastic");
  if (dataB) parseData(dataB, dataL, from, channel);
}

static void parseFromRadio(const uint8_t *b, size_t len) {
  Pb r{b, b + len};
  uint32_t f, w;
  while (r.tag(f, w)) {
    // FromRadio.id is field 1 (uint32, varint) -- NOT part of the
    // payload_variant oneof, which starts at field 2 with `packet`.
    // Confirmed against meshtastic/protobufs' mesh.proto back to the
    // earliest tagged release (v2.6.13); this file previously checked
    // f==1 here, which (wire type 0 vs 2) could never actually match a
    // real packet -- parseMeshPacket() was effectively dead code, so
    // live text messages/telemetry/SNR updates and the whole
    // alert-channel command feature likely never fired on real hardware
    // until this fix.
    if (f == 2 && w == 2) { size_t n; const uint8_t *p = r.lenPref(n); parseMeshPacket(p, n); }
    else if (f == 3 && w == 2) {            // my_info
      size_t n; const uint8_t *p = r.lenPref(n);
      Pb mr{p, p + n}; uint32_t mf, mw;
      while (mr.tag(mf, mw)) { if (mf == 1 && mw == 0) myNum = (uint32_t)mr.varint(); else mr.skip(mw); }
    }
    else if (f == 4 && w == 2) { size_t n; const uint8_t *p = r.lenPref(n); parseNodeInfo(p, n); }
    else if (f == 7 && w == 0) { r.varint(); cfgDone = true; }
    else if (f == 10 && w == 2) { size_t n; const uint8_t *p = r.lenPref(n); parseChannel(p, n); }
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

// Clear all stored BLE bonds. A failed pairing attempt saves a bad key. The system reuses this key on future tries. A reboot does not clear the key. This function removes all bonds.
static void meshForgetBonds() {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return;
  esp_ble_bond_dev_t *list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (!list) return;
  esp_ble_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  free(list);
}

// Check if the device already holds a BLE bond. The system only uses a passkey during initial pairing. Reconnections use stored keys. The code skips the passkey prompt for bonded devices.
static bool meshIsBonded(const String &addr) {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return false;
  esp_ble_bond_dev_t *list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (!list) return false;
  esp_ble_get_bond_device_list(&n, list);
  const uint8_t *target = BLEAddress(addr.c_str()).getNative();
  bool found = false;
  for (int i = 0; i < n; i++)
    if (memcmp(list[i].bd_addr, target, 6) == 0) { found = true; break; }
  free(list);
  return found;
}

class MeshCliCB : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {}
  void onDisconnect(BLEClient *) override { linkUp = false; }
};
static MeshCliCB meshCliCB;

static void fromNumCB(BLERemoteCharacteristic *, uint8_t *, size_t, bool) {
  dataWaiting = true;
}

// Drain the response stream in small batches. A single GATT read can block forever. The main task would hang during a long drain. This code processes four messages per loop iteration. It clears the wait flag only when the queue is truly empty. The main loop resumes draining on the next iteration.
// Bound the read operation with a timeout. The library cannot cancel a stuck read. This code runs the read on a separate task. It waits for one and a half seconds. It abandons the task if the timeout expires. The task deletes itself when the read finishes. A small memory leak on timeout is acceptable.
struct MeshReadArgs {
  BLERemoteCharacteristic *ch;
  String result;
  volatile bool done = false;
};

static void meshReadTask(void *arg) {
  MeshReadArgs *a = (MeshReadArgs *)arg;
  a->result = a->ch->readValue();
  a->done = true;
  vTaskDelete(nullptr);
}

// Read a message with a timeout. Return false if the operation times out. Return true if the read succeeds. The output string may be empty when the queue drains completely.
static bool readFromRadioWithTimeout(String &out) {
  MeshReadArgs *args = new MeshReadArgs{chFromRadio};
  xTaskCreatePinnedToCore(meshReadTask, "meshRd", 4096, args, 1, nullptr, 0);
  const uint32_t TIMEOUT_MS = 1500;   // comfortably over a normal single GATT round trip
  uint32_t t0 = millis();
  while (!args->done && millis() - t0 < TIMEOUT_MS) delay(5);
  if (!args->done) return false;   // abandon -- see comment above
  out = args->result;
  delete args;
  return true;
}

static void drainFromRadio() {
  if (!chFromRadio) return;
  const int BATCH = 4;
  for (int i = 0; i < BATCH; i++) {
    String v;
    if (!readFromRadioWithTimeout(v)) { dataWaiting = true; return; }   // stalled -- bail this tick, retry next
    if (v.length() == 0) { dataWaiting = false; return; }
    parseFromRadio((const uint8_t *)v.c_str(), v.length());
  }
  dataWaiting = true;   // hit the batch cap -- more may still be queued
}

struct MeshConnectArgs {
  BLEClient *cli;
  BLEAddress addr;
  volatile bool done = false;
  volatile bool ok = false;
};

static void meshConnectTask(void *arg) {
  MeshConnectArgs *a = (MeshConnectArgs *)arg;
  a->ok = a->cli->connect(a->addr, 0xFF, 8000);
  a->done = true;
  vTaskDelete(nullptr);
}

// Connect to a BLE device with a timeout. The library blocks the main task for up to eight seconds. Touch input stops working during this wait. This code runs the connection on a separate task. It polls for touch input and cancels the wait if the user presses back. It abandons the task on timeout.
static bool connectWithCancel(BLEClient *c, const BLEAddress &addr, bool *cancelled) {
  *cancelled = false;
  MeshConnectArgs *args = new MeshConnectArgs{c, addr};
  xTaskCreatePinnedToCore(meshConnectTask, "meshConn", 4096, args, 1, nullptr, 0);
  uint32_t t0 = millis();
  while (!args->done && millis() - t0 < 8500) {   // a hair over the task's own 8000ms cap
    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) { *cancelled = true; uiWaitForRelease(); return false; }
    delay(20);
  }
  if (!args->done) return false;   // task itself still stuck somehow -- abandon, same as above
  bool ok = args->ok;
  delete args;
  return ok;
}

static bool connectTo(const String &addr, bool *cancelled) {
  *cancelled = false;
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
  if (!connectWithCancel(cli, BLEAddress(addr.c_str()), cancelled)) {
    if (!*cancelled) errMsg = "connect failed";
    return false;
  }

  // Set the MTU to 517 bytes. The default MTU is too small for node info messages. Small MTUs force multiple ATT read blob exchanges. A lost packet stalls the read. This code requests the maximum MTU. The call does not block.
  cli->setMTU(517);

  BLERemoteService *svc = cli->getService(BLEUUID(MESH_SVC));
  if (!svc) { errMsg = "no mesh service (BLE off on node?)"; cli->disconnect(); return false; }
  chToRadio   = svc->getCharacteristic(BLEUUID(UUID_TORAD));
  chFromRadio = svc->getCharacteristic(BLEUUID(UUID_FROMR));
  chFromNum   = svc->getCharacteristic(BLEUUID(UUID_FROMN));
  if (!chToRadio || !chFromRadio) { errMsg = "missing characteristics"; cli->disconnect(); return false; }

  if (chFromNum && chFromNum->canNotify()) chFromNum->registerForNotify(fromNumCB);

  // Send a config request. The payload contains the config ID.
  uint8_t hs[3] = { (3 << 3) | 0, 0x2a };
  chToRadio->writeValue(hs, 2, false);

  // Wait for the link to establish. Check for authentication failures. A silent timeout usually means the phone app holds the connection slot.
  dataWaiting = true;
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) {
      *cancelled = true;
      uiWaitForRelease();
      cli->disconnect();
      return false;
    }
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
static Btn tabNodes, tabFeed, tabAlerts;
static int view = 0;   // 0 = nodes, 1 = feed, 2 = alerts (channel/code settings)
static Btn alertChanBtn, alertCodeBtn;   // view == 2 only; set in drawBody()

// Skip tab redraws when the view does not change. The main loop calls the draw function many times during data bursts. This check prevents screen flicker. Reset the view tracker to minus one when a modal covers the tabs. This forces a fresh redraw.
static int lastTabsView = -1;

static void drawTabs() {
  if (view == lastTabsView) return;
  lastTabsView = view;
  uiClearRect(0, TABS_Y, tft.width(), TABS_H);
  int w3 = tft.width() / 3;
  tabNodes  = {0,      TABS_Y, w3, TABS_H, "Nodes"};
  tabFeed   = {w3,     TABS_Y, w3, TABS_H, "Feed"};
  tabAlerts = {2 * w3, TABS_Y, tft.width() - 2 * w3, TABS_H, "Alerts"};
  // Active tab in the theme's normal button color, the other two dimmed --
  // same active/inactive treatment uiDrawPager()'s prev/next buttons
  // already use for "not available right now", so tabs read as tabs
  // instead of three same-color buttons distinguished only by the
  // underline below.
  (view == 0 ? uiDrawButton : uiDrawButtonDim)(tabNodes);
  (view == 1 ? uiDrawButton : uiDrawButtonDim)(tabFeed);
  (view == 2 ? uiDrawButton : uiDrawButtonDim)(tabAlerts);
  Btn &cur = (view == 0) ? tabNodes : (view == 1) ? tabFeed : tabAlerts;
  tft.fillRect(cur.x + 2, TABS_Y + TABS_H - 4, cur.w - 4, 3, accentFill());
}

// Track the last drawn screen state. The layout shifts when the link status changes. The content model differs between tabs. The reset function clears this state. The diffed redraw always starts from a clean layout.
static char prevRow[40][UI_LIST_SIG_LEN];
static char prevHeader[64] = "";
static char prevEmpty[48] = "";
static bool lastLinkUp = false;
static int lastBodyView = -1;
static const int MESH_ROW_H = 12;

static void forceMeshBodyReset() {
  uiClearBelow(BODY_Y);
  memset(prevRow, 0, sizeof(prevRow));
  prevHeader[0] = '\0';
  prevEmpty[0] = '\0';
  lastLinkUp = linkUp;
  lastBodyView = view;
}

static void drawBody() {
  tft.setTextSize(1);
  tft.setTextWrap(false);

  if (linkUp != lastLinkUp || view != lastBodyView) forceMeshBodyReset();

  if (!linkUp) {
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(4, BODY_Y + 6);
    tft.print("link lost");
  }

  int y0 = BODY_Y + (linkUp ? 4 : 18);
  if (view == 0) {
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "my !%06x   nodes: %d", (unsigned)(myNum & 0xFFFFFF), nodeN);
    if (uiFieldChanged(prevHeader, sizeof(prevHeader), hdr)) {
      uiClearRect(0, y0, tft.width(), MESH_ROW_H);
      tft.setTextColor(accentLabel());
      tft.setCursor(4, y0);
      tft.print(hdr);
    }
    // Just a count + last-heard summary -- the per-node row list (name/
    // SNR/hops/battery/age columns) was dropped; this use case doesn't
    // need it, and it's not worth the screen space for it on this display.
    uint32_t lastSeen = 0;
    for (int i = 0; i < nodeN; i++) if (nodes[i].seen > lastSeen) lastSeen = nodes[i].seen;
    char line2[48];
    if (lastSeen) {
      uint32_t ago = (millis() - lastSeen) / 1000;
      snprintf(line2, sizeof(line2), "last heard: %luh%02lum%02lus ago",
               (unsigned long)(ago / 3600), (unsigned long)((ago / 60) % 60), (unsigned long)(ago % 60));
    } else {
      snprintf(line2, sizeof(line2), "last heard: --");
    }
    if (uiFieldChanged(prevEmpty, sizeof(prevEmpty), line2)) {
      uiClearRect(0, y0 + 14, tft.width(), MESH_ROW_H);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(4, y0 + 14);
      tft.print(line2);
    }
  } else if (view == 1) {
    int maxVisible = (tft.height() - 2 - y0) / MESH_ROW_H;
    if (maxVisible > 40) maxVisible = 40;
    if (maxVisible < 0) maxVisible = 0;
    int shown = min(feedCount, maxVisible);
    if (shown == 0) {
      uiDrawListIfChanged(0, y0, tft.width(), MESH_ROW_H, 0, maxVisible, prevRow,
        [](int, char *, size_t) {}, [](int) {});   // just clears any rows left from before
      if (uiFieldChanged(prevEmpty, sizeof(prevEmpty), "empty")) {
        uiClearRect(4, y0, tft.width() - 4, MESH_ROW_H);
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(4, y0);
        tft.print("waiting for mesh traffic...");
      }
    } else {
      prevEmpty[0] = '\0';   // forget it so the message reprints if the feed empties out again later
      uiDrawListIfChanged(0, y0, tft.width(), MESH_ROW_H, shown, maxVisible, prevRow,
        [](int k, char *sig, size_t cap) {
          int idx = (feedHead - 1 - k + FEED_MAX * 2) % FEED_MAX;
          snprintf(sig, cap, "%s", feed[idx]);
        },
        [&](int k) {
          int idx = (feedHead - 1 - k + FEED_MAX * 2) % FEED_MAX;
          tft.setTextColor(ILI9341_WHITE);
          tft.setCursor(4, y0 + k * MESH_ROW_H);
          tft.print(feed[idx]);
        });
    }
  } else {   // view == 2: alert-channel settings
    int ch = meshAlertChannelGet();
    const char *code = meshAlertCodeGet();
    char sig[64];
    // channelName[ch] is included so a name that arrives (or changes)
    // after this row already drew with just the bare number still
    // triggers a redraw to pick it up.
    snprintf(sig, sizeof(sig), "%d|%s|%d", ch, (ch >= 0 ? channelName[ch] : ""), code[0] != 0);
    if (uiFieldChanged(prevHeader, sizeof(prevHeader), sig)) {
      char chanLbl[32], codeLbl[24];
      if (ch < 0) snprintf(chanLbl, sizeof(chanLbl), "Channel: not set");
      else if (channelName[ch][0]) snprintf(chanLbl, sizeof(chanLbl), "Channel: %d (%s)", ch, channelName[ch]);
      else        snprintf(chanLbl, sizeof(chanLbl), "Channel: %d", ch);
      snprintf(codeLbl, sizeof(codeLbl), code[0] ? "Code: set" : "Code: not set");
      alertChanBtn = {8, y0 + 4, tft.width() - 16, 34, chanLbl};
      alertCodeBtn = {8, y0 + 46, tft.width() - 16, 34, codeLbl};
      uiDrawButton(alertChanBtn);
      uiDrawButton(alertCodeBtn);
      tft.setTextColor(ILI9341_DARKGREY);
      tft.setCursor(8, y0 + 92);
      tft.print("status needs no code;");
      tft.setCursor(8, y0 + 104);
      tft.print("reboot / ids / guardian do");
    }
  }
}

// ---------------- scan / pick ----------------
// Collect mesh advertisers during the scan. The main task reads the count while scanning. It reads the candidate array only after stopping the scan.
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
  uiClearRect(0, 30, tft.width(), 44);
  tft.setTextSize(1);
  tft.setTextColor(accentLabel());
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
    // tft.height() - 28 used to put the bottom edge at height-4, inside
    // the 18px status bar -- this clears it with a 4px gap above instead.
    forgetBtn = {6, tft.height() - UI_STATUSBAR_H - 4 - 24, tft.width() - 12, 24, "forget BLE bond"};
    uiDrawMenuButton(forgetBtn);
    return;
  }
  tft.setTextColor(accentLabel());
  tft.setCursor(6, 34);
  tft.print("Pick a node:");
  int y = 50;
  for (int i = 0; i < candN && y + 26 <= tft.height() - 34; i++) {
    Btn b = {6, y, tft.width() - 12, 26, cand[i].name.c_str()};
    uiDrawMenuButton(b);
    y += 30;
  }
  forgetBtn = {6, tft.height() - UI_STATUSBAR_H - 4 - 24, tft.width() - 12, 24, "forget BLE bond"};
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
        bool bonded = meshIsBonded(cand[candSel].addr);
        if (!bonded) {   // only a fresh pairing actually needs the passkey
          String pin = uiTextInput("Node BLE PIN", "123456", false);
          s_pin = (uint32_t)pin.toInt();
        }
        uiDrawTopBar("Meshtastic");
        uiShowLoading(bonded ? "Reconnecting..." : "Pairing / connecting...");
        st = MS_CONNECT;
        bool cancelled = false;
        if (connectTo(cand[candSel].addr, &cancelled)) {
          st = MS_LIVE;
          view = 0;
          lastBodyView = -1;   // force drawBody() to treat this as a fresh body, not a stale re-entry
          lastTabsView = -1;   // ditto for drawTabs() -- view==0 may already match a stale prior session
          uiDrawTopBar("Meshtastic");
          drawTabs();
          drawBody();
        } else if (cancelled) {
          // Back-button tap during the connect/pairing wait -- straight
          // back to the candidate list, not the MS_ERR screen (nothing
          // actually failed, the user just backed out).
          st = MS_PICK;
          teardown();
          uiDrawTopBar("Meshtastic");
          drawPick();
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
    else if (uiTouchInButton(t, tabAlerts)) { view = 2; drawTabs(); drawBody(); uiWaitForRelease(); }
    else if (view == 2 && uiTouchInButton(t, alertChanBtn)) {
      uiWaitForRelease();
      int chosen = uiDropdownPick("Alert channel", 8, alertChanLabel, meshAlertChannelGet());
      meshAlertSetChannel(chosen);
      // uiDropdownPick() just repainted the whole screen for its own UI --
      // forceMeshBodyReset() re-wipes the content area and forgets every
      // diffed field (not just this tab's) so nothing below thinks stale
      // pixels from that modal are still its own last-drawn state; view
      // isn't changing here, so drawTabs() needs the same forced-redraw
      // nudge or its own diff would wrongly skip it.
      uiDrawTopBar("Meshtastic");
      lastTabsView = -1;
      drawTabs();
      forceMeshBodyReset();
      drawBody();
    } else if (view == 2 && uiTouchInButton(t, alertCodeBtn)) {
      uiWaitForRelease();
      String code = uiTextInput("Alert code", meshAlertCodeGet(), false);
      meshAlertSetCode(code.c_str());
      uiDrawTopBar("Meshtastic");
      lastTabsView = -1;
      drawTabs();
      forceMeshBodyReset();
      drawBody();
    }
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
