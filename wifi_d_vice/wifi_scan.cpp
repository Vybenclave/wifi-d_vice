#include <WiFi.h>
#include "ui.h"
#include "wifi_scan.h"
#include "screens.h"
#include "mac_vendor.h"
#include "keyboard.h"
#include "devtime.h"
#include "engstore.h"
#include "wlog.h"
#include "wifiauto.h"
#include "theme.h"

// Set when the user taps a shortcut on the post-connect screen; the main
// loop drains it via wifiScanTakePendingJump() and switches screens.
static int s_pendingJump = WSJUMP_NONE;
int wifiScanTakePendingJump() { int j = s_pendingJump; s_pendingJump = WSJUMP_NONE; return j; }

enum SubMode { LIST, DETAIL, LOCATE };
static SubMode subMode = LIST;
static uint32_t lastScan = 0;
static int selected = -1;
static int lockedChannel = 0;
static String lockedBssidStr;
static bool muted = false;
static const int MAX_ROWS = 10;
static int rowCount = 0;
static ApInfo rows[MAX_ROWS];
static Btn trackBtn, connectBtn, muteBtn;   // positioned once tft is sized/rotated
// LIST-mode SD logging: an action-row toggle (default off). While on, every
// completed scan writes one row per AP via the shared wlog. Opened on
// toggle-on, closed on toggle-off and on wifiScanExit().
static Btn logBtn;
static bool logging = false;
// WiFi.scanNetworks(false, ...) blocks the calling task until every channel
// is hopped (1-4s for a full scan) -- that's the whole ESP32 task, so
// loop()'s touch-polling stalls right along with it (confirmed as the cause
// of "unresponsive UI" reports here, not something needing a separate
// FreeRTOS task). async=true kicks the scan off on the WiFi driver's own
// task and returns immediately; WiFi.scanComplete() is polled from loop()
// each iteration instead, so touch keeps getting serviced while a scan runs.
static bool listScanPending = false;
static bool locateScanPending = false;

static const char *encName(wifi_auth_mode_t enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    case WIFI_AUTH_OWE: return "OWE";
    default: return "?";
  }
}

static void startListScan() {
  WiFi.mode(WIFI_STA);             // may have been powered off by another screen
  WiFi.setSleep(false);            // power save makes scan yield vary wildly
  // Explicit 200ms/channel active dwell (default is a fast pass). On a
  // radio that was just cold-re-inited the default caught almost nothing
  // for ~5 scans; 200ms/ch is ~2.8s for a full sweep but lands APs on
  // scan 1.
  WiFi.scanNetworks(true, true, false, 200);
  listScanPending = true;
  ledBusy(true);                   // green heartbeat until the scan lands
}

static void harvestListScan(int n) {
  rowCount = min(n, MAX_ROWS);
  for (int i = 0; i < rowCount; i++) {
    rows[i].ssid = WiFi.SSID(i);
    memcpy(rows[i].bssid, WiFi.BSSID(i), 6);
    rows[i].rssi = WiFi.RSSI(i);
    rows[i].channel = WiFi.channel(i);
    rows[i].enc = WiFi.encryptionType(i);
  }
  // Log every AP the scan saw (not just the MAX_ROWS shown), one row each,
  // then a single flush for the batch.
  if (logging) {
    for (int i = 0; i < n; i++) {
      char line[220];
      snprintf(line, sizeof(line), "%s,%s,%s,%d,%d,%s",
               devTimeNowString().c_str(), WiFi.SSID(i).c_str(), WiFi.BSSIDstr(i).c_str(),
               (int)WiFi.RSSI(i), (int)WiFi.channel(i), encName(WiFi.encryptionType(i)));
      wlogRow(line);
    }
    wlogFlush();
  }
  WiFi.scanDelete();
  listScanPending = false;
  ledBusy(false);
}

// Bigger list: SSID at text size 2, channel/RSSI in a size-1 tail. Content
// starts at UI_CONTENT_Y (not the plain 29) to leave room for the log
// toggle in the action row -- see the UI rule in ui.h.
static const int LIST_Y0 = UI_CONTENT_Y + 2, LIST_STEP = 20;

