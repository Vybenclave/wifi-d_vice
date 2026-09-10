// Passive BLE tracker detector: AirTag / Find My, Samsung SmartTag, Tile,
// Chipolo and generic item-finders. Listen-only (passive scan -- no scan
// requests). Two things it does that a plain BLE scan doesn't:
//
//   * follow-detection -- trackers rotate their MAC (AirTag every ~15min
//     when separated) so you can't lock one address. Instead it tracks
//     each tracker CLASS: how long some beacon of that class has been
//     continuously in range, and how many distinct MACs of it have shown
//     up. A class that's been present for minutes AND is rotating MACs
//     near you gets a << FOLLOW flag.
//   * direction-find -- tap a class to home in: RSSI bar + the shared
//     range-finder chirp on the strongest live advertiser of that class.
//
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <string.h>
#include "ui.h"
#include "wlog.h"

enum { TK_FINDMY = 0, TK_SMARTTAG, TK_TILE, TK_CHIPOLO, TK_UNKNOWN, TK_N };
static const char *KIND_NAME[TK_N] = { "AirTag/FindMy", "SmartTag", "Tile", "Chipolo", "Tracker?" };

struct ClassStat {
  uint32_t firstSeen, lastSeen;
  int      rssiSmooth;
  uint16_t hits;
  bool     separated;         // saw a "lost/separated" Find My adv (Apple type 0x12)
  char     macs[4][18];
  uint8_t  macN;
};
static ClassStat cs[TK_N];

struct Live { char mac[18]; uint8_t kind; int rssi; uint32_t seen; };
static const int LIVE_N = 24;
static Live live[LIVE_N];

static BLEScan *pScan = nullptr;

enum Sub { LIST, LOCATE };
static Sub sub = LIST;
static int locKind = -1;
static Btn rows[TK_N], resetBtn, backRow;
static uint32_t lastListDraw = 0, lastLoc = 0;
static int lastShownRssi = -999;

static const uint32_t GAP_MS    = 90000;    // class is "gone" after this quiet
static const uint32_t FOLLOW_MS = 300000;   // dwell before the FOLLOW flag

static void noteMac(ClassStat &c, const char *mac) {
  for (int i = 0; i < c.macN; i++) if (strcmp(c.macs[i], mac) == 0) return;
  if (c.macN < 4) {
    strncpy(c.macs[c.macN], mac, 17); c.macs[c.macN][17] = 0; c.macN++;
  } else {
    memmove(c.macs[0], c.macs[1], 3 * 18);
    strncpy(c.macs[3], mac, 17); c.macs[3][17] = 0;
  }
}

class Cb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    int  kind = -1;
    bool separated = false;

    if (d.haveManufacturerData()) {
      String md = d.getManufacturerData();
      const uint8_t *m = (const uint8_t *)md.c_str();
      size_t n = md.length();
      if (n >= 4) {
        uint16_t cid = m[0] | (m[1] << 8);
        if (cid == 0x004C) {                       // Apple
          if (m[2] == 0x12) { kind = TK_FINDMY; separated = true; }   // offline / separated
          else if (m[2] == 0x10) kind = TK_FINDMY;                    // nearby (with owner)
        } else if (cid == 0x0075) {                 // Samsung
          kind = TK_SMARTTAG;
        }
      }
    }
    for (int i = 0; i < d.getServiceUUIDCount(); i++) {
      String u = d.getServiceUUID(i).toString(); u.toLowerCase();
      if (u.indexOf("feed") >= 0 || u.indexOf("feec") >= 0) kind = TK_TILE;
      else if (u.indexOf("fd5a") >= 0) kind = TK_SMARTTAG;
    }
    if (d.haveName()) {
      String nl = d.getName().c_str(); nl.toLowerCase();
      if (nl.indexOf("tile") >= 0) kind = TK_TILE;
      else if (nl.indexOf("chipolo") >= 0) kind = TK_CHIPOLO;
      else if (nl.indexOf("smarttag") >= 0 || nl.indexOf("smart tag") >= 0 || nl.indexOf("galaxy find") >= 0) kind = TK_SMARTTAG;
      else if (kind < 0 && (nl.indexOf("itag") >= 0 || nl.indexOf("nutfind") >= 0 || nl.indexOf("tracker") >= 0)) kind = TK_UNKNOWN;
    }
    if (kind < 0) return;

    int rssi = d.getRSSI();
    String macS = d.getAddress().toString().c_str();
    const char *mac = macS.c_str();
    uint32_t now = millis();

    ClassStat &c = cs[kind];
    if (c.hits == 0 || now - c.lastSeen > GAP_MS) {   // (re)start the dwell window
      c.firstSeen = now; c.macN = 0; c.separated = false;
    }
    c.lastSeen = now;
    c.rssiSmooth = c.hits ? (c.rssiSmooth * 3 + rssi) / 4 : rssi;
    c.hits++;
    if (separated) c.separated = true;
    noteMac(c, mac);

    int slot = -1, oldest = 0;
    for (int i = 0; i < LIVE_N; i++) {
      if (live[i].seen && strcmp(live[i].mac, mac) == 0) { slot = i; break; }
      if (!live[i].seen && slot < 0) slot = i;
      if (live[i].seen < live[oldest].seen) oldest = i;
    }
    if (slot < 0) slot = oldest;
    strncpy(live[slot].mac, mac, 17); live[slot].mac[17] = 0;
    live[slot].kind = kind;
    live[slot].rssi = rssi;
    live[slot].seen = now;
  }
};
static Cb cb;

