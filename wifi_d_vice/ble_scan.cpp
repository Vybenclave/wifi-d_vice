#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include "ui.h"
#include "mac_vendor.h"
#include "wlog.h"
#include "known_signatures.h"
#include "devtime.h"
#include "accent.h"
#include "flagfinding.h"
#include "demomode.h"
#include "power.h"
#include <esp_random.h>

enum SubMode { LIST, DETAIL, LOCATE };
static SubMode subMode = LIST;
static BLEScan *pBLEScan = nullptr;
static uint32_t lastScan = 0;
static const int MAX_ROWS = 10;

// Classify notable devices on the scan list. Two classes appear on sight.
// FLIPPER devices start with "Flipper ". GLASSES devices match a name or a Meta company ID.
// Signatures live in known_signatures.h. This scan only reads existing advertisement fields.
enum BleClass { BC_NONE = 0, BC_FLIPPER, BC_GLASSES };
static const char *bleClassLabel(BleClass c) {
  return c == BC_FLIPPER ? "FLIPPER" : c == BC_GLASSES ? "GLASSES" : "";
}
static uint16_t bleClassColor(BleClass c) {
  return c == BC_FLIPPER ? ILI9341_MAGENTA : c == BC_GLASSES ? ILI9341_ORANGE : ILI9341_WHITE;
}
static BleClass bleClassify(BLEAdvertisedDevice &d) {
  if (d.haveName()) {
    String low = String(d.getName().c_str()); low.toLowerCase();
    if (matchesAnyPattern(low, kFlipperNamePatterns, kFlipperNamePatternCount)) return BC_FLIPPER;
    if (matchesAnyPattern(low, kMetaGlassesNamePatterns, kMetaGlassesNamePatternCount)) return BC_GLASSES;
  }
  if (d.haveManufacturerData()) {
    String md = d.getManufacturerData();
    if (md.length() >= 2) {
      uint16_t cid = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
      for (int i = 0; i < kMetaCompanyIdCount; i++)
        if (cid == kMetaCompanyIds[i]) return BC_GLASSES;
    }
  }
  return BC_NONE;
}

struct BleRow { String name; uint8_t mac[6]; String macStr; int rssi; BleClass cls; };
static BleRow rows[MAX_ROWS];
static int rowCount = 0;
// Count all devices in the last scan. The summary line uses these totals.
static int nFlipper = 0, nGlasses = 0;
static int selected = -1;
static Btn trackBtn, muteBtn, flagBtn;
static bool muted = false;
static int lastRssiShown = -999;
// Toggle SD logging for the list mode. The default state is off.
// The code writes one row per device per scan.
// The code opens the file on toggle.
// The code closes the file on toggle off or screen exit.
static Btn logBtn;
static bool logging = false;

static const uint32_t SCAN_SECONDS = 1;
static volatile bool bleDone = false;
static bool scanInFlight = false;
// Request a fresh list scan immediately. A previous scan may still run.
// The code cannot start a new scan until the old one finishes.
// This flag skips the normal three second wait.
// The code starts the new scan right after the old one ends.
static bool forceListScan = false;
static bool locateModeScan = false;   // which half of finishScan() applies to the in-flight scan

static void onScanComplete(BLEScanResults) { bleDone = true; }
static void drawRows();

// Demo mode generates fake data. The code skips the BLE radio.
// It bypasses the async scan pipeline.
// List mode fills rows and redraws immediately.
// Locate mode uses its own timing gate.
// The real scan takes one second.
// Demo mode skips it.
static void spawnDemoList() {
  static const char *kNames[] = {"iPhone", "Galaxy Buds", "Flipper 1A2B", "Pixel Watch", "AirPods Pro"};
  rowCount = 3 + (int)(esp_random() % 3);   // 3..5 fake rows
  nFlipper = nGlasses = 0;
  for (int i = 0; i < rowCount; i++) {
    demoRandMac(rows[i].mac);
    rows[i].macStr = demoRandMacStr(rows[i].mac);
    rows[i].name = demoRandPick(kNames, 5);
    rows[i].rssi = demoRandRssi();
    rows[i].cls = (rows[i].name == "Flipper 1A2B") ? BC_FLIPPER : BC_NONE;
    if (rows[i].cls == BC_FLIPPER) nFlipper++;
  }
}