static void drawList() {
  uiClearBelow(29);
  Btn row[1] = {{0, 0, 0, 0, logging ? "log: on" : "log: off"}};
  uiDrawActionRow(row, 1);
  logBtn = row[0];
  int y = LIST_Y0;
  for (int i = 0; i < rowCount && y + 16 <= tft.height() - 2; i++) {
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(4, y);
    char nm[14];
    snprintf(nm, sizeof(nm), "%-13.13s", rows[i].ssid.c_str());
    tft.print(nm);
    tft.setTextSize(1);
    tft.setTextColor(thLabel());
    tft.setCursor(4 + 13 * 12 + 4, y + 5);
    tft.printf("c%-3d %ddBm", rows[i].channel, rows[i].rssi);
    y += LIST_STEP;
  }
  tft.setTextSize(1);
}

static void drawDetail() {
  // Action row, not a bottom-pinned footer -- matches drawList() and
  // drawLocateChrome() in this same file (and the UI rule in ui.h: a
  // per-screen button belongs in the action row, not floating wherever).
  // This used to sit at tft.height()-44, which put it right under a short
  // 5-line info block on a short (landscape) screen but stranded it far
  // below a big empty gap in portrait -- looked like the button had
  // "fallen" to the bottom of the screen.
  uiClearBelow(UI_ACTIONROW_Y);
  Btn row[2] = {{0, 0, 0, 0, "Connect"}, {0, 0, 0, 0, "Track"}};
  uiDrawActionRow(row, 2);
  connectBtn = row[0];
  trackBtn   = row[1];

  const ApInfo &r = rows[selected];
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5]);
  tft.setTextSize(1);
  tft.setTextColor(thLabel());
  int y = UI_CONTENT_Y + 4;
  tft.setCursor(4, y);       tft.printf("SSID: %s", r.ssid.c_str());          y += 16;
  tft.setCursor(4, y);       tft.printf("BSSID: %s", macStr);                 y += 16;
  tft.setCursor(4, y);       tft.printf("Vendor: %s", macVendorTag(r.bssid).c_str()); y += 16;
  tft.setCursor(4, y);       tft.printf("Channel: %d   Security: %s", r.channel, encName(r.enc)); y += 16;
  tft.setCursor(4, y);       tft.printf("RSSI: %d dBm", r.rssi);              y += 18;

  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == r.ssid) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(4, y);
    tft.print("connected  "); tft.print(WiFi.localIP());
  }
}