static bool classActive(int k) { return cs[k].hits > 0 && millis() - cs[k].lastSeen < GAP_MS; }

// --- event-level SD logging -------------------------------------------
// One row when a tracker class first comes into range, one when its FOLLOW
// flag rises. NO per-advert rows. Edge state is checked from trackerLoop()
// (main task) -- never the BLE callback -- so the SD write stays off the
// radio task. Flags re-arm when the class drops so a genuine reappearance
// logs again.
static bool tkSeenLogged[TK_N];
static bool tkFollowLogged[TK_N];

static void trackerLogEdges();

static bool classFollow(int k) {
  if (!classActive(k)) return false;
  const ClassStat &c = cs[k];
  if (millis() - c.firstSeen < FOLLOW_MS) return false;
  if (k == TK_FINDMY || k == TK_SMARTTAG) return c.macN >= 2 || c.separated;  // rotating types
  return true;                                                                // Tile/Chipolo: just persistent
}

static void trackerLogEdges() {
  if (!wlogIsOpen()) return;
  bool wrote = false;
  for (int k = 0; k < TK_N; k++) {
    bool act = classActive(k);
    if (act && !tkSeenLogged[k]) {
      char line[96];
      snprintf(line, sizeof(line), "%lu,seen,%s,%d,%u",
               (unsigned long)millis(), KIND_NAME[k], cs[k].rssiSmooth, cs[k].macN);
      wlogRow(line); wrote = true;
      tkSeenLogged[k] = true;
    } else if (!act) {
      tkSeenLogged[k] = false;    // re-arm for a fresh reappearance
    }

    bool foll = classFollow(k);
    if (foll && !tkFollowLogged[k]) {
      char line[96];
      snprintf(line, sizeof(line), "%lu,follow,%s,%d,%u",
               (unsigned long)millis(), KIND_NAME[k], cs[k].rssiSmooth, cs[k].macN);
      wlogRow(line); wrote = true;
      tkFollowLogged[k] = true;
    } else if (!foll) {
      tkFollowLogged[k] = false;
    }
  }
  if (wrote) wlogFlush();
}

static int liveRssiFor(int kind) {
  int best = -127; uint32_t now = millis();
  for (int i = 0; i < LIVE_N; i++)
    if (live[i].seen && live[i].kind == kind && now - live[i].seen < 4000 && live[i].rssi > best)
      best = live[i].rssi;
  return best;
}

// One fixed 36px slot per tracker class -- so a row repaints in place
// instead of the whole list reflowing (and flickering) every refresh.
static int slotY(int k) { return UI_CONTENT_Y + 2 + k * 36; }

// Static chrome: action row. Drawn once on enter / on return from locate,
// NOT in the 1.2s refresh -- redrawing it there was the flicker.
static void drawListChrome() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn r[1] = {{0, 0, 0, 0, "reset dwell timers"}};
  uiDrawActionRow(r, 1);
  resetBtn = r[0];
}

static void drawListRows() {
  int shown = 0;
  for (int k = 0; k < TK_N; k++) {
    int y = slotY(k);
    uiClearRect(4, y, tft.width() - 8, 34);          // just this slot
    if (!classActive(k)) { rows[k] = {0, 0, 0, 0, ""}; continue; }
    const ClassStat &c = cs[k];
    bool foll = classFollow(k);
    rows[k] = {4, y, tft.width() - 8, 32, ""};
    tft.drawRect(rows[k].x, rows[k].y, rows[k].w, rows[k].h, foll ? ILI9341_RED : ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setTextColor(foll ? ILI9341_RED : ILI9341_WHITE);
    tft.setCursor(8, y + 2);
    tft.print(KIND_NAME[k]);
    tft.setTextSize(1);
    tft.setTextColor(foll ? ILI9341_RED : ILI9341_CYAN);
    uint32_t d = (millis() - c.firstSeen) / 1000;
    tft.setCursor(8, y + 21);
    tft.printf("%ddBm  %lum%02lus  %dmac%s", c.rssiSmooth, (unsigned long)(d / 60),
               (unsigned long)(d % 60), c.macN,
               foll ? "  << FOLLOW" : (c.separated ? "  separated" : ""));
    shown++;
  }
  tft.setTextSize(1);
  if (shown == 0) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(6, slotY(0) + 10);
    tft.print("No trackers in range.");
  }
}