static void startScan(bool locate) {
  if (demoModeEnabled()) {
    locateModeScan = locate;
    if (locate) {
      if (millis() - lastScan < SCAN_SECONDS * 1000) return;   // self-pace, same cadence a real scan would
      lastScan = millis();
      int rssi = demoRandRssi();
      uiDrawLocateReading(4, UI_CONTENT_Y + 36, tft.width() - 8, accentLabel(), -95, -40, &lastRssiShown, rssi);
      beepHold(!muted);
      if (!muted) rangeBeep(rssi);
    } else {
      lastScan = millis();
      spawnDemoList();
      drawRows();
    }
    return;
  }
  locateModeScan = locate;
  bleDone = false;
  scanInFlight = true;
  pBLEScan->start(SCAN_SECONDS, onScanComplete, false);
}

static int finishScan() {
  scanInFlight = false;
  lastScan = millis();
  BLEScanResults *results = pBLEScan->getResults();
  int rssi = -100;

  if (locateModeScan) {
    for (int i = 0; i < (int)results->getCount(); i++) {
      BLEAdvertisedDevice d = results->getDevice(i);
      uint8_t mac[6];
      memcpy(mac, d.getAddress().getNative(), 6);
      if (memcmp(mac, rows[selected].mac, 6) == 0) { rssi = d.getRSSI(); break; }
    }
  } else {
    int total = (int)results->getCount();
    rowCount = min(total, MAX_ROWS);
    nFlipper = nGlasses = 0;
    for (int i = 0; i < rowCount; i++) {
      BLEAdvertisedDevice d = results->getDevice(i);
      rows[i].name = d.haveName() ? String(d.getName().c_str()) : "(no name)";
      memcpy(rows[i].mac, d.getAddress().getNative(), 6);
      rows[i].macStr = String(d.getAddress().toString().c_str());
      rows[i].rssi = d.getRSSI();
      rows[i].cls = bleClassify(d);
    }
    for (int i = 0; i < total; i++) {
      BLEAdvertisedDevice d = results->getDevice(i);
      BleClass c = bleClassify(d);
      if (c == BC_FLIPPER) nFlipper++;
      else if (c == BC_GLASSES) nGlasses++;
    }
    // Log every device the scan detects. The code writes one row per device.
    // The code flushes the batch once.
    if (logging) {
      for (int i = 0; i < total; i++) {
        BLEAdvertisedDevice d = results->getDevice(i);
        uint8_t mac[6];
        memcpy(mac, d.getAddress().getNative(), 6);
        char line[192];
        snprintf(line, sizeof(line), "%s,%s,%s,%s,%d",
                 devTimeNowString().c_str(),
                 d.haveName() ? d.getName().c_str() : "(no name)",
                 d.getAddress().toString().c_str(),
                 macVendorTag(mac).c_str(), d.getRSSI());
        wlogRow(line);
      }
      wlogFlush();
    }
  }
  pBLEScan->clearResults();
  return rssi;
}

// Display the list with two lines per row.
// The vendor tag and RSSI exceed the screen width on one line.
// The code places them on separate lines.
// Content starts at UI_CONTENT_Y.
// This leaves space for the log toggle.
// The code stops drawing rows at LIST_BOTTOM.
// The summary line sits just above the status bar.
static const int LIST_Y0 = UI_CONTENT_Y + 2, LIST_STEP = 30;
static const int LIST_BOTTOM = 204;   // rows stop here; 206..220 is the summary line

// Track drawn content for each row and the summary.
// The action row clears itself.
// The code skips a full screen clear.
// The code only erases changed rows.
// The code only erases the changed summary line.
static char prevRow[MAX_ROWS][UI_LIST_SIG_LEN];
static char prevSummary[48] = "";

