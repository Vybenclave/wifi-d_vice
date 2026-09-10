// Probe-request watch (Marauder "sniffprobe" / Recon probe logging). Passive:
// registers a PROBE_REQ consumer on the shared wifi_ids promiscuous core and
// lists the SSIDs nearby clients are actively asking for, with how many
// distinct devices asked and the strongest RSSI seen. A directed probe
// request names a network the device has joined before -- it leaks that
// device's preferred-network list, and it is the surface a KARMA / "answer
// every SSID" rogue AP feeds on.
//
// PASSIVE ONLY -- wifi_ids receives; nothing here transmits.
#include <string.h>
#include "ui.h"
#include "wifi_ids.h"

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
static bool running = false;
static uint32_t lastDraw;
static bool dirty = true;

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
    tft.setTextColor(fresh ? ILI9341_YELLOW : ILI9341_CYAN);
    tft.setCursor(4, y);
    tft.printf("%-20.20s %dd %ddBm x%u", p.ssid, p.devN, p.rssi, p.hits);
    y += 12;
  }
}

void probeWatchEnter() {
  uiDrawTopBar("Probe Watch");
  memset(pw, 0, sizeof pw);
  gReqs = gBroadcast = 0;
  dirty = true;
  wifiIdsBegin();
  wifiIdsSetDwell(250);
  handle = wifiIdsRegister(&onProbeReq, nullptr, WIDS_BIT(WIDS_PROBE_REQ));
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
  gReqs = gBroadcast = 0;
  draw();
  uiWaitForRelease();
}

void probeWatchExit() {
  if (!running) return;
  running = false;
  wifiIdsUnregister(handle);
  handle = -1;
  wifiIdsEnd();
}
