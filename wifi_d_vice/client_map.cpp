// Client / station map (Marauder "scansta" / Wireless Wizard "Wizard's
// Discovery"). Passive: registers probe-request / (re)assoc-request / auth
// consumers on the shared wifi_ids core and builds a table of the client
// devices in the air -- their MAC (flagged when randomized), the vendor from
// the OUI, the AP they were last seen talking to, RSSI and a frame count.
//
// Uses management frames only (the wifi_ids core is MGMT-filtered): a STA
// leaks its address in every probe request and every association / auth
// attempt, so no data-frame capture is needed for a useful station list.
//
// PASSIVE ONLY -- receive and count; nothing here transmits.
#include <string.h>
#include "ui.h"
#include "wifi_ids.h"
#include "mac_vendor.h"
#include "accent.h"

struct Sta {
  uint8_t  mac[6];
  uint8_t  ap[6];        // last AP addr1 from an assoc/auth (0 = none seen)
  int8_t   rssi;
  uint16_t frames;
  uint32_t seen;
  bool     rand;         // locally-administered (randomized) MAC
};
static const int ST_N = 32;
static Sta st[ST_N];
static uint32_t gFrames;
static int hProbe = -1, hAssoc = -1, hAuth = -1;
static bool running = false;
static uint32_t lastDraw;
static bool dirty = true;

static Sta *stFind(const uint8_t *m) {
  for (int i = 0; i < ST_N; i++)
    if (st[i].frames && !memcmp(st[i].mac, m, 6)) return &st[i];
  return nullptr;
}
static Sta *stSlot() {
  int lru = 0;
  for (int i = 1; i < ST_N; i++) {
    if (!st[i].frames) return &st[i];
    if (st[i].seen < st[lru].seen) lru = i;
  }
  return &st[lru];
}

static void note(const uint8_t *sta, const uint8_t *ap, int8_t rssi) {
  // Ignore group / broadcast source addresses (bit0 of first byte) -- a STA
  // source is always an individual address.
  if (sta[0] & 0x01) return;
  gFrames++;
  Sta *s = stFind(sta);
  if (!s) { s = stSlot(); memset(s, 0, sizeof(*s)); memcpy(s->mac, sta, 6); s->rand = (sta[0] & 0x02); }
  s->frames++;
  s->rssi = rssi;
  s->seen = millis();
  if (ap && !(ap[0] & 0x01) && memcmp(ap, "\0\0\0\0\0\0", 6)) memcpy(s->ap, ap, 6);
  dirty = true;
}

static void onProbe(const WifiIdsFrame &f, void *) { note(f.addr2, nullptr, f.rssi); }
static void onAssoc(const WifiIdsFrame &f, void *) { note(f.addr2, f.addr1, f.rssi); }
static void onAuth (const WifiIdsFrame &f, void *) { note(f.addr2, f.addr1, f.rssi); }

// "What's currently drawn" signatures for the header line, the row list and
// the empty-state message -- see uiDrawListIfChanged()/uiFieldChanged() in
// ui.h. clientMapEnter() does a one-time uiClearBelow() + resets these (see
// below); draw() itself only touches what actually changed.
static char prevRow[ST_N][UI_LIST_SIG_LEN];
static char prevHeader[48] = "";
static char prevEmpty[40] = "";
static const int ROW_H = 12;