static void drawRows() {
  Btn row[1] = {{0, 0, 0, 0, logging ? "log: on" : "log: off"}};
  uiDrawActionRow(row, 1);
  logBtn = row[0];

  int maxVisible = (LIST_BOTTOM - LIST_Y0) / LIST_STEP;
  if (maxVisible > MAX_ROWS) maxVisible = MAX_ROWS;
  if (maxVisible < 0) maxVisible = 0;
  int shown = min(rowCount, maxVisible);

  uiDrawListIfChanged(0, LIST_Y0, tft.width(), LIST_STEP, shown, maxVisible, prevRow,
    [](int i, char *sig, size_t cap) {
      BleClass cls = rows[i].cls;
      if (cls != BC_NONE) snprintf(sig, cap, "%d|%-13.13s|%-7s|%d", cls, rows[i].name.c_str(), bleClassLabel(cls), rows[i].rssi);
      else                snprintf(sig, cap, "%d|%-13.13s|%s|%d", cls, rows[i].name.c_str(), macVendorTag(rows[i].mac).c_str(), rows[i].rssi);
    },
    [](int i) {
      int y = LIST_Y0 + i * LIST_STEP;
      BleClass cls = rows[i].cls;
      if (cls != BC_NONE) tft.fillRect(0, y, 3, LIST_STEP - 6, bleClassColor(cls));   // left edge marker, spans both lines
      tft.setTextColor(cls != BC_NONE ? bleClassColor(cls) : ILI9341_WHITE);
      tft.setTextSize(2);
      tft.setCursor(6, y);
      char nm[14];
      snprintf(nm, sizeof(nm), "%-13.13s", rows[i].name.c_str());
      tft.print(nm);
      tft.setTextSize(1);
      tft.setTextColor(accentLabel());
      tft.setCursor(6, y + 18);
      if (cls != BC_NONE) tft.printf("%-7s %ddBm", bleClassLabel(cls), rows[i].rssi);
      else                tft.printf("%s %ddBm", macVendorTag(rows[i].mac).c_str(), rows[i].rssi);
    });
  tft.setTextSize(1);

  // Show the notable device summary always.
  // The code displays green text when no devices appear.
  // This makes new devices obvious.
  // The user does not need to search the list.
  char sum[48];
  if (nFlipper == 0 && nGlasses == 0) snprintf(sum, sizeof(sum), "none");
  else                                snprintf(sum, sizeof(sum), "Flipper:%d Glasses:%d", nFlipper, nGlasses);
  if (uiFieldChanged(prevSummary, sizeof(prevSummary), sum)) {
    uiClearRect(0, 206, tft.width(), 14);
    tft.setCursor(4, 208);
    if (nFlipper == 0 && nGlasses == 0) {
      tft.setTextColor(ILI9341_GREEN);
      tft.print("no Flipper / glasses seen");
    } else {
      tft.setTextColor(ILI9341_MAGENTA);
      tft.printf("Flipper:%d  ", nFlipper);
      tft.setTextColor(ILI9341_ORANGE);
      tft.printf("Glasses:%d", nGlasses);
    }
  }
}

static void drawDetail() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn row[2] = {{0, 0, 0, 0, "Track"}, {0, 0, 0, 0, "Flag"}};
  uiDrawActionRow(row, 2);
  trackBtn = row[0];
  flagBtn  = row[1];

  const BleRow &r = rows[selected];
  tft.setTextSize(1);
  tft.setTextColor(accentLabel());
  int y = UI_CONTENT_Y + 4;
  tft.setCursor(4, y);  tft.printf("Name: %s", r.name.c_str());               y += 16;
  tft.setCursor(4, y);  tft.printf("MAC:  %s", r.macStr.c_str());             y += 16;
  tft.setCursor(4, y);  tft.printf("Vendor: %s", macVendorTag(r.mac).c_str()); y += 16;
  tft.setCursor(4, y);  tft.printf("RSSI: %d dBm", r.rssi);
}

static void drawLocateChrome() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn row[1] = {{0,0,0,0, muted ? "unmute" : "mute"}};
  uiDrawActionRow(row, 1);
  muteBtn = row[0];
  tft.setTextColor(accentLabel());
  tft.setTextSize(2);
  tft.setCursor(4, UI_CONTENT_Y + 6);
  tft.print(rows[selected].name);
  tft.drawRect(4, UI_CONTENT_Y + 73, tft.width() - 8, 26, ILI9341_WHITE);
  lastRssiShown = -999;
}

void bleScanEnter() {
  subMode = LIST;
  logging = false;   // fresh each entry; the file is closed in bleScanExit()
  memset(prevRow, 0, sizeof(prevRow));   // uiShowLoading() below wipes the content area -- forget what drawRows() thinks is on screen
  prevSummary[0] = '\0';
  uiDrawTopBar("BLE Scan");
  if (demoModeEnabled()) {
    uiShowLoading("Scanning...");
    startScan(false);
    return;
  }
  uiShowLoading("Initializing radio...");
  if (!pBLEScan) {
    WiFi.disconnect(true, false);   // radio coexistence -- see README
    WiFi.mode(WIFI_OFF);
    delay(50);
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
  }
  uiShowLoading("Scanning...");
  startScan(false);
}

