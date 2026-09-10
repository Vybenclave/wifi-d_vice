// "Rogue AP" screen (WiFi menu) -- learn a known-good AP baseline and watch
// live WiFi.scanNetworks() passes against it for evil-twin / downgrade /
// channel-move / RSSI-jump anomalies. The baseline (rogue_ap.cpp, SD
// /rogueap.csv) is shared with the WiFi IDS screen, which runs the same
// rogueApCheck() against received beacons.
//
// PASSIVE: standard station scans + a table compare. Nothing transmits.

#include <WiFi.h>
#include "ui.h"
#include "screens.h"
#include "rogue_ap.h"

enum RView { RV_VIEW, RV_LEARN, RV_DONE };
static RView    view = RV_VIEW;
static bool     scanPending = false;
static uint32_t lastScan = 0;
static int      learnPass = 0;
static int      lastCommitN = 0;
static uint32_t clearArmedAt = 0;     // "tap Clear again" window
static const int LEARN_PASSES = 4;

static Btn learnBtn, clearBtn;

static const int VMAX = 12;
struct VRow { char essid[19]; uint8_t ch; int8_t rssi; RogueKind k; };
static VRow vrows[VMAX];
static int  vrowN = 0;
static bool anyAlert = false;

// ---- scanning ----
static void startScan() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.scanNetworks(true, true, false, 180);   // async, incl. hidden, 180ms/ch
  scanPending = true;
  lastScan = millis();
  ledBusy(true);
}

// ---- drawing ----
static void drawChrome() {
  uiClearBelow(29);
  Btn row[2] = {{0, 0, 0, 0, "Learn"},
                {0, 0, 0, 0, rogueApLoaded() ? "Clear" : "--"}};
  uiDrawActionRow(row, 2);
  learnBtn = row[0];
  clearBtn = row[1];
}

static int contentBot() { return tft.height() - UI_STATUSBAR_H - 2; }

static void drawView() {
  tft.fillRect(0, UI_CONTENT_Y, tft.width(), contentBot() - UI_CONTENT_Y, ILI9341_BLACK);
  tft.setTextSize(1);

  tft.setTextColor(rogueApLoaded() ? ILI9341_CYAN : ILI9341_YELLOW);
  tft.setCursor(4, UI_CONTENT_Y);
  if (rogueApLoaded())
    tft.printf("baseline: %d nets  --  Learn to redo", rogueApCount());
  else
    tft.print("no baseline -- tap Learn with your real APs in range");

  if (clearArmedAt && millis() - clearArmedAt < 3000) {
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(4, UI_CONTENT_Y + 11);
    tft.print("tap Clear again to wipe the baseline");
  }

  int y = UI_CONTENT_Y + 26;
  for (int i = 0; i < vrowN && y + 13 <= contentBot(); i++) {
    RogueKind k = vrows[i].k;
    uint16_t col = k == ROGUE_NONE ? ILI9341_WHITE
                 : (k == ROGUE_EVIL_TWIN || k == ROGUE_DOWNGRADE) ? ILI9341_RED
                 : ILI9341_YELLOW;
    tft.setTextColor(col);
    tft.setCursor(4, y);
    tft.printf("%-18s c%-2d %4d  %s", vrows[i].essid, vrows[i].ch, vrows[i].rssi,
               k == ROGUE_NONE ? "" : rogueKindTag(k));
    y += 13;
  }
  if (vrowN == 0) {
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(4, y);
    tft.print("scanning...");
  }
}

