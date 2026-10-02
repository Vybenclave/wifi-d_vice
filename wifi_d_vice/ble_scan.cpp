#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include "ui.h"
#include "mac_vendor.h"
#include "wlog.h"
#include "known_signatures.h"
#include "devtime.h"
#include "accent.h"

enum SubMode { LIST, DETAIL, LOCATE };
static SubMode subMode = LIST;
static BLEScan *pBLEScan = nullptr;
static uint32_t lastScan = 0;
static const int MAX_ROWS = 10;

// Notable-device classification layered onto the plain scan list. Two
// classes worth calling out on sight (Marauder's Flipper Sniff / Meta
// Detect, Wireless Wizard's Flipper Detect / Meta Glasses Detect):
//   FLIPPER  -- a Flipper Zero: BLE name begins "Flipper ".
//   GLASSES  -- Ray-Ban / Meta smart glasses: name match, or a Meta /
//               Luxottica company id in the manufacturer data.
// Signatures live in known_signatures.h. Passive: this only reads the
// advertisement fields the scan already collected.
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
// Counts across the WHOLE last scan (not just the MAX_ROWS shown), for the
// summary line under the list.
static int nFlipper = 0, nGlasses = 0;
static int selected = -1;
static Btn trackBtn, muteBtn;
static bool muted = false;
static int lastRssiShown = -999;
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
  // Log every device the scan saw (not just the MAX_ROWS shown), one row
  // each, then a single flush for the batch.
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
  pBLEScan->clearResults();
}

// Bigger list: name at text size 2, vendor/RSSI in a size-1 line below it --
// the vendor tag (up to 23 chars, see macVendorTag()) plus RSSI ran off the
// right edge of the screen when squeezed onto the same line as the name, so
// it gets its own line and the full row width instead. Content starts at
// UI_CONTENT_Y (not the plain 29) to leave room for the log toggle in the
// action row -- see the UI rule in ui.h. The last visible row is held back
// to LIST_BOTTOM so the notable-device summary line has a fixed home just
// above the status bar.
static const int LIST_Y0 = UI_CONTENT_Y + 2, LIST_STEP = 30;
static const int LIST_BOTTOM = 204;   // rows stop here; 206..220 is the summary line

// Per-row / summary "what's currently drawn" signatures for
// uiDrawListIfChanged() / uiFieldChanged() -- uiDrawActionRow() already
// self-clears its own band, so drawRows() no longer needs a uiClearBelow()
// up front; only rows (and the summary line) whose content changed get
// erased and reprinted.
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

  // Notable-device summary -- always shown (green "none" when clear) so a
  // Flipper/glasses appearing is unmistakable without hunting the list.
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
  // Action row, not a bottom-pinned footer -- matches drawLocateChrome()
  // below (and the UI rule in ui.h). This used to sit at tft.height()-44,
  // stranded far below this short 4-line info block on a tall screen --
  // looked like the button had "fallen" to the bottom of the screen.
  uiClearBelow(UI_ACTIONROW_Y);
  Btn row[1] = {{0, 0, 0, 0, "Track"}};
  uiDrawActionRow(row, 1);
  trackBtn = row[0];

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

  uiDrawLocateReading(4, UI_CONTENT_Y + 36, tft.width() - 8,
                       accentLabel(), -95, -40, &lastRssiShown, rssi);

  beepHold(!muted);              // keep the amp warm so short chirps aren't swallowed
  if (!muted) rangeBeep(rssi);   // rate + pitch scale with signal as a range proxy
}

void bleScanEnter() {
  subMode = LIST;
  logging = false;   // fresh each entry; the file is closed in bleScanExit()
  memset(prevRow, 0, sizeof(prevRow));   // uiShowLoading() below wipes the content area -- forget what drawRows() thinks is on screen
  prevSummary[0] = '\0';
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
  memset(prevRow, 0, sizeof(prevRow));   // uiShowLoading() below wipes the content area -- forget what drawRows() thinks is on screen
  prevSummary[0] = '\0';
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