// Blocking join flow (modal, like the on-screen keyboard it calls): prompt
// for a passphrase if the AP isn't open, kick off WiFi.begin(), and sit on
// a status/back-to-cancel wait until it connects, fails, or times out. Used
// so the Speed test screen has a network to run against.
static void doConnectFlow() {
  const ApInfo &r = rows[selected];
  bool open = (r.enc == WIFI_AUTH_OPEN);
  String pass;
  if (!open) {
    String saved;
    engStoreFindWifi(r.ssid, saved);   // pre-fill from a saved profile if we have one
    String prompt = "Wi-Fi passphrase for " + r.ssid;
    pass = uiTextInput(prompt.c_str(), saved, true);
    // The keyboard did a full fillScreen -- the top bar is gone, restore it.
    uiDrawTopBar("WiFi Scan");
    if (pass.length() == 0) { drawDetail(); return; }   // cancelled / empty
  }

  uiDrawTopBar("WiFi Scan");
  uiClearBelow(29);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(4, 40);
  tft.printf("Connecting to %s ...", r.ssid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(250);        // cold-PHY settle (same reason as the scan path)
  if (open) WiFi.begin(r.ssid.c_str());
  else      WiFi.begin(r.ssid.c_str(), pass.c_str());

  ledBusy(true);
  uint32_t t0 = millis();
  bool cancelled = false;
  while (millis() - t0 < 20000) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED || s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) break;
    TouchPoint t = uiReadTouch();
    if (t.pressed && uiTouchInBackButton(t)) { cancelled = true; uiWaitForRelease(); break; }
    delay(80);
  }
  ledBusy(false);

  uiDrawTopBar("WiFi Scan");
  uiClearBelow(29);
  tft.setTextSize(1);
  bool ok = (WiFi.status() == WL_CONNECTED);
  Btn stBtn{}, nsBtn{}, saveChk{}, bootChk{};
  bool saveNet = true;
  bool bootConn = wifiAutoIsFor(r.ssid);
  String storePass = open ? String("") : pass;

  auto drawChecks = [&]() {
    auto box = [&](const Btn &b, bool on, const char *label) {
      tft.fillRect(b.x, b.y, b.w, b.h, ILI9341_BLACK);
      tft.drawRect(b.x, b.y + 1, 14, 14, ILI9341_WHITE);
      if (on) {
        tft.drawLine(b.x + 2, b.y + 8, b.x + 5, b.y + 12, ILI9341_GREEN);
        tft.drawLine(b.x + 5, b.y + 12, b.x + 13, b.y + 3, ILI9341_GREEN);
      }
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(1);
      tft.setCursor(b.x + 22, b.y + 4);
      tft.print(label);
    };
    box(saveChk, saveNet,  "save network");
    box(bootChk, bootConn, "connect on boot");
  };
  auto applyPersist = [&]() {
    if (!ok) return;
    if (saveNet) engStoreSaveWifi(r.ssid, storePass);        // SD, per-client
    if (bootConn)                 wifiAutoStore(r.ssid, storePass);
    else if (wifiAutoIsFor(r.ssid)) wifiAutoClear();
  };

  if (ok) {
    devTimeBeginNet();   // start SNTP (DHCP / gateway / NIST) now that we're online
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(4, 40);  tft.printf("Connected to %s", r.ssid.c_str());
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(4, 60);  tft.print("IP:   "); tft.print(WiFi.localIP());
    tft.setCursor(4, 76);  tft.printf("RSSI: %d dBm", WiFi.RSSI());

    int by = 96, halfW = (tft.width() - 12) / 2;
    stBtn = {4, by, halfW, 34, "Speed test"};
    nsBtn = {8 + halfW, by, tft.width() - 12 - halfW, 34, "Network info"};
    uiDrawMenuButton(stBtn);
    uiDrawMenuButton(nsBtn);
    saveChk = {4, by + 42, tft.width() - 8, 16, ""};
    bootChk = {4, by + 62, tft.width() - 8, 16, ""};
    drawChecks();
  } else {
    WiFi.disconnect();
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(4, 40);  tft.print(cancelled ? "Cancelled." : "Connection failed.");
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(4, 62);  tft.print("Wrong passphrase or out of range.");
  }
  for (;;) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) {
      if (uiTouchInBackButton(t)) { applyPersist(); uiWaitForRelease(); break; }
      if (ok && uiTouchInButton(t, stBtn)) { applyPersist(); s_pendingJump = WSJUMP_NETSTATS_SPEED; uiWaitForRelease(); return; }
      if (ok && uiTouchInButton(t, nsBtn)) { applyPersist(); s_pendingJump = WSJUMP_NETSTATS_CONN;  uiWaitForRelease(); return; }
      if (ok && uiTouchInButton(t, saveChk)) {
        saveNet = !saveNet;
        if (!saveNet) bootConn = false;   // can't boot-connect to a network we didn't save
        drawChecks();
        uiWaitForRelease();
      }
      if (ok && uiTouchInButton(t, bootChk)) {
        bootConn = !bootConn;
        if (bootConn) saveNet = true;
        drawChecks();
        uiWaitForRelease();
      }
    }
    delay(15);
  }
  uiDrawTopBar("WiFi Scan");
  drawDetail();
}

// Locate/track mode redraws only the parts that actually change (RSSI
// number + bar), on a throttled interval -- confirmed on hardware that
// redrawing the whole area every loop iteration (which also re-ran a
// ~300ms blocking WiFi scan every single time, unthrottled) caused visible
// flicker and left touch getting checked far too rarely, which also broke
// the back button in this mode.
static const uint32_t LOCATE_UPDATE_MS = 400;
static int lastRssiShown = -1000;

static void drawLocateChrome() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn row[1] = {{0,0,0,0, muted ? "unmute" : "mute"}};
  uiDrawActionRow(row, 1);
  muteBtn = row[0];
  tft.setTextColor(thLabel());
  tft.setTextSize(2);
  tft.setCursor(4, UI_CONTENT_Y + 6);
  tft.print(rows[selected].ssid);
  tft.drawRect(4, UI_CONTENT_Y + 70, tft.width() - 8, 30, ILI9341_WHITE);
  lastRssiShown = -1000;   // force the dynamic region to redraw once
}

static void applyLocateReading(int rssi) {
  if (rssi != lastRssiShown) {
    lastRssiShown = rssi;
    tft.fillRect(4, UI_CONTENT_Y + 36, tft.width() - 8, 34, ILI9341_BLACK);
    tft.setTextColor(thLabel());
    tft.setTextSize(3);
    tft.setCursor(4, UI_CONTENT_Y + 36);
    tft.printf("%4d dBm", rssi);

    int barW = map(constrain(rssi, -90, -30), -90, -30, 0, tft.width() - 8);
    tft.fillRect(5, UI_CONTENT_Y + 71, tft.width() - 10, 28, ILI9341_BLACK);
    tft.fillRect(5, UI_CONTENT_Y + 71, barW, 28, rssi > -55 ? ILI9341_GREEN : (rssi > -75 ? ILI9341_YELLOW : ILI9341_RED));
  }

  beepHold(!muted);              // keep the amp warm so short chirps aren't swallowed
  if (!muted) rangeBeep(rssi);   // rate + pitch scale with signal as a range proxy
}