static void drawLearn() {
  tft.fillRect(0, UI_CONTENT_Y, tft.width(), contentBot() - UI_CONTENT_Y, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(4, UI_CONTENT_Y);
  tft.printf("Learning baseline: pass %d/%d", learnPass, LEARN_PASSES);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(4, UI_CONTENT_Y + 14);
  tft.printf("%d networks seen so far", rogueApLearnPending());
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(4, UI_CONTENT_Y + 30);
  tft.print("keep your real APs powered + in range");
  tft.setCursor(4, UI_CONTENT_Y + 42);
  tft.print("back = cancel");
}

static void drawDone() {
  tft.fillRect(0, UI_CONTENT_Y, tft.width(), contentBot() - UI_CONTENT_Y, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_GREEN);
  tft.setCursor(4, UI_CONTENT_Y);
  tft.printf("Saved %d networks to /rogueap.csv", lastCommitN);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(4, UI_CONTENT_Y + 16);
  tft.print("tap to return to the watch view");
}

// ---- scan-complete handlers ----
static void addVRow(const String &ss, uint8_t ch, int rssi, RogueKind k) {
  if (vrowN >= VMAX) return;
  VRow &r = vrows[vrowN++];
  snprintf(r.essid, sizeof(r.essid), "%s", ss.length() ? ss.c_str() : "(hidden)");
  r.ch = ch;
  r.rssi = (int8_t)rssi;
  r.k = k;
}

static void harvestView(int n) {
  vrowN = 0;
  anyAlert = false;
  // pass 1: anomalies first
  for (int i = 0; i < n; i++) {
    uint8_t b[6]; memcpy(b, WiFi.BSSID(i), 6);
    String ss = WiFi.SSID(i);
    RogueHit h;
    RogueKind k = rogueApCheck(ss.c_str(), b, (uint8_t)WiFi.encryptionType(i),
                               (uint8_t)WiFi.channel(i), WiFi.RSSI(i), &h);
    if (k == ROGUE_NONE) continue;
    addVRow(ss, WiFi.channel(i), WiFi.RSSI(i), k);
    if (k == ROGUE_EVIL_TWIN || k == ROGUE_DOWNGRADE) anyAlert = true;
  }
  // pass 2: fill the rest with clean rows
  for (int i = 0; i < n && vrowN < VMAX; i++) {
    uint8_t b[6]; memcpy(b, WiFi.BSSID(i), 6);
    String ss = WiFi.SSID(i);
    if (rogueApCheck(ss.c_str(), b, (uint8_t)WiFi.encryptionType(i),
                     (uint8_t)WiFi.channel(i), WiFi.RSSI(i), nullptr) != ROGUE_NONE) continue;
    addVRow(ss, WiFi.channel(i), WiFi.RSSI(i), ROGUE_NONE);
  }
  WiFi.scanDelete();
  drawView();
  if (anyAlert) { ledSet(true); beep(400, 1200); }
  else            ledSet(false);
}

static void harvestLearn(int n) {
  for (int i = 0; i < n; i++) {
    uint8_t b[6]; memcpy(b, WiFi.BSSID(i), 6);
    rogueApLearnObserve(WiFi.SSID(i).c_str(), b, (uint8_t)WiFi.encryptionType(i),
                        (uint8_t)WiFi.channel(i), WiFi.RSSI(i));
  }
  WiFi.scanDelete();
  learnPass++;
  if (learnPass >= LEARN_PASSES) {
    lastCommitN = rogueApLearnCommit();
    view = RV_DONE;
    drawChrome();
    drawDone();
  } else {
    drawLearn();
    startScan();
  }
}

// ---- contract ----
void rogueEnter() {
  uiDrawTopBar("Rogue AP");
  view = RV_VIEW;
  vrowN = 0;
  clearArmedAt = 0;
  rogueApLoad();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(200);              // let a possibly-cold PHY settle before the first scan
  drawChrome();
  drawView();
  startScan();
}

void rogueLoop() {
  if (!scanPending) {
    if (view == RV_VIEW && millis() - lastScan > 4000) startScan();
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanPending = false;
  ledBusy(false);
  if (n < 0) n = 0;   // WIFI_SCAN_FAILED -> treat as empty, retry on the interval
  if (view == RV_LEARN) harvestLearn(n);
  else                  harvestView(n);
}

void rogueTouch(const TouchPoint &t) {
  if (!t.isNewPress) return;

  if (view == RV_DONE) {
    view = RV_VIEW;
    drawChrome();
    drawView();
    startScan();
    return;
  }
  if (view == RV_LEARN) return;   // back button cancels (rogueHandleBack)

  if (uiTouchInButton(t, learnBtn)) {
    view = RV_LEARN;
    learnPass = 0;
    clearArmedAt = 0;
    rogueApLearnReset();
    drawLearn();
    startScan();
    return;
  }
  if (rogueApLoaded() && uiTouchInButton(t, clearBtn)) {
    if (clearArmedAt && millis() - clearArmedAt < 3000) {
      rogueApClear();
      clearArmedAt = 0;
      drawChrome();
      drawView();
    } else {
      clearArmedAt = millis();
      drawView();
    }
    return;
  }
}

bool rogueHandleBack() {
  if (view == RV_VIEW) return false;   // leave the screen
  view = RV_VIEW;                      // LEARN / DONE -> back to the watch view
  drawChrome();
  drawView();
  startScan();
  return true;
}

void rogueExit() {
  scanPending = false;
  WiFi.scanDelete();
  ledBusy(false);
  ledSet(false);
  // leave the radio in WIFI_STA (cold-radio rule) for the next screen
}