void bleScanLoop() {
  // Keep the display timeout clock fresh.
  // The code calls powerNoteActivity every tick.
  // The code runs this unconditionally.
  // The scan early return skips the locate branch.
  // This keeps the screen awake during tracking.
  if (subMode == LOCATE) powerNoteActivity();
  if (scanInFlight) {
    if (!bleDone) return;         // still scanning -- touch keeps being read/handled every frame regardless
    bool wasLocate = locateModeScan;
    int rssi = finishScan();
    // Guard against a scan that was started in one mode completing after
    // the user has already navigated elsewhere (possible now that this
    // doesn't block -- a back-button tap mid-scan can switch subMode
    // before the callback resolves). Only draw/beep for the mode this
    // scan was actually for, and only if still there.
    if (wasLocate && subMode == LOCATE) {
      uiDrawLocateReading(4, UI_CONTENT_Y + 36, tft.width() - 8,
                           accentLabel(), -95, -40, &lastRssiShown, rssi);
      beepHold(!muted);              // keep the amp warm so short chirps aren't swallowed
      if (!muted) rangeBeep(rssi);   // rate + pitch scale with signal as a range proxy
    } else if (!wasLocate && subMode == LIST) {
      drawRows();
    }
    return;
  }
  if (subMode == LIST) {
    if (forceListScan || millis() - lastScan > 3000) { forceListScan = false; startScan(false); }
  } else if (subMode == LOCATE) {
    startScan(true);   // self-paces at ~SCAN_SECONDS per cycle; no extra gating needed
  }
}

void bleScanTouch(const TouchPoint &t) {
  if (subMode == LIST) {
    if (!t.isNewPress) return;   // Use manual bounds checking for touch. The code skips the standard button helper. This method requires its own edge guard
    if (uiTouchInButton(t, logBtn)) {
      if (!logging) {
        logging = wlogOpen("blescan", "utc,name,mac,vendor,rssi");
      } else {
        logging = false;
        wlogClose();
      }
      drawRows();
      uiWaitForRelease();
      return;
    }
    for (int i = 0; i < rowCount; i++) {
      int rowY = LIST_Y0 + i * LIST_STEP;
      if (rowY + LIST_STEP > LIST_BOTTOM) break;   // wasn't drawn
      if (t.y >= rowY - 2 && t.y < rowY + LIST_STEP - 2) {
        selected = i;
        subMode = DETAIL;
        uiDrawTopBar("BLE Scan");
        drawDetail();
        uiWaitForRelease();
        return;
      }
    }
    return;
  }
  if (subMode == DETAIL) {
    if (uiTouchInButton(t, trackBtn)) {
      subMode = LOCATE;
      drawLocateChrome();
      uiWaitForRelease();
      return;
    }
    if (uiTouchInButton(t, flagBtn)) {
      uiWaitForRelease();
      const BleRow &r = rows[selected];
      uint8_t sev = (r.cls != BC_NONE) ? UI_SEV_WATCH : UI_SEV_OK;
      flagDetectionShow("ble_scan", sev);   // this screen already has its own detail view (above) -- no need for showDetectionDetail()
      uiDrawTopBar("BLE Scan");
      drawDetail();
      return;
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

// The back button steps up one level.
// Detail and locate modes return to the list.
// The list mode exits to the main loop.
// This keeps the back button consistent.
// The code avoids extra list buttons.
bool bleScanHandleBack() {
  if (subMode == LIST) return false;
  beepHold(false);
  subMode = LIST;
  memset(prevRow, 0, sizeof(prevRow));   // uiShowLoading() below wipes the content area -- forget what drawRows() thinks is on screen
  prevSummary[0] = '\0';
  uiDrawTopBar("BLE Scan");
  uiShowLoading("Scanning...");
  // Can't call start() again if a scan (quite possibly a LOCATE one) is
  // still in flight from before this back-tap -- bleScanLoop()'s subMode
  // guard will just drop that scan's result since subMode is LIST now,
  // and forceListScan makes sure a fresh one starts the moment it's
  // free, rather than waiting out the normal 3s refresh interval.
  if (!scanInFlight) startScan(false);
  else                forceListScan = true;
  return true;
}

void bleScanExit() {
  beepHold(false);
  logging = false;
  wlogClose();
  if (pBLEScan) { pBLEScan->stop(); pBLEScan = nullptr; }
  if (BLEDevice::getInitialized()) BLEDevice::deinit(false);   // radio coexistence -- see README; re-inits via the !pBLEScan guard
  scanInFlight = false;
  forceListScan = false;
}