static void updateLocate() {
  if (locateScanPending) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;   // not ready yet -- don't block, just check next loop()
    locateScanPending = false;
    int rssi = -100;
    if (n >= 0) {
      for (int i = 0; i < n; i++) {
        if (WiFi.BSSIDstr(i) == lockedBssidStr) { rssi = WiFi.RSSI(i); break; }
      }
      WiFi.scanDelete();
    }
    lastScan = millis();
    applyLocateReading(rssi);
    return;
  }
  if (millis() - lastScan < LOCATE_UPDATE_MS) return;
  WiFi.scanNetworks(true, false, false, 300, lockedChannel);   // async
  locateScanPending = true;
}

void wifiScanEnter() {
  subMode = LIST;
  logging = false;   // fresh each entry; the file is closed in wifiScanExit()
  uiDrawTopBar("WiFi Scan");
  uiShowLoading("Scanning...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(250);        // let a possibly-cold PHY settle before the first scan
  startListScan();
  lastScan = millis();
}

void wifiScanLoop() {
  if (subMode == LIST) {
    if (listScanPending) {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) return;   // still hopping channels -- don't block, retry next loop()
      if (n >= 0) harvestListScan(n); else { listScanPending = false; ledBusy(false); }   // WIFI_SCAN_FAILED: drop it, retry on the next interval
      drawList();
      lastScan = millis();
      return;
    }
    if (millis() - lastScan > 4000) startListScan();
  } else if (subMode == LOCATE) {
    updateLocate();
  }
}

void wifiScanTouch(const TouchPoint &t) {
  if (subMode == LIST) {
    if (!t.isNewPress) return;   // manual bounds check below, not uiTouchInButton() -- needs its own edge guard
    if (uiTouchInButton(t, logBtn)) {
      if (!logging) {
        logging = wlogOpen("wifiscan", "utc,ssid,bssid,rssi,channel,security");
      } else {
        logging = false;
        wlogClose();
      }
      drawList();
      uiWaitForRelease();
      return;
    }
    for (int i = 0; i < rowCount; i++) {
      int rowY = LIST_Y0 + i * LIST_STEP;
      if (rowY + 16 > tft.height() - 2) break;   // wasn't drawn
      if (t.y >= rowY - 2 && t.y < rowY + LIST_STEP - 2) {
        selected = i;
        subMode = DETAIL;
        uiDrawTopBar("WiFi Scan");
        drawDetail();
        uiWaitForRelease();
        return;
      }
    }
    return;
  }

  if (subMode == DETAIL) {
    if (uiTouchInButton(t, connectBtn)) {
      doConnectFlow();
      uiWaitForRelease();
      return;
    }
    if (uiTouchInButton(t, trackBtn)) {
      char buf[18];
      snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
               rows[selected].bssid[0], rows[selected].bssid[1], rows[selected].bssid[2],
               rows[selected].bssid[3], rows[selected].bssid[4], rows[selected].bssid[5]);
      lockedBssidStr = buf;
      lockedChannel = rows[selected].channel;
      subMode = LOCATE;
      drawLocateChrome();
      lastScan = 0;   // force an immediate first reading
      uiWaitForRelease();
    }
    return;
  }

  // LOCATE
  if (uiTouchInButton(t, muteBtn)) {
    muted = !muted;
    drawLocateChrome();
    uiWaitForRelease();
    return;
  }
}

// Top-bar back button steps up one level inside this screen: DETAIL or
// LOCATE -> the AP list; only from the list does it fall through to the
// main loop and leave the screen. One back button, same as every other
// screen -- no in-content "list" button needed.
bool wifiScanHandleBack() {
  if (subMode == LIST) return false;
  beepHold(false);
  locateScanPending = false;   // abandon any in-flight locate scan
  subMode = LIST;
  uiDrawTopBar("WiFi Scan");
  uiShowLoading("Scanning...");
  startListScan();
  return true;
}

void wifiScanExit() {
  beepHold(false);
  ledBusy(false);
  listScanPending = false;
  locateScanPending = false;
  logging = false;
  wlogClose();
  WiFi.scanDelete();
}
