// Meshtastic mesh monitor -- BLE central link to a node's phone API.
//
// Flow: scan for advertisers of the mesh service (or name "Meshtastic*"),
// pick one, enter its BLE PIN, bond, then write ToRadio{want_config_id}
// and drain FromRadio. A tiny hand-rolled protobuf walker pulls out the
// bits we show: my node num, NodeInfo (num/names/snr/hops/last-heard),
// and MeshPacket payloads for TEXT_MESSAGE_APP (1) and TELEMETRY_APP (67).
// Nothing is written onto the mesh -- receive/display only.
#include "meshtastic_mon.h"
#include "accent.h"
#include "keyboard.h"
#include "devtime.h"
#include "wifi_ids.h"
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
// snr/hops/batt/volt were dropped -- their only reader was the per-node
// row list the Nodes tab used to show (replaced with a count + last-heard
// summary; see drawBody()'s view == 0 branch).
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

// Channel names for the Alerts-tab picker (FromRadio.channel, field 10 --
// one Channel message per configured slot, sent during the config dump
// same as NodeInfo). ChannelSettings.name is documented "Less than 12
// bytes"; empty means the node treats that slot as the unnamed "Default"
// channel (per channel.proto's own comment), so this stays blank and the
// picker just shows the bare index for it, same as before this existed.
static char channelName[8][13];

static const int FEED_MAX = 36;
static char feed[FEED_MAX][46];
static int  feedHead = 0, feedCount = 0;

// ------- alert-channel commands (Guardian pre-work) -------
// A dedicated Meshtastic channel this device listens to for a handful of
// remote commands. Meshtastic channels are PSK-encrypted (except the
// unencrypted default/primary channel), so picking a dedicated,
// PSK-protected channel here is real access control on its own --
// "status" relies on just that. "reboot"/"ids on|off"/"guardian on|off"
// additionally require a configurable code as a second, explicit gate
// (the user asked for this specifically) since those have real-world
// effects (a remote reboot; disarming detection right before an attack).
static int8_t s_alertChan = -1;        // -1 = no alert channel configured
static char   s_alertCode[24] = "";    // "" = no code set -- gated commands always refuse
static bool   s_idsArmed = false;      // background WiFi IDS raw-capture ref held via a command
static bool   s_guardianArmed = false; // placeholder flag only -- see meshSetGuardianArmed()

// Declared in screens.h -- called once from setup() (see wifi_d_vice.ino),
// same spot/reasoning as themeLoad()/accentLoad()/tzLoad()/modvisLoad():
// must run before the boot-time meshIdsArmed() check right after it.
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
// Declared in screens.h -- wifi_d_vice.ino's setup()/loop() read this to
// restore and service background WiFi IDS capture independent of whichever
// screen is active; the mesh BLE link itself stays screen-bound (meshEnter()/
// meshExit()), but an armed background capture outlives leaving this screen
// (and a "<code> reboot") because this flag -- not the BLE link -- is what
// loop() checks.
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
// Holds/drops a background ref on wifi_ids.cpp's promiscuous sniffer via
// its existing ref-counted wifiIdsBegin()/wifiIdsEnd() -- the same seam
// wifi_ids.h documents as built for "a screen and (later) Guardian [to]
// each hold a reference without stepping on each other". Raw capture only:
// the actual deauth/beacon/karma/rogue-AP detectors and alerting live
// entirely in wifi_ids_screen.cpp, tied to that screen's own lifecycle, so
// they still only run while that screen is open -- this just keeps frames
// flowing into the ring in the meantime. Closing that gap for real is
// Guardian work, not this.
static void meshSetIdsArmed(bool on) {
  if (on == s_idsArmed) return;
  s_idsArmed = on;
  Preferences p; p.begin("meshalert", false); p.putBool("idsarm", on); p.end();
  if (on) wifiIdsBegin(); else wifiIdsEnd();
}
// Pure placeholder -- no monitor is wired up to this yet. This flag (and
// the "guardian on/off" command surface) is the hook point DESIGN.md's
// Guardian mode (a unified multi-monitor alert framework) will replace
// with real behavior later.
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

// Channel{ index=1 (int32, varint); settings=2 (ChannelSettings, len-delim);
// role=3 (enum, varint) } -- per channel.proto. Only settings.name (field 3
// inside ChannelSettings) is wanted here; everything else (psk, role, ...)
// is skipped.
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
// Hand-rolled protobuf ENCODER, mirroring the Pb decoder's style above --
// until now this file only ever sent the fixed want_config_id handshake
// (connectTo()), nothing else. Submessages are length-prefixed, so this
// builds innermost-first: Data -> MeshPacket -> ToRadio, each written into
// its own small stack buffer before being embedded (as bytes) in the next.
// Data.portnum=1/payload=2 are the same field numbers the decoder above
// already proves correct on real traffic; MeshPacket.to/channel/hop_limit
// are standard mesh.proto fields the decoder never needed to read, so
// they're NOT yet proven against this device -- verify a reply actually
// lands in the Meshtastic app before trusting this on a node running a
// materially different firmware version.
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

// "status" needs no code (see the access-control note on s_alertChan
// above); reboot/ids/guardian do -- the user asked for this split
// specifically, since those have real effects a stray/garbled message on
// an otherwise-open channel shouldn't be able to trigger.
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

// Whether this device already holds a BLE bond for `addr` -- a passkey is
// only ever used during INITIAL pairing; reconnecting to an already-bonded
// device resumes encryption from the stored key and never calls
// onPassKeyRequest() at all, so prompting for one every single connect
// attempt (which the MS_PICK tap handler used to do unconditionally) was
// pure unnecessary UX, not something the stack needed.
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

