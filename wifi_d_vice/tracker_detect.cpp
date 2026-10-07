#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <string.h>
#include "ui.h"
#include "wlog.h"
#include "devtime.h"
#include "accent.h"
#include "flagfinding.h"
#include "demomode.h"
#include "power.h"
#include <esp_random.h>

enum { TK_FINDMY = 0, TK_SMARTTAG, TK_TILE, TK_CHIPOLO, TK_FMDN, TK_UNKNOWN, TK_N };
static const char *KIND_NAME[TK_N] = { "AirTag/FindMy", "SmartTag", "Tile", "Chipolo", "Google FMDN", "Tracker?" };

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
static Btn rows[TK_N], resetBtn, backRow, flagBtn;
static uint32_t lastListDraw = 0, lastLoc = 0;
static uint32_t lastDemoSpawn = 0;
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
    // The service uses UUID 0xFEAA. Eddystone also uses this UUID.
    // The first data byte uses frame types 0x40 to 0x4F.
    // Eddystone uses types 0x00, 0x10, 0x20, and 0x30.
    // This prevents misreading the beacon.
    for (int i = 0; i < d.getServiceDataCount(); i++) {
      String su = d.getServiceDataUUID(i).toString(); su.toLowerCase();
      if (su.indexOf("feaa") < 0) continue;
      String sd = d.getServiceData(i);
      if (sd.length() >= 1 && ((uint8_t)sd[0] & 0xF0) == 0x40) kind = TK_FMDN;
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

// Demo mode updates the state arrays.
// It mimics real advertisement processing.
// This keeps all screens and logic identical.
// The code skips the BLE radio.
// It fabricates inputs instead.
static void spawnDemoHit() {
  int kind = esp_random() % TK_N;
  int rssi = demoRandRssi();
  uint8_t macBytes[6];
  demoRandMac(macBytes);
  String macS = demoRandMacStr(macBytes);
  const char *mac = macS.c_str();
  uint32_t now = millis();

  ClassStat &c = cs[kind];
  if (c.hits == 0 || now - c.lastSeen > GAP_MS) { c.firstSeen = now; c.macN = 0; c.separated = false; }
  c.lastSeen = now;
  c.rssiSmooth = c.hits ? (c.rssiSmooth * 3 + rssi) / 4 : rssi;
  c.hits++;
  if (kind == TK_FINDMY && (esp_random() % 4) == 0) c.separated = true;   // occasionally simulate a "lost" AirTag
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

// --- event-level SD logging -------------------------------------------
// The logger writes one row per class appearance.
// It writes one row when the FOLLOW flag rises.
// It never logs per advertisement.
// The main task checks edge state.
// The BLE callback never writes to SD.
// This keeps SD writes off the radio task.
// The flags reset when the class drops.
// A reappearance triggers a new log entry.
static bool tkSeenLogged[TK_N];
static bool tkFollowLogged[TK_N];

static void trackerLogEdges();

static bool classFollow(int k) {
  if (!classActive(k)) return false;
  const ClassStat &c = cs[k];
  if (millis() - c.firstSeen < FOLLOW_MS) return false;
  if (k == TK_FINDMY || k == TK_SMARTTAG || k == TK_FMDN) return c.macN >= 2 || c.separated;  // rotating types
  return true;                                                                // Tile/Chipolo: just persistent
}

static void trackerLogEdges() {
  if (!wlogIsOpen()) return;
  bool wrote = false;
  for (int k = 0; k < TK_N; k++) {
    bool act = classActive(k);
    if (act && !tkSeenLogged[k]) {
      char line[112];
      snprintf(line, sizeof(line), "%s,seen,%s,%d,%u",
               devTimeNowString().c_str(), KIND_NAME[k], cs[k].rssiSmooth, cs[k].macN);
      wlogRow(line); wrote = true;
      tkSeenLogged[k] = true;
    } else if (!act) {
      tkSeenLogged[k] = false;    // re-arm for a fresh reappearance
    }

    bool foll = classFollow(k);
    if (foll && !tkFollowLogged[k]) {
      char line[112];
      snprintf(line, sizeof(line), "%s,follow,%s,%d,%u",
               devTimeNowString().c_str(), KIND_NAME[k], cs[k].rssiSmooth, cs[k].macN);
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

// Active classes pack into consecutive 32-pixel slots.
// The screen fits five slots.
// The code recomputes slot assignment only when the active class set changes.
// This prevents row flicker during the 1.2-second refresh cycle.
static const int SLOT_H   = 32;
static const int MAX_SLOTS = 5;   // 58 + 5*32 = 218, clear of the 222px status bar
static int slotY(int slot) { return UI_CONTENT_Y + 2 + slot * SLOT_H; }
static uint8_t slotForClass[TK_N];   // TK_N sentinel = not shown
static uint8_t activeSig = 0xFF;     // bitmask of classes shown last full layout

// The code tracks a draw signature per slot.
// A row erases and reprints only when its content changes.
// This avoids unconditional redraws on every 1.2-second refresh.
static char prevRow[MAX_SLOTS][UI_LIST_SIG_LEN];
static char prevEmpty[48] = "";

// The action row draws once on entry.
// It draws again after returning from locate mode.
// The code skips this row during the 1.2-second refresh.
// Redrawing it caused screen flicker.
static void drawListChrome() {
  uiClearBelow(UI_ACTIONROW_Y);   // This clears the row area. It resets the draw state. The list function now redraws the blank area.
  memset(prevRow, 0, sizeof(prevRow));
  prevEmpty[0] = '\0';
  Btn r[1] = {{0, 0, 0, 0, "reset dwell timers"}};
  uiDrawActionRow(r, 1);
  resetBtn = r[0];
}

static void drawListRows() {
    // The code recomputes slot assignment only when active classes change.
    // The draw function diffs content per slot.
    // The signature includes the class ID.
    // A changed slot redraws automatically.
    // A stable slot only diffs its metrics.
    // This prevents unnecessary redraws.
  uint8_t sig = 0;
  for (int k = 0; k < TK_N; k++) if (classActive(k)) sig |= (1u << k);
  if (sig != activeSig) {
    activeSig = sig;
    int slot = 0;
    for (int k = 0; k < TK_N; k++)
      slotForClass[k] = (classActive(k) && slot < MAX_SLOTS) ? (uint8_t)slot++ : (uint8_t)TK_N;
  }

    // The code updates touch hit-boxes every frame.
    // This costs little CPU time.
    // The touch handler requires live coordinates.
    // It needs them even for unchanged rows.
  int idx[TK_N], shown = 0;
  for (int k = 0; k < TK_N; k++) {
    if (!classActive(k) || slotForClass[k] >= (uint8_t)TK_N) { rows[k] = {0, 0, 0, 0, ""}; continue; }
    int y = slotY(slotForClass[k]);
    rows[k] = {4, y, tft.width() - 8, SLOT_H - 4, ""};
    idx[shown++] = k;
  }

  uiDrawListIfChanged(4, UI_CONTENT_Y + 2, tft.width() - 8, SLOT_H, shown, MAX_SLOTS, prevRow,
    [&](int i, char *sigOut, size_t cap) {
      const ClassStat &c = cs[idx[i]];
      uint32_t d = (millis() - c.firstSeen) / 1000;
      snprintf(sigOut, cap, "%d|%d|%lu|%d|%d|%d", idx[i], c.rssiSmooth, (unsigned long)d, c.macN,
               classFollow(idx[i]), c.separated);
    },
    [&](int i) {
      int k = idx[i];
      int y = slotY(i);
      const ClassStat &c = cs[k];
      bool foll = classFollow(k);
      tft.drawRect(4, y, tft.width() - 8, SLOT_H - 4, foll ? ILI9341_RED : ILI9341_WHITE);
      tft.setTextSize(2);
      tft.setTextColor(foll ? ILI9341_RED : ILI9341_WHITE);
      tft.setCursor(8, y + 2);
      tft.print(KIND_NAME[k]);
      tft.setTextSize(1);
      tft.setTextColor(foll ? ILI9341_RED : accentLabel());
      uint32_t d = (millis() - c.firstSeen) / 1000;
      tft.setCursor(8, y + 19);
      tft.printf("%ddBm  %lum%02lus  %dmac%s", c.rssiSmooth, (unsigned long)(d / 60),
                 (unsigned long)(d % 60), c.macN,
                 foll ? "  << FOLLOW" : (c.separated ? "  separated" : ""));
    });
  tft.setTextSize(1);

  if (shown == 0) {
    if (uiFieldChanged(prevEmpty, sizeof(prevEmpty), "empty")) {
      tft.setTextColor(ILI9341_GREEN);
      tft.setCursor(6, slotY(0) + 10);
      tft.print("No trackers in range.");
    }
  } else {
    prevEmpty[0] = '\0';   // forget it so the message reprints if the list empties out again later
  }
}

static void drawList() { drawListChrome(); drawListRows(); }

static void drawLocateChrome() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn r[2] = {{0, 0, 0, 0, "< list"}, {0, 0, 0, 0, "Flag"}};
  uiDrawActionRow(r, 2);
  backRow = r[0];
  flagBtn = r[1];
  tft.setTextSize(2);
  tft.setTextColor(accentLabel());
  tft.setCursor(6, UI_CONTENT_Y + 6);
  tft.print(KIND_NAME[locKind]);
  tft.drawRect(4, UI_CONTENT_Y + 73, tft.width() - 8, 26, ILI9341_WHITE);
  lastShownRssi = -999;
}

static void updateLocate() {
  if (millis() - lastLoc < 350) return;
  lastLoc = millis();
  int rssi = liveRssiFor(locKind);
  beepHold(rssi > -127);
  if (rssi > -127) rangeBeep(rssi);

  uiDrawLocateReading(4, UI_CONTENT_Y + 36, tft.width() - 8,
                       accentLabel(), -95, -35, &lastShownRssi, rssi);
}

void trackerEnter() {
  uiDrawTopBar("Tracker Detect");
  sub = LIST;
  activeSig = 0xFF;   // force a full slot layout on the first drawListRows()
  memset(cs, 0, sizeof(cs));
  memset(live, 0, sizeof(live));
  memset(tkSeenLogged, 0, sizeof(tkSeenLogged));
  memset(tkFollowLogged, 0, sizeof(tkFollowLogged));
  wlogOpen("tracker", "utc,event,class,rssi,macs");   // event rows only; ok if SD absent
  uiShowLoading("Listening...");
  if (!demoModeEnabled()) {
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
  }
  drawList();
  lastListDraw = millis();
  lastDemoSpawn = 0;
}

void trackerLoop() {
  if (demoModeEnabled() && millis() - lastDemoSpawn > 4000) { spawnDemoHit(); lastDemoSpawn = millis(); }
  trackerLogEdges();   // detection runs in both sub-modes -- check edges regardless
  if (sub == LIST) {
    if (millis() - lastListDraw > 1200) { drawListRows(); lastListDraw = millis(); }
  } else {
    // The RSSI meter runs continuously.
    // It calls the power activity function every tick.
    // This keeps the display timeout clock fresh.
    // The screen stays awake during tracking.
    powerNoteActivity();
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
    return;
  }
  if (uiTouchInButton(t, flagBtn)) {
    uiWaitForRelease();
    // A separated advertisement signals a lost device.
    // This poses a higher risk than a passing tracker.
    // The code assigns an alert severity level.
    // It uses watch severity for nearby trackers.
    uint8_t sev = cs[locKind].separated ? UI_SEV_ALERT : UI_SEV_WATCH;
    flagDetectionShow("tracker", sev);   // LOCATE chrome (big class name + RSSI) already IS this screen's detail view
    uiDrawTopBar("Tracker Detect");
    drawLocateChrome();
    return;
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
  // The code checks initialization state before deinitializing.
  // Demo mode skips radio setup.
  // An unconditional deinit would crash the system.
  if (BLEDevice::getInitialized()) BLEDevice::deinit(false);   // radio coexistence -- see README; re-inits via the !pScan guard
}
