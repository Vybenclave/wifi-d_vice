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
#include "accent.h"
#include "power.h"

// The main loop drains this value. It switches screens after a tap.
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
static Btn trackBtn, connectBtn, muteBtn;   // The code positions these buttons after the screen size changes.
// The action row toggles SD logging. The code writes one row per access point after each scan.
// The system opens the file on toggle. It closes the file on toggle off or screen exit.
static Btn logBtn;
static bool logging = false;
// The synchronous scan blocks the main task. It stalls touch polling.
// The asynchronous scan runs on the WiFi driver task. The main loop polls for completion.
// Touch input stays responsive during the scan.
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
  WiFi.mode(WIFI_STA);             // Another screen may power off the WiFi module.
  WiFi.setSleep(false);            // Power save mode causes unpredictable scan timing.
  // The default dwell time misses access points after a cold start.
  // A 200 millisecond dwell per channel finds them quickly.
  WiFi.scanNetworks(true, true, false, 200);
  listScanPending = true;
  ledBusy(true);
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

// The list uses larger text for SSIDs. The content starts below the top bar.
// This leaves space for the log toggle button.
static const int LIST_Y0 = UI_CONTENT_Y + 2, LIST_STEP = 20;

// The system tracks the last drawn signature for each row.
// The action row clears itself. The code only redraws rows with changed signatures.
static char prevRow[MAX_ROWS][UI_LIST_SIG_LEN];

static void drawList() {
  Btn row[1] = {{0, 0, 0, 0, logging ? "log: on" : "log: off"}};
  uiDrawActionRow(row, 1);
  logBtn = row[0];

  int maxVisible = (tft.height() - 18 - LIST_Y0) / LIST_STEP + 1;
  if (maxVisible > MAX_ROWS) maxVisible = MAX_ROWS;
  if (maxVisible < 0) maxVisible = 0;
  int shown = min(rowCount, maxVisible);

  uiDrawListIfChanged(4, LIST_Y0, tft.width() - 4, LIST_STEP, shown, maxVisible, prevRow,
    [](int i, char *sig, size_t cap) {
      snprintf(sig, cap, "%-13.13s c%-3d %ddBm", rows[i].ssid.c_str(), rows[i].channel, rows[i].rssi);
    },
    [](int i) {
      int y = LIST_Y0 + i * LIST_STEP;
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(2);
      tft.setCursor(4, y);
      char nm[14];
      snprintf(nm, sizeof(nm), "%-13.13s", rows[i].ssid.c_str());
      tft.print(nm);
      tft.setTextSize(1);
      tft.setTextColor(accentLabel());
      tft.setCursor(4 + 13 * 12 + 4, y + 5);
      tft.printf("c%-3d %ddBm", rows[i].channel, rows[i].rssi);
    });
  tft.setTextSize(1);
}

static void drawDetail() {
  // The action row matches other screens in this file.
  // Per-screen buttons belong in the action row. They do not float freely.
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
  tft.setTextColor(accentLabel());
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

// The function prompts for a passphrase on closed networks. It starts the connection process.
// It waits for success, failure, or timeout. The speed test screen requires an active network.
static void doConnectFlow() {
  const ApInfo &r = rows[selected];
  bool open = (r.enc == WIFI_AUTH_OPEN);
  String pass;
  if (!open) {
    String saved;
    engStoreFindWifi(r.ssid, saved);   // pre-fill from a saved profile if we have one
    String prompt = "Wi-Fi passphrase for " + r.ssid;
    pass = uiTextInput(prompt.c_str(), saved, true);
    // The keyboard clears the entire screen. The code restores the top bar.
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
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
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
      uiClearRect(b.x, b.y, b.w, b.h);
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
    devTimeBeginNet();   // The system starts SNTP synchronization now that the network is active.
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
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
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

// The code redraws only changed elements. It updates at a fixed interval.
// Redrawing every loop causes flicker. It also blocks touch input and breaks the back button.
static const uint32_t LOCATE_UPDATE_MS = 400;
static int lastRssiShown = -999;

static void drawLocateChrome() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn row[1] = {{0,0,0,0, muted ? "unmute" : "mute"}};
  uiDrawActionRow(row, 1);
  muteBtn = row[0];
  tft.setTextColor(accentLabel());
  tft.setTextSize(2);
  tft.setCursor(4, UI_CONTENT_Y + 6);
  tft.print(rows[selected].ssid);
  tft.drawRect(4, UI_CONTENT_Y + 73, tft.width() - 8, 26, ILI9341_WHITE);
  lastRssiShown = -999;   // force the dynamic region to redraw once
}

static void applyLocateReading(int rssi) {
  uiDrawLocateReading(4, UI_CONTENT_Y + 36, tft.width() - 8,
                       accentLabel(), -90, -30, &lastRssiShown, rssi);

  beepHold(!muted);              // The amplifier stays warm to prevent short chirps from dropping.
  if (!muted) rangeBeep(rssi);
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
  logging = false;   // The system closes the log file on screen exit.
  memset(prevRow, 0, sizeof(prevRow));   // The loading screen clears the content area. The code resets the row signatures.
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
    // The operator walks around while tracking signal strength.
    // The code updates the idle timer every loop. The display stays awake during the track.
    powerNoteActivity();
    updateLocate();
  }
}

void wifiScanTouch(const TouchPoint &t) {
  if (subMode == LIST) {
    if (!t.isNewPress) return;   // The code uses manual bounds checking. It requires a custom edge guard.
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

// The back button steps up one screen level. It returns to the AP list from detail or locate modes.
// It exits the screen only from the list. The system uses one back button per screen.
bool wifiScanHandleBack() {
  if (subMode == LIST) return false;
  beepHold(false);
  locateScanPending = false;
  subMode = LIST;
  memset(prevRow, 0, sizeof(prevRow));   // The loading screen clears the content area. The code resets the row signatures.
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