static void drawList() { drawListChrome(); drawListRows(); }

static void drawLocateChrome() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn r[1] = {{0, 0, 0, 0, "< list"}};
  uiDrawActionRow(r, 1);
  backRow = r[0];
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(6, UI_CONTENT_Y + 6);
  tft.print(KIND_NAME[locKind]);
  tft.drawRect(4, UI_CONTENT_Y + 74, tft.width() - 8, 24, ILI9341_WHITE);
  lastShownRssi = -999;
}

static void updateLocate() {
  if (millis() - lastLoc < 350) return;
  lastLoc = millis();
  int rssi = liveRssiFor(locKind);
  beepHold(rssi > -127);
  if (rssi > -127) rangeBeep(rssi);
  if (rssi == lastShownRssi) return;
  lastShownRssi = rssi;

  tft.fillRect(4, UI_CONTENT_Y + 34, tft.width() - 8, 34, ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(rssi > -127 ? ILI9341_CYAN : ILI9341_DARKGREY);
  tft.setCursor(6, UI_CONTENT_Y + 34);
  if (rssi > -127) tft.printf("%4d dBm", rssi); else tft.print(" -- lost");

  int barW = rssi > -127 ? map(constrain(rssi, -95, -35), -95, -35, 0, tft.width() - 10) : 0;
  tft.fillRect(5, UI_CONTENT_Y + 75, tft.width() - 10, 22, ILI9341_BLACK);
  tft.fillRect(5, UI_CONTENT_Y + 75, barW, 22,
               rssi > -55 ? ILI9341_GREEN : (rssi > -75 ? ILI9341_YELLOW : ILI9341_RED));
}

void trackerEnter() {
  uiDrawTopBar("Tracker Detect");
  sub = LIST;
  memset(cs, 0, sizeof(cs));
  memset(live, 0, sizeof(live));
  memset(tkSeenLogged, 0, sizeof(tkSeenLogged));
  memset(tkFollowLogged, 0, sizeof(tkFollowLogged));
  wlogOpen("tracker", "millis,event,class,rssi,macs");   // event rows only; ok if SD absent
  uiShowLoading("Listening...");
  if (!pScan) {
    WiFi.disconnect(true, false);   // radio coexistence -- see README
    WiFi.mode(WIFI_OFF);
    delay(50);
    BLEDevice::init("");
    pScan = BLEDevice::getScan();
  }
  pScan->setActiveScan(false);          // truly listen-only -- no scan requests
  pScan->setInterval(200);
  pScan->setWindow(180);
  pScan->setAdvertisedDeviceCallbacks(&cb, true /* want duplicates */);
  pScan->start(0, nullptr, false);      // continuous until trackerExit()
  drawList();
  lastListDraw = millis();
}

void trackerLoop() {
  trackerLogEdges();   // detection runs in both sub-modes -- check edges regardless
  if (sub == LIST) {
    if (millis() - lastListDraw > 1200) { drawListRows(); lastListDraw = millis(); }
  } else {
    updateLocate();
  }
}

void trackerTouch(const TouchPoint &t) {
  if (sub == LIST) {
    if (uiTouchInButton(t, resetBtn)) {
      for (int k = 0; k < TK_N; k++) if (cs[k].hits) cs[k].firstSeen = millis();
      drawList(); lastListDraw = millis();
      uiWaitForRelease();
      return;
    }
    for (int k = 0; k < TK_N; k++) {
      if (classActive(k) && uiTouchInButton(t, rows[k])) {
        locKind = k;
        sub = LOCATE;
        drawLocateChrome();
        lastLoc = 0;
        uiWaitForRelease();
        return;
      }
    }
    return;
  }
  if (uiTouchInButton(t, backRow)) {
    beepHold(false);
    sub = LIST;
    drawList(); lastListDraw = millis();
    uiWaitForRelease();
  }
}

bool trackerHandleBack() {
  if (sub == LIST) return false;
  beepHold(false);
  sub = LIST;
  drawList(); lastListDraw = millis();
  return true;
}

void trackerExit() {
  beepHold(false);
  wlogClose();
  if (pScan) {
    pScan->stop();
    pScan->setAdvertisedDeviceCallbacks(nullptr);
    pScan = nullptr;
  }
  BLEDevice::deinit(false);   // radio coexistence -- see README; re-inits via the !pScan guard
}
