// Probe-request watch (Marauder "sniffprobe" / Recon probe logging). Passive:
// registers a PROBE_REQ consumer on the shared wifi_ids promiscuous core and
// lists the SSIDs nearby clients are actively asking for, with how many
// distinct devices asked and the strongest RSSI seen. A directed probe
// request names a network the device has joined before -- it leaks that
// device's preferred-network list, and it is the surface a KARMA / "answer
// every SSID" rogue AP feeds on.
//
// Also tracks BEACON SSIDs seen in the same hop sweep, and flags any probed
// SSID that has no matching beacon nearby: the device is announcing a known
// network that is not actually present here -- either out of range, powered
// off, or a hidden (non-broadcasting) AP the device still remembers. That is
// the strongest form of the PNL leak, and it is exactly the gap a rogue AP
// answering "every SSID" (KARMA) is built to fill.
//
// A cloaked AP sends beacons with a blank SSID, but still answers a client's
// DIRECTED probe with a normal PROBE_RESP that carries its real SSID in the
// clear -- cloaking hides the name from a casual scan, not from a device
// that already knows it. This module records which BSSIDs are currently
// beaconing hidden, then watches probe responses from those same BSSIDs to
// unmask the SSID they carry. An unmasked hidden AP also counts as
// "broadcasting nearby" for the ABSENT check above, and is called out with
// its own tag ('H') so a hidden hit reads differently from an ordinary one.
//
// PASSIVE ONLY -- wifi_ids receives; nothing here transmits.
#include <string.h>
#include "ui.h"
#include "wifi_ids.h"
#include "accent.h"

struct Probe {
  char     ssid[24];
  uint8_t  devMacs[4][6];
  uint8_t  devN;
  int8_t   rssi;         // strongest seen
  uint16_t hits;
  uint32_t seen;
};
static const int PW_N = 16;
static Probe pw[PW_N];
static uint32_t gReqs, gBroadcast;
static int handle = -1;
static int beaconHandle = -1;
static int probeRespHandle = -1;
static bool running = false;
static uint32_t lastDraw;
static bool dirty = true;

// SSIDs seen in beacons (or unmasked via a probe response, see below) during
// this session, so a probed SSID can be marked "not broadcasting nearby"
// when it has no recent match here.
struct BeaconSsid {
  char     ssid[24];
  uint32_t seen;
  bool     hidden;   // learned by unmasking a cloaked AP's probe response, not its beacon
};
static const int BC_N = 24;
static const uint32_t BC_STALE_MS = 15000;  // beacons repeat ~10x/sec typ.; 15s covers a full hop cycle
static BeaconSsid bc[BC_N];

// BSSIDs currently seen beaconing with a blank (cloaked) SSID. A probe
// response from one of these, naming the real SSID, is an unmask.
struct HiddenBssid {
  uint8_t  bssid[6];
  uint32_t seen;
};
static const int HB_N = 8;
static HiddenBssid hb[HB_N];

static BeaconSsid *bcFind(const char *s) {
  for (int i = 0; i < BC_N; i++)
    if (bc[i].seen && strcmp(bc[i].ssid, s) == 0) return &bc[i];
  return nullptr;
}
static BeaconSsid *bcSlot() {
  int lru = 0;
  for (int i = 1; i < BC_N; i++) {
    if (!bc[i].seen) return &bc[i];
    if (bc[i].seen < bc[lru].seen) lru = i;
  }
  return &bc[lru];
}
// Record `s` as currently present. `viaHidden` marks it as learned by
// unmasking a cloaked AP rather than from an ordinary open beacon; once an
// SSID is flagged hidden it stays flagged (a later ordinary sighting does
// not erase the "this AP cloaks" fact).
static void bcRecord(const char *s, bool viaHidden) {
  BeaconSsid *b = bcFind(s);
  if (!b) { b = bcSlot(); memset(b, 0, sizeof(*b)); strncpy(b->ssid, s, 23); }
  b->seen = millis();
  b->hidden = b->hidden || viaHidden;
}
static bool broadcastingNearby(const char *s) {
  BeaconSsid *b = bcFind(s);
  return b && (millis() - b->seen) < BC_STALE_MS;
}
// nullptr if not present at all; else true/false for whether the presence
// came from unmasking a cloaked AP.
static const BeaconSsid *presenceInfo(const char *s) {
  BeaconSsid *b = bcFind(s);
  return (b && (millis() - b->seen) < BC_STALE_MS) ? b : nullptr;
}