// Each readValue() is a real synchronous over-the-air BLE GATT read/
// response round trip -- a node typically floods its whole known NodeDB
// right after want_config_id, and draining that in one unbounded loop (it
// used to run up to 24 of these back to back) blocked the main task solid
// for several seconds with no touch polling or redraw, which read as a
// hang (and, confirmed on hardware, could even starve the BLE link's
// supervision timeout into a "link lost"). A small batch per call instead,
// with dataWaiting only cleared on a true empty read (actually drained,
// not just batch-capped), keeps a big backlog draining just as completely
// but across many quick loop() iterations instead of one long blocking one
// -- meshLoop()'s own "if (dataWaiting || ...)" trigger calls back in on
// the very next iteration when there's more to read.
// readValue() has NO timeout parameter and can block forever if a GATT
// read's response is ever lost -- confirmed in the vendored BLE library:
// BLERemoteCharacteristic.cpp's m_semaphoreReadCharEvt.wait("readValue")
// is FreeRTOS::Semaphore::wait(), an unconditional
// xSemaphoreTake(m_semaphore, portMAX_DELAY). Same bug class as the
// BLEClient::connect() hang fixed last pass (that one at least had a
// timeoutMs parameter to pass); this one has none at all
// (BLERemoteCharacteristic.h: `String readValue();`). Reducing the read
// batch size (the previous attempt at this) does nothing if a single read
// stalls -- the whole device still hangs on that one call, and a stuck
// link's own supervision timeout firing while we're stuck is consistent
// with the "link lost right after" the user also saw.
//
// There's no library API to cancel a stuck read, so the only way to bound
// this is to run the real call on its own task and stop WAITING on it
// after a timeout -- the task may still be genuinely stuck forever inside
// the library's own wait, and is deliberately abandoned in that case (its
// heap-allocated args block outlives this function on purpose); it
// self-deletes whenever/if the real BLE call ever does resolve, most
// likely when the stack itself tears the stuck operation down on
// disconnect. A leaked ~4KB task+small buffer on the rare timeout path is
// an acceptable trade against a device that otherwise freezes solid
// forever with no recovery.
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

// false = timed out (nothing read this attempt); true = *out is valid
// (possibly empty, meaning the characteristic's queue is actually drained).
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
  // Explicit timeout -- BLEClient::connect()'s default (no 3rd arg) is
  // portMAX_DELAY, i.e. block forever. This call runs synchronously inside
  // the touch handler with no touch polling, so a BLE open event that
  // never arrives (most commonly: the node's one BLE slot is already held
  // by its phone app, same case the error message below already names)
  // used to freeze the whole screen with no way out but a hard reset.
  if (!cli->connect(BLEAddress(addr.c_str()), 0xFF, 8000)) { errMsg = "connect failed"; return false; }

  // Meshtastic's own BLE client-API docs: "highly recommended that you
  // call your phone's setMTU function to increase MTU to 512 bytes" right
  // after connecting -- this codebase never did, leaving every connection
  // at BLE's default 23-byte MTU (20 usable payload bytes). A NodeInfo
  // message (num + User submessage's name fields + SNR/hops) routinely
  // exceeds that, so readValue() -> esp_ble_gattc_read_char() has to fall
  // back to multiple internal ATT "read blob" round trips per packet
  // instead of one -- exactly the kind of multi-step exchange where one
  // lost PDU mid-sequence stalls the whole read waiting on a continuation
  // that never arrives. 517 is the usual max-MTU value other Meshtastic
  // BLE clients request (517 - 5 byte ATT header = the documented 512
  // usable bytes). Fire-and-forget (no blocking wait), so this can't
  // introduce a new hang itself.
  cli->setMTU(517);

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
static Btn tabNodes, tabFeed, tabAlerts;
static int view = 0;   // 0 = nodes, 1 = feed, 2 = alerts (channel/code settings)
static Btn alertChanBtn, alertCodeBtn;   // view == 2 only; set in drawBody()

// Flicker-free gate: drawTabs() is a pure function of `view`, so skip the
// clear+redraw entirely when it hasn't changed since the last draw. With
// the node-list backlog drain now happening in quick back-to-back
// meshLoop() ticks (see drainFromRadio()), meshLoop()'s periodic
// "drainFromRadio(); drawTabs(); drawBody();" call was firing many times
// per second during that burst -- drawTabs() had no diffing of its own
// (unlike drawBody(), which already uses uiFieldChanged()/
// uiDrawListIfChanged() throughout), so the tab bar was being wiped and
// fully repainted every single time even though nothing about it was
// actually changing, which read as flicker. A call site that needs a
// guaranteed fresh redraw even though `view` itself isn't changing (a
// full-screen modal just wiped the tab bar out from under it) resets
// lastTabsView = -1 first -- same pattern forceMeshBodyReset() already
// uses for lastBodyView below.
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

// "What's currently drawn" state for uiDrawListIfChanged()/uiFieldChanged().
// The row baseline shifts when the "link lost" banner appears/disappears,
// and the row content model is entirely different between the Nodes and
// Feed tabs -- forceMeshBodyReset() wipes the body and forgets all of this
// whenever either changes (or the screen goes live fresh), so the diffed
// redraw below always starts clean at the current layout.
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
        if (connectTo(cand[candSel].addr)) {
          st = MS_LIVE;
          view = 0;
          lastBodyView = -1;   // force drawBody() to treat this as a fresh body, not a stale re-entry
          lastTabsView = -1;   // ditto for drawTabs() -- view==0 may already match a stale prior session
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