static void draw() {
  dirty = false;
  tft.setTextSize(1);

  int n = 0;
  for (int i = 0; i < ST_N; i++) if (st[i].frames) n++;
  char hdr[48];
  snprintf(hdr, sizeof(hdr), "ch%2d  %d stations  %lu frames", wifiIdsChannel(), n, (unsigned long)gFrames);
  if (uiFieldChanged(prevHeader, sizeof(prevHeader), hdr)) {
    uiClearRect(0, UI_CONTENT_Y_PLAIN, tft.width(), ROW_H);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(2, UI_CONTENT_Y_PLAIN);
    tft.print(hdr);
  }

  int idx[ST_N], m = 0;
  for (int i = 0; i < ST_N; i++) if (st[i].frames) idx[m++] = i;
  for (int a = 0; a < m; a++)
    for (int b = a + 1; b < m; b++)
      if (st[idx[b]].frames > st[idx[a]].frames) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }

  int y0 = UI_CONTENT_Y_PLAIN + 14;
  if (m == 0) {
    uiDrawListIfChanged(0, y0, tft.width(), ROW_H, 0, ST_N, prevRow,
      [](int, char *, size_t) {}, [](int) {});   // just clears any rows left from before
    if (uiFieldChanged(prevEmpty, sizeof(prevEmpty), "empty")) {
      uiClearRect(4, y0 + 4, tft.width() - 4, ROW_H);
      tft.setTextColor(ILI9341_GREEN);
      tft.setCursor(4, y0 + 4);
      tft.print("no client devices seen yet");
    }
    return;
  }
  prevEmpty[0] = '\0';   // forget it so the message reprints if the list empties out again later

  int maxVisible = (tft.height() - UI_STATUSBAR_H - y0) / ROW_H;
  if (maxVisible > ST_N) maxVisible = ST_N;
  if (maxVisible < 0) maxVisible = 0;
  int shown = min(m, maxVisible);

  uiDrawListIfChanged(0, y0, tft.width(), ROW_H, shown, maxVisible, prevRow,
    [&](int k, char *sig, size_t cap) {
      const Sta &s = st[idx[k]];
      bool fresh = millis() - s.seen < 8000;
      bool haveAp = memcmp(s.ap, "\0\0\0\0\0\0", 6) != 0;
      snprintf(sig, cap, "%02X%02X%02X|%d|%d|%c%02X%02X|%d|%u",
               s.mac[3], s.mac[4], s.mac[5], s.rand, fresh,
               haveAp ? '>' : ' ', s.ap[4], s.ap[5], s.rssi, s.frames);
    },
    [&](int k) {
      const Sta &s = st[idx[k]];
      bool fresh = millis() - s.seen < 8000;
      bool haveAp = memcmp(s.ap, "\0\0\0\0\0\0", 6) != 0;
      tft.setTextColor(s.rand ? ILI9341_DARKGREY : (fresh ? ILI9341_YELLOW : accentLabel()));
      tft.setCursor(4, y0 + k * ROW_H);
      tft.printf("%02X%02X%02X %-14.14s %c%02X%02X %ddBm x%u",
                 s.mac[3], s.mac[4], s.mac[5],
                 s.rand ? "(random)" : macVendorTag(s.mac).c_str(),
                 haveAp ? '>' : ' ', s.ap[4], s.ap[5], s.rssi, s.frames);
    });
}

void clientMapEnter() {
  uiDrawTopBar("Client Map");
  memset(st, 0, sizeof st);
  gFrames = 0;
  dirty = true;
  uiClearBelow(UI_CONTENT_Y_PLAIN);   // wipe whatever the previous screen left here
  memset(prevRow, 0, sizeof(prevRow));
  prevHeader[0] = '\0';
  prevEmpty[0] = '\0';
  wifiIdsBegin();
  wifiIdsSetDwell(250);
  hProbe = wifiIdsRegister(&onProbe, nullptr, WIDS_BIT(WIDS_PROBE_REQ));
  hAssoc = wifiIdsRegister(&onAssoc, nullptr, WIDS_BIT(WIDS_ASSOC_REQ) | WIDS_BIT(WIDS_REASSOC_REQ));
  hAuth  = wifiIdsRegister(&onAuth,  nullptr, WIDS_BIT(WIDS_AUTH));
  running = true;
  lastDraw = 0;
  draw();
}

void clientMapLoop() {
  if (!running) return;
  wifiIdsLoop();
  uint32_t now = millis();
  if ((dirty && now - lastDraw > 1200) || now - lastDraw > 4000) { draw(); lastDraw = now; }
}

void clientMapTouch(const TouchPoint &t) {
  if (!t.isNewPress || t.y < UI_CONTENT_Y_PLAIN) return;
  memset(st, 0, sizeof st);
  gFrames = 0;
  draw();
  uiWaitForRelease();
}

void clientMapExit() {
  if (!running) return;
  running = false;
  wifiIdsUnregister(hProbe);
  wifiIdsUnregister(hAssoc);
  wifiIdsUnregister(hAuth);
  hProbe = hAssoc = hAuth = -1;
  wifiIdsEnd();
}