static HiddenBssid *hbFind(const uint8_t *bssid) {
  for (int i = 0; i < HB_N; i++)
    if (hb[i].seen && !memcmp(hb[i].bssid, bssid, 6)) return &hb[i];
  return nullptr;
}
static HiddenBssid *hbSlot() {
  int lru = 0;
  for (int i = 1; i < HB_N; i++) {
    if (!hb[i].seen) return &hb[i];
    if (hb[i].seen < hb[lru].seen) lru = i;
  }
  return &hb[lru];
}

static Probe *pwFind(const char *s) {
  for (int i = 0; i < PW_N; i++)
    if (pw[i].hits && strcmp(pw[i].ssid, s) == 0) return &pw[i];
  return nullptr;
}
static Probe *pwSlot() {
  int lru = 0;
  for (int i = 1; i < PW_N; i++) {
    if (!pw[i].hits) return &pw[i];
    if (pw[i].seen < pw[lru].seen) lru = i;
  }
  return &pw[lru];
}

static void onProbeReq(const WifiIdsFrame &f, void *) {
  gReqs++;
  if (f.rawLen < 26) return;
  const uint8_t *ie = f.raw + 24;              // IE list starts right after the 24-byte header
  if (ie[0] != 0) return;                      // first IE must be SSID
  uint8_t l = ie[1];
  if (l == 0 || 26 + l > f.rawLen) { gBroadcast++; return; }   // wildcard probe -- not tracked
  if (l > 23) l = 23;

  char s[24];
  memcpy(s, ie + 2, l); s[l] = 0;
  for (int i = 0; i < l; i++) if (s[i] < 0x20 || s[i] > 0x7E) s[i] = '.';

  Probe *p = pwFind(s);
  if (!p) { p = pwSlot(); memset(p, 0, sizeof(*p)); strncpy(p->ssid, s, 23); p->rssi = -127; }
  p->hits++;
  p->seen = millis();
  if (f.rssi > p->rssi) p->rssi = f.rssi;
  bool known = false;
  for (int i = 0; i < p->devN; i++) if (!memcmp(p->devMacs[i], f.addr2, 6)) { known = true; break; }
  if (!known) {
    if (p->devN < 4) memcpy(p->devMacs[p->devN++], f.addr2, 6);
    else {
      memmove(p->devMacs[0], p->devMacs[1], 3 * 6);
      memcpy(p->devMacs[3], f.addr2, 6);
    }
  }
  dirty = true;
}

// Beacon fixed params are 12 bytes (timestamp 8 + interval 2 + capability 2),
// so the IE list starts at raw+24+12 -- unlike the 24-byte header subtypes,
// see the WifiIdsFrame layout note in wifi_ids.h.
static void onBeacon(const WifiIdsFrame &f, void *) {
  if (f.rawLen < 38) return;
  const uint8_t *ie = f.raw + 36;
  if (ie[0] != 0) return;                       // first IE must be SSID
  uint8_t l = ie[1];
  if (l == 0 || 38 + l > f.rawLen) {             // hidden (blank) SSID -- remember the BSSID
    HiddenBssid *hbb = hbFind(f.addr3);
    if (!hbb) { hbb = hbSlot(); memset(hbb, 0, sizeof(*hbb)); memcpy(hbb->bssid, f.addr3, 6); }
    hbb->seen = millis();
    return;
  }
  if (l > 23) l = 23;

  char s[24];
  memcpy(s, ie + 2, l); s[l] = 0;
  for (int i = 0; i < l; i++) if (s[i] < 0x20 || s[i] > 0x7E) s[i] = '.';

  bcRecord(s, false);
  dirty = true;
}

