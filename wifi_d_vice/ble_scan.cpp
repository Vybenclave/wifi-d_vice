#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include "ui.h"
#include "mac_vendor.h"
#include "wlog.h"

enum SubMode { LIST, DETAIL, LOCATE };
static SubMode subMode = LIST;
static BLEScan *pBLEScan = nullptr;
static uint32_t lastScan = 0;
static const int MAX_ROWS = 10;
struct BleRow { String name; uint8_t mac[6]; String macStr; int rssi; };
static BleRow rows[MAX_ROWS];
static int rowCount = 0;
static int selected = -1;
static Btn trackBtn, muteBtn;
static bool muted = false;
static int lastRssiShown = -1000;
// LIST-mode SD logging: an action-row toggle (default off). While on, every
// completed scan writes one row per device via the shared wlog. Opened on
// toggle-on, closed on toggle-off and on bleScanExit().
static Btn logBtn;
static bool logging = false;

// BLE scans block for their whole duration with no channel-restricted fast
// mode like WiFi has -- kept short (1s, was 2s) so back-button taps and
// screen transitions aren't starved as long between checks.
static const uint32_t SCAN_SECONDS = 1;

static void doScan() {
  BLEScanResults *results = pBLEScan->start(SCAN_SECONDS, false);
  int total = (int)results->getCount();
  rowCount = min(total, MAX_ROWS);
  for (int i = 0; i < rowCount; i++) {
    BLEAdvertisedDevice d = results->getDevice(i);
    rows[i].name = d.haveName() ? String(d.getName().c_str()) : "(no name)";
    memcpy(rows[i].mac, d.getAddress().getNative(), 6);
    rows[i].macStr = String(d.getAddress().toString().c_str());
    rows[i].rssi = d.getRSSI();
  }
  // Log every device the scan saw (not just the MAX_ROWS shown), one row
  // each, then a single flush for the batch.
  if (logging) {
    for (int i = 0; i < total; i++) {
      BLEAdvertisedDevice d = results->getDevice(i);
      uint8_t mac[6];
      memcpy(mac, d.getAddress().getNative(), 6);
      char line[176];
      snprintf(line, sizeof(line), "%lu,%s,%s,%s,%d",
               (unsigned long)millis(),
               d.haveName() ? d.getName().c_str() : "(no name)",
               d.getAddress().toString().c_str(),
               macVendorTag(mac).c_str(), d.getRSSI());
      wlogRow(line);
    }
    wlogFlush();
  }
  pBLEScan->clearResults();
}

// Bigger list: name at text size 2, vendor/RSSI in a size-1 tail. Content
// starts at UI_CONTENT_Y (not the plain 29) to leave room for the log
// toggle in the action row -- see the UI rule in ui.h.
static const int LIST_Y0 = UI_CONTENT_Y + 2, LIST_STEP = 20;

static void drawRows() {
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
    snprintf(nm, sizeof(nm), "%-13.13s", rows[i].name.c_str());
    tft.print(nm);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(4 + 13 * 12 + 4, y + 5);
    tft.printf("%s %ddBm", macVendorTag(rows[i].mac).c_str(), rows[i].rssi);
    y += LIST_STEP;
  }
  tft.setTextSize(1);
}

static void drawDetail() {
  uiClearBelow(29);
  const BleRow &r = rows[selected];
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(4, 34);  tft.printf("Name: %s", r.name.c_str());
  tft.setCursor(4, 50);  tft.printf("MAC:  %s", r.macStr.c_str());
  tft.setCursor(4, 66);  tft.printf("Vendor: %s", macVendorTag(r.mac).c_str());
  tft.setCursor(4, 82);  tft.printf("RSSI: %d dBm", r.rssi);

  trackBtn = {4, tft.height() - 44, tft.width() - 8, 36, "Track"};
  uiDrawMenuButton(trackBtn);
}

static void drawLocateChrome() {
  uiClearBelow(UI_ACTIONROW_Y);
  Btn row[1] = {{0,0,0,0, muted ? "unmute" : "mute"}};
  uiDrawActionRow(row, 1);
  muteBtn = row[0];
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(4, UI_CONTENT_Y + 6);
  tft.print(rows[selected].name);
  tft.drawRect(4, UI_CONTENT_Y + 70, tft.width() - 8, 30, ILI9341_WHITE);
  lastRssiShown = -1000;
}

static void updateLocate() {
  if (millis() - lastScan < 250) return;   // BLEScanResults blocks for SCAN_SECONDS itself; this just avoids a tight spin
  lastScan = millis();

  BLEScanResults *results = pBLEScan->start(SCAN_SECONDS, false);
  int rssi = -100;
  for (int i = 0; i < (int)results->getCount(); i++) {
    BLEAdvertisedDevice d = results->getDevice(i);
    uint8_t mac[6];
    memcpy(mac, d.getAddress().getNative(), 6);
    if (memcmp(mac, rows[selected].mac, 6) == 0) { rssi = d.getRSSI(); break; }
  }
  pBLEScan->clearResults();

  if (rssi != lastRssiShown) {
    lastRssiShown = rssi;
    tft.fillRect(4, UI_CONTENT_Y + 36, tft.width() - 8, 34, ILI9341_BLACK);
    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(3);
    tft.setCursor(4, UI_CONTENT_Y + 36);
    tft.printf("%4d dBm", rssi);

    int barW = map(constrain(rssi, -95, -40), -95, -40, 0, tft.width() - 8);
    tft.fillRect(5, UI_CONTENT_Y + 71, tft.width() - 10, 28, ILI9341_BLACK);
    tft.fillRect(5, UI_CONTENT_Y + 71, barW, 28, rssi > -60 ? ILI9341_GREEN : (rssi > -80 ? ILI9341_YELLOW : ILI9341_RED));
  }

  beepHold(!muted);              // keep the amp warm so short chirps aren't swallowed
  if (!muted) rangeBeep(rssi);   // rate + pitch scale with signal as a range proxy
}

void bleScanEnter() {
  subMode = LIST;
  logging = false;   // fresh each entry; the file is closed in bleScanExit()
  uiDrawTopBar("BLE Scan");
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
  doScan();
  drawRows();
  lastScan = millis();
}

void bleScanLoop() {
  if (subMode == LIST) {
    if (millis() - lastScan > 3000) {
      doScan();
      drawRows();
      lastScan = millis();
    }
  } else if (subMode == LOCATE) {
    updateLocate();
  }
}

void bleScanTouch(const TouchPoint &t) {
  if (subMode == LIST) {
    if (!t.isNewPress) return;   // manual bounds check below, not uiTouchInButton() -- needs its own edge guard
    if (uiTouchInButton(t, logBtn)) {
      if (!logging) {
        logging = wlogOpen("blescan", "millis,name,mac,vendor,rssi");
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
      if (rowY + 16 > tft.height() - 2) break;   // wasn't drawn
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
      lastScan = 0;
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
// LOCATE -> the device list; only from the list does it fall through to the
// main loop and leave the screen. Keeps the one back button consistent
// with every other screen instead of needing an in-content "list" button.
bool bleScanHandleBack() {
  if (subMode == LIST) return false;
  beepHold(false);
  subMode = LIST;
  uiDrawTopBar("BLE Scan");
  uiShowLoading("Scanning...");
  doScan();
  drawRows();
  return true;
}

void bleScanExit() {
  beepHold(false);
  logging = false;
  wlogClose();
  if (pBLEScan) { pBLEScan->stop(); pBLEScan = nullptr; }
  BLEDevice::deinit(false);   // radio coexistence -- see README; re-inits via the !pBLEScan guard
}