// A cloaked AP still answers a directed probe with its real SSID -- correlate
// against the hidden-BSSID set gathered from beacons above to confirm this is
// an unmask, not just an ordinary AP being asked its name.
static void onProbeResp(const WifiIdsFrame &f, void *) {
  if (f.rawLen < 38) return;
  const uint8_t *ie = f.raw + 36;
  if (ie[0] != 0) return;                       // first IE must be SSID
  uint8_t l = ie[1];
  if (l == 0 || 38 + l > f.rawLen) return;       // no SSID in this response -- nothing to unmask
  if (l > 23) l = 23;

  char s[24];
  memcpy(s, ie + 2, l); s[l] = 0;
  for (int i = 0; i < l; i++) if (s[i] < 0x20 || s[i] > 0x7E) s[i] = '.';

  HiddenBssid *hbb = hbFind(f.addr3);
  bool wasHidden = hbb && (millis() - hbb->seen) < BC_STALE_MS;
  bcRecord(s, wasHidden);
  dirty = true;
}

static void draw() {
  dirty = false;
  uiClearBelow(UI_CONTENT_Y_PLAIN);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(2, UI_CONTENT_Y_PLAIN);
  tft.printf("ch%2d  %lu req  %lu wildcard", wifiIdsChannel(),
             (unsigned long)gReqs, (unsigned long)gBroadcast);

  // recency order
  int idx[PW_N], n = 0;
  for (int i = 0; i < PW_N; i++) if (pw[i].hits) idx[n++] = i;
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      if (pw[idx[b]].seen > pw[idx[a]].seen) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }

  int y = UI_CONTENT_Y_PLAIN + 14;
  if (n == 0) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(4, y + 4);
    tft.print("no directed probe requests yet");
    return;
  }
  for (int k = 0; k < n && y + 12 <= tft.height() - UI_STATUSBAR_H; k++) {
    const Probe &p = pw[idx[k]];
    bool fresh = millis() - p.seen < 8000;
    const BeaconSsid *b = presenceInfo(p.ssid);
    char flag = !b ? '!' : (b->hidden ? 'H' : ' ');
    const char *tag = !b ? " ABSENT" : (b->hidden ? " HIDDEN-AP" : "");
    tft.setTextColor(!b ? ILI9341_RED : (b->hidden ? ILI9341_MAGENTA : (fresh ? ILI9341_YELLOW : accentLabel())));
    tft.setCursor(4, y);
    tft.printf("%c%-15.15s %dd %ddBm x%u%s", flag, p.ssid, p.devN, p.rssi, p.hits, tag);
    y += 12;
  }
}

void probeWatchEnter() {
  uiDrawTopBar("Probe Watch");
  memset(pw, 0, sizeof pw);
  memset(bc, 0, sizeof bc);
  memset(hb, 0, sizeof hb);
  gReqs = gBroadcast = 0;
  dirty = true;
  wifiIdsBegin();
  wifiIdsSetDwell(250);
  handle = wifiIdsRegister(&onProbeReq, nullptr, WIDS_BIT(WIDS_PROBE_REQ));
  beaconHandle = wifiIdsRegister(&onBeacon, nullptr, WIDS_BIT(WIDS_BEACON));
  probeRespHandle = wifiIdsRegister(&onProbeResp, nullptr, WIDS_BIT(WIDS_PROBE_RESP));
  running = true;
  lastDraw = 0;
  draw();
}

void probeWatchLoop() {
  if (!running) return;
  wifiIdsLoop();
  uint32_t now = millis();
  if ((dirty && now - lastDraw > 1200) || now - lastDraw > 4000) { draw(); lastDraw = now; }
}

void probeWatchTouch(const TouchPoint &t) {
  if (!t.isNewPress || t.y < UI_CONTENT_Y_PLAIN) return;
  memset(pw, 0, sizeof pw);
  memset(bc, 0, sizeof bc);
  memset(hb, 0, sizeof hb);
  gReqs = gBroadcast = 0;
  draw();
  uiWaitForRelease();
}

void probeWatchExit() {
  if (!running) return;
  running = false;
  wifiIdsUnregister(handle);
  handle = -1;
  wifiIdsUnregister(beaconHandle);
  beaconHandle = -1;
  wifiIdsUnregister(probeRespHandle);
  probeRespHandle = -1;
  wifiIdsEnd();
}
