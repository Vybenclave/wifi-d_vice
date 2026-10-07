// Place the power icon in the bottom-left corner. This corner stays free on this screen.
// Users access it frequently.
#include "system_screen.h"
#include <SD.h>
#include <Preferences.h>
#include "qrcode.h"
#include "pins.h"
#include "sd_bus.h"
#include "keyboard.h"
#include "onboarding.h"
#include "engstore.h"
#include "engagement.h"
#include "power.h"
#include "ble_2fa.h"
#include "splash.h"
#include "theme.h"
#include "accent.h"
#include "tz.h"
#include "demomode.h"
#include "devtime.h"
#include "modvis.h"
#include "pincfg.h"
#include "gps_shared.h"

static const char *REPO_URL = "https://github.com/Vybenclave/wifi-d_vice";

static Btn displayBtn, hwBtn, modsBtn, volBtn, wizardBtn, aboutBtn;
static Btn mainPrevBtn, mainNextBtn;
static int mainPage = 0, mainPages = 1;
static bool sdOk = false;

// Power icon, bottom-left corner -- reached constantly, unlike the other
// System sub-pages, so it gets the same corner the main menu's gear claims.
static const int PWR_ICON_R = 10;
static void powerIconCenter(int &cx, int &cy) { cx = 18; cy = tft.height() - 18; }

static void drawPowerIcon() {
  int cx, cy; powerIconCenter(cx, cy);
  const int R = PWR_ICON_R;
  const int PUCK_R = R + 6;                 // 16 -- unchanged, this is the correct puck size
  tft.fillCircle(cx, cy, PUCK_R, ILI9341_BLACK);   // opaque puck against the scene bg

  const int RING_R = PUCK_R - 2;            // 14 -- close to the puck edge, 2px black margin
  const uint16_t glyph = ILI9341_DARKGREY;
  for (int rr = RING_R - 2; rr <= RING_R; rr++) tft.drawCircle(cx, cy, rr, glyph);

  // Cut a gap at the top. Draw the stem through the gap into the ring.
  tft.fillRect(cx - 4, cy - RING_R - 1, 8, 6, ILI9341_BLACK);
  tft.fillRect(cx - 1, cy - RING_R - 1, 3, RING_R - 1, glyph);
}

static bool touchInPowerIcon(const TouchPoint &t) {
  int cx, cy; powerIconCenter(cx, cy);
  int dx = t.x - cx, dy = t.y - cy;
  int rr = PWR_ICON_R + 8;
  return dx * dx + dy * dy <= rr * rr;
}

// Use standard button dimensions. This list uses real pagination when rows exceed screen height.
// Landscape mode requires this layout. The power icon sits separately.
static void drawButtons() {
  uiSetBgMode(UI_BG_IMAGE);   // button screen -> scene background
  uiClearBelow(29);

  static const char *kLabels[6] = {"Display", "Hardware", "Modules", "Beep volume", "Run setup wizard", "About"};
  Btn *const kBtns[6] = {&displayBtn, &hwBtn, &modsBtn, &volBtn, &wizardBtn, &aboutBtn};

  // Set bottom margin to 36 pixels. This clears space for the power icon.
  // The icon extends above the standard status bar margin.
  const int y0 = 34, rowH = 36, gap = 6, bottomMargin = 36, pagerGap = 6;
  int itemsPerPage, pages, pagerY;
  uiPagerLayout(y0, rowH, gap, bottomMargin, pagerGap, 6, 6, 1, itemsPerPage, pages, pagerY);
  mainPages = pages;
  if (mainPage >= pages) mainPage = pages - 1;   // an orientation change can shrink the page count
  if (mainPage < 0) mainPage = 0;

  int base = mainPage * itemsPerPage;
  int n = min(itemsPerPage, 6 - base);
  int y = y0;
  for (int i = 0; i < 6; i++) *kBtns[i] = {0, 0, 0, 0, kLabels[i]};   // off-page buttons never match touch
  for (int i = 0; i < n; i++) {
    int idx = base + i;
    *kBtns[idx] = {8, y, tft.width() - 16, rowH, kLabels[idx]};
    uiDrawMenuButton(*kBtns[idx]);
    y += rowH + gap;
  }
  uiDrawPager(pagerY, mainPage, pages, mainPrevBtn, mainNextBtn);
  drawPowerIcon();
}

// Draw the QR code directly to the display. The function runs synchronously on one thread.
// This avoids cross-task conflicts. `s_qrTop` marks the start position. `s_qrBottom` marks the end position.
static int s_qrTop = 34;
static int s_qrBottom = 34;
static int s_qrX = 0, s_qrPx = 0;   // QR bounding box (for the easter-egg hit test)

static void qrDisplay(esp_qrcode_handle_t qrcode) {
  int side = esp_qrcode_get_size(qrcode);
  // Use integer pixel scaling. This keeps the code crisp and scannable.
  int scale = 100 / side;
  if (scale < 1) scale = 1;
  int px = side * scale;
  int originX = (tft.width() - px) / 2;
  if (originX < 6) originX = 6;
  int originY = s_qrTop;
  tft.fillRect(originX - 6, originY - 6, px + 12, px + 12, ILI9341_WHITE);   // quiet zone
  for (int qy = 0; qy < side; qy++) {
    for (int qx = 0; qx < side; qx++) {
      bool dark = esp_qrcode_get_module(qrcode, qx, qy);
      tft.fillRect(originX + qx * scale, originY + qy * scale, scale, scale,
                   dark ? ILI9341_BLACK : ILI9341_WHITE);
    }
  }
  s_qrBottom = originY + px + 6;   // past the quiet zone
  s_qrX = originX; s_qrPx = px;
}

static void drawAbout() {
  for (;;) {   // outer loop: re-drawn after the easter egg exits
    uiDrawTopBar("About");
    uiClearBelow(29);

    s_qrTop = 46;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qrDisplay;
    cfg.max_qrcode_version = 10;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    esp_qrcode_generate(&cfg, REPO_URL);

    auto centerLine = [&](int y, uint16_t color, const char *s) {
      int16_t bx, by; uint16_t w, h;
      tft.setTextSize(1);
      tft.getTextBounds(s, 0, 0, &bx, &by, &w, &h);
      tft.setTextColor(color);
      tft.setCursor((tft.width() - (int)w) / 2 - bx, y);
      tft.print(s);
    };

    int y = s_qrBottom + 12;
    centerLine(y, accentLabel(),      "WIFI D_VICE  v0.9"); y += 15;
    centerLine(y, ILI9341_WHITE,  "MIT license");                   y += 13;
    centerLine(y, ILI9341_WHITE,  "github.com/Vybenclave/wifi-d_vice"); y += 16;
    char batl[32];
    int bpct = uiBatteryPct();
    if (bpct < 0) snprintf(batl, sizeof(batl), "battery: %d mV (USB / no cell)", uiBatteryMv());
    else          snprintf(batl, sizeof(batl), "battery: %d mV  ~%d%%", uiBatteryMv(), bpct);
    centerLine(y, ILI9341_WHITE, batl);                              y += 16;
    centerLine(y, ILI9341_YELLOW, "scan the code for source + license");

    // Use the top bar back button to exit. Tap the QR code to show splash art.
    for (;;) {
      TouchPoint t = uiReadTouch();
      uiServiceChrome();
      if (t.pressed) powerNoteActivity();
      powerServiceAutoOff();
      if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
      if (t.pressed && t.x >= s_qrX && t.x < s_qrX + s_qrPx &&
          t.y >= s_qrTop && t.y < s_qrTop + s_qrPx) {
        uiWaitForRelease();
        showSplashArt();
        break;   // fall out to the outer loop -> full redraw
      }
      delay(15);
    }
  }
}

void systemEnter() {
  uiDrawTopBar("System");
  themeLoad();
  if (!sdOk) {
    uiShowLoading("Checking SD card...");
    sdBusBegin();
    sdOk = SD.begin(SD_CS, sdSPI);
  }
  mainPage = 0;
  drawButtons();
}

void systemLoop() {}

// Read GPS data from the shared background reader. This avoids UART starvation.
// The reader runs continuously for time sync. Tap the back button to exit.
static void systemTestGps() {
  uiDrawTopBar("Test GPS");
  uiClearBelow(29);
  TinyGPSPlus &gps = gpsShared();
  uint32_t lastDraw = 0;

  // Use a static array to track previous rows. This persists across loop ticks.
  // The screen resets it on entry. The row list changes based on GPS state.
  static char prevRow[10][UI_LIST_SIG_LEN];
  static char prevFooter[48];
  memset(prevRow, 0, sizeof(prevRow));
  prevFooter[0] = '\0';
  const int rowY0 = 36, rowH = 15;

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); break; }

    if (millis() - lastDraw > 400) {
      lastDraw = millis();
      bool anyData = gps.charsProcessed() > 0;
      tft.setTextSize(1);

      struct Row { uint16_t col; const char *k; String v; };
      Row rows[10];
      int n = 0;
      rows[n++] = {ILI9341_WHITE, "RX pin", String(GPS_RX)};
      rows[n++] = {anyData ? ILI9341_GREEN : ILI9341_RED, "serial",
                   anyData ? (String(gps.charsProcessed()) + " bytes") : String("no data")};
      rows[n++] = {gps.sentencesWithFix() ? ILI9341_GREEN : ILI9341_YELLOW, "NMEA ok", String(gps.passedChecksum())};
      rows[n++] = {gps.satellites.isValid() ? ILI9341_GREEN : ILI9341_YELLOW, "sats",
                   gps.satellites.isValid() ? String(gps.satellites.value()) : String("--")};
      rows[n++] = {gps.location.isValid() ? ILI9341_GREEN : ILI9341_YELLOW, "fix",
                   gps.location.isValid() ? String("yes") : String("no fix")};
      if (gps.location.isValid()) {
        rows[n++] = {ILI9341_WHITE, "lat", String(gps.location.lat(), 6)};
        rows[n++] = {ILI9341_WHITE, "lon", String(gps.location.lng(), 6)};
        rows[n++] = {ILI9341_WHITE, "alt m", gps.altitude.isValid() ? String(gps.altitude.meters(), 1) : String("--")};
      }
      rows[n++] = {ILI9341_WHITE, "HDOP", gps.hdop.isValid() ? String(gps.hdop.hdop(), 1) : String("--")};
      if (gps.time.isValid()) {
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
        rows[n++] = {ILI9341_WHITE, "UTC", String(b)};
      }
      rows[n++] = {devTimeSynced() ? ILI9341_GREEN : ILI9341_YELLOW, "dev clock",
                   devTimeSynced() ? devTimeNowString() : String("unsynced")};

      uiDrawListIfChanged(0, rowY0, tft.width(), rowH, n, 10, prevRow,
        [&](int i, char *sig, size_t cap) { snprintf(sig, cap, "%04X|%s|%s", rows[i].col, rows[i].k, rows[i].v.c_str()); },
        [&](int i) {
          tft.setTextColor(accentLabel()); tft.setCursor(4, rowY0 + i * rowH);   tft.print(rows[i].k);
          tft.setTextColor(rows[i].col);   tft.setCursor(100, rowY0 + i * rowH); tft.print(rows[i].v);
        });

      char footer[48] = "";
      if (!anyData) snprintf(footer, sizeof(footer), "no bytes -- GPS TX -> GPIO35, GND, 3V3?");
      if (uiFieldChanged(prevFooter, sizeof(prevFooter), footer)) {
        uiClearRect(0, tft.height() - 16, tft.width(), 14);
        if (footer[0]) {
          tft.setTextColor(ILI9341_YELLOW);
          tft.setCursor(4, tft.height() - 16);
          tft.print(footer);
        }
      }
    }
    delay(5);
  }
}

// Pick an accent color from a swatch grid. Tapping a cell applies it immediately.
// Use standard pager buttons for navigation.
static int pickAccentColor(int current) {
  const int y0 = 34, cols = 2, gap = 8, bottomMargin = UI_STATUSBAR_H + 4, pagerGap = 6;
  const int MIN_CELL_H = 32, MAX_CELL_H = 36;
  // Reserve space for the pager row. Calculate cell height from the total budget.
  // This keeps the pager in a fixed position. Stretch cells to fill the space.
  // Ignore the helper pager Y value.
  int perPage, pages, unstretchedPagerY;
  int rowsFit = uiPagerLayout(y0, MIN_CELL_H, gap, bottomMargin, pagerGap, ACCENT_N, 999, cols, perPage, pages, unstretchedPagerY);
  int cellH = (tft.height() - bottomMargin - UI_PAGER_H - pagerGap - y0 - gap * (rowsFit - 1)) / rowsFit;
  if (cellH > MAX_CELL_H) cellH = MAX_CELL_H;   // stay button-row-sized even with room to spare
  int page = (current >= 0 && current < ACCENT_N) ? current / perPage : 0;
  const int pagerY = y0 + rowsFit * (cellH + gap) - gap + pagerGap;

  Btn cell[ACCENT_N], prevBtn, nextBtn;   // only [0, n) of cell[] is filled in on any given page
  int n = 0;
  auto draw = [&]() {
    uiDrawTopBar("Accent Color");
    uiClearBelow(29);
    int base = page * perPage;
    n = min(perPage, ACCENT_N - base);
    int cellW = (tft.width() - 16 - gap) / cols;

    for (int i = 0; i < n; i++) {
      int id = base + i;
      int col = i % cols, row = i / cols;
      int x = 8 + col * (cellW + gap);
      int y = y0 + row * (cellH + gap);
      cell[i] = {x, y, cellW, cellH, accentName(id)};

      // Render swatches using the active theme. Vice uses filled rounded buttons.
      // Basic uses plain square outlines. The outline color previews the selection.
      uint16_t fill = accentFillFor(id);
      if (themeIsVice()) {
        tft.fillRoundRect(x, y, cellW, cellH, 8, fill);
        tft.drawRoundRect(x, y, cellW, cellH, 8, accentEdgeFor(id));
        tft.setTextColor(uiContrastText(fill));
      } else {
        tft.drawRect(x, y, cellW, cellH, fill);
        tft.setTextColor(fill);
      }
      tft.setTextSize(cellH >= 28 ? 2 : 1);
      int16_t bx, by; uint16_t bw, bh;
      tft.getTextBounds(accentName(id), 0, 0, &bx, &by, &bw, &bh);
      tft.setCursor(x + (cellW - (int)bw) / 2 - bx, y + (cellH - (int)bh) / 2 - by);
      tft.print(accentName(id));

      if (id == current) {   // "current" marker -- a double outline reads on any fill color
        tft.drawRect(x - 3, y - 3, cellW + 6, cellH + 6, ILI9341_GREEN);
        tft.drawRect(x - 2, y - 2, cellW + 4, cellH + 4, ILI9341_GREEN);
      }
    }
    uiDrawPager(pagerY, page, pages, prevBtn, nextBtn);
  };
  draw();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return current; }
    int base = page * perPage;
    for (int i = 0; i < n; i++)
      if (uiTouchInButton(t, cell[i])) { uiWaitForRelease(); return base + i; }
    if (uiTouchInButton(t, prevBtn) && page > 0) { uiWaitForRelease(); page--; draw(); continue; }
    if (uiTouchInButton(t, nextBtn) && page < pages - 1) { uiWaitForRelease(); page++; draw(); continue; }
    delay(15);
  }
}

// Treat theme and accent color as independent settings. Changes apply immediately.
// No pending state requires tracking.
static void systemShowThemeColor() {
  Btn themeBtn, accentBtn, listBgBtn;
  auto draw = [&]() {
    uiDrawTopBar("Theme & Color");
    uiClearBelow(29);
    char tlbl[24], albl[24];
    snprintf(tlbl, sizeof(tlbl), "Theme: %s", themeName(themeGet()));
    snprintf(albl, sizeof(albl), "Accent: %s", accentName(accentGet()));
    themeBtn  = {8, 40, tft.width() - 16, 40, tlbl};
    accentBtn = {8, 88, tft.width() - 16, 40, albl};
    // Show the list background option in all themes. It only works in Vice mode.
    // Basic mode ignores it.
    listBgBtn = {8, 136, tft.width() - 16, 40,
                 uiListBgEnabled() ? "List screens: blurred scene bg"
                                   : "List screens: gradient bg"};
    uiDrawButton(themeBtn);
    uiDrawButton(accentBtn);
    uiDrawButton(listBgBtn);
    // Draw a live swatch next to the accent row. Match the active theme style.
    int sw = 24;
    int sx = accentBtn.x + accentBtn.w - sw - 10, sy = accentBtn.y + (accentBtn.h - sw) / 2;
    if (themeIsVice()) {
      tft.fillRoundRect(sx, sy, sw, sw, 4, accentFill());
      tft.drawRoundRect(sx, sy, sw, sw, 4, accentEdge());
    } else {
      tft.drawRect(sx, sy, sw, sw, accentFill());
    }
  };
  draw();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    if (uiTouchInButton(t, themeBtn)) {
      uiWaitForRelease();
      int chosen = uiDropdownPick("Theme", THEME_N, themeName, themeGet());
      if (chosen != themeGet()) { themeSet(chosen); uiClearBelow(0); }   // wipe old-skin artifacts
      draw();
      continue;
    }
    if (uiTouchInButton(t, accentBtn)) {
      uiWaitForRelease();
      int chosen = pickAccentColor(accentGet());
      if (chosen != accentGet()) { accentSet(chosen); uiClearBelow(0); }
      draw();
      continue;
    }
    if (uiTouchInButton(t, listBgBtn)) {
      uiWaitForRelease();
      uiSetListBgEnabled(!uiListBgEnabled());
      draw();
      continue;
    }
    delay(15);
  }
}

// Adjust only the display clock. System logs stay in UTC.
// Compute row count from screen height. This prevents layout overlap in landscape mode.
static void systemShowTimezone() {
  const int y0 = 34, rowH = 22, gap = 3;
  // Clear the full status bar at the bottom. Pin toggle buttons from the screen bottom.
  // This keeps them in a fixed position. Ignore the helper pager Y value.
  // Recompute page count locally.
  const int TOGGLE_H = 28, APPLY_H = 34, STACK_GAP = 6, BOTTOM_MARGIN = UI_STATUSBAR_H + 4;
  const int MAX_ROWS = 10;
  int stackTop = tft.height() - BOTTOM_MARGIN - APPLY_H - STACK_GAP
                 - TOGGLE_H - STACK_GAP - TOGGLE_H - STACK_GAP - UI_PAGER_H;
  int ignoredItemsPerPage, ignoredPages, ignoredPagerY;
  int ROWS = uiPagerLayout(y0, rowH, gap,
               BOTTOM_MARGIN + APPLY_H + STACK_GAP + TOGGLE_H + STACK_GAP + TOGGLE_H + STACK_GAP, gap,
               tzCount(), MAX_ROWS, 1, ignoredItemsPerPage, ignoredPages, ignoredPagerY);
  if (ROWS < 3) ROWS = 3;

  Btn items[MAX_ROWS], applyBtn, prevBtn, nextBtn, toggleBtn, dstBtn;
  int sel = tzGetIndex();               // pending selection, starts at the active one
  int page = sel / ROWS;

  auto drawPicker = [&]() {
    uiDrawTopBar("Timezone");
    uiClearBelow(29);
    int pages = (tzCount() + ROWS - 1) / ROWS;
    int y = y0;
    int base = page * ROWS;
    int n = min(ROWS, tzCount() - base);
    for (int i = 0; i < n; i++) {
      int idx = base + i;
      items[i] = {8, y, tft.width() - 16, rowH, tzLabel(idx)};
      uiDrawButton(items[i]);
      if (idx == sel)                    // pending selection: cyan outline
        tft.drawRect(items[i].x - 2, items[i].y - 2,
                     items[i].w + 4, items[i].h + 4, ILI9341_CYAN);
      if (idx == tzGetIndex()) {         // "on" marker: currently active zone
        tft.fillRect(tft.width() - 38, y + 3, 32, rowH - 6, uiBgColor(y + 3));
        tft.setTextColor(ILI9341_GREEN);
        tft.setCursor(tft.width() - 34, y + rowH / 2 - 4);
        tft.print("on");
      }
      y += rowH + gap;
    }

    int py = stackTop;
    uiDrawPager(py, page, pages, prevBtn, nextBtn);

    toggleBtn = {8, py + UI_PAGER_H + STACK_GAP, tft.width() - 16, TOGGLE_H,
                 tzUse24h() ? "24-hour clock" : "12-hour clock"};
    uiDrawMenuButton(toggleBtn);

    dstBtn = {8, py + UI_PAGER_H + STACK_GAP + TOGGLE_H + STACK_GAP, tft.width() - 16, TOGGLE_H,
              tzAutoDst() ? "Auto DST: on" : "Auto DST: off"};
    uiDrawMenuButton(dstBtn);

    applyBtn = {8, py + UI_PAGER_H + STACK_GAP + TOGGLE_H + STACK_GAP + TOGGLE_H + STACK_GAP,
                tft.width() - 16, APPLY_H,
                sel == tzGetIndex() ? "Apply (no change)" : "Apply"};
    uiDrawButton(applyBtn);
  };
  drawPicker();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    if (t.pressed && uiTouchInButton(t, applyBtn)) {
      uiWaitForRelease();
      if (sel != tzGetIndex()) { tzSetIndex(sel); drawPicker(); }
      continue;
    }
    if (t.pressed && uiTouchInButton(t, toggleBtn)) {
      uiWaitForRelease();
      tzSet24h(!tzUse24h());
      drawPicker();
      continue;
    }
    if (t.pressed && uiTouchInButton(t, dstBtn)) {
      uiWaitForRelease();
      tzSetAutoDst(!tzAutoDst());
      drawPicker();
      continue;
    }
    int pages = (tzCount() + ROWS - 1) / ROWS;
    if (t.pressed && uiTouchInButton(t, prevBtn) && page > 0) {
      uiWaitForRelease(); page--; drawPicker(); continue;
    }
    if (t.pressed && uiTouchInButton(t, nextBtn) && page < pages - 1) {
      uiWaitForRelease(); page++; drawPicker(); continue;
    }
    int base = page * ROWS, n = min(ROWS, tzCount() - base);
    for (int i = 0; i < n; i++) {
      if (t.pressed && uiTouchInButton(t, items[i])) {
        uiWaitForRelease();
        sel = base + i;
        drawPicker();
        break;
      }
    }
    delay(15);
  }
}

// A 2x2 grid, one button per rotation, each labeled with its own preview
// text rotated to match the orientation it would apply -- so you pick the
// exact one you want instead of blind-cycling through all four.
void systemShowRotationPicker() {
  static const char *kLabels[4] = {"0", "90", "180", "270"};
  uiClearBelow(0);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.print("Pick orientation");

  Btn cells[4];
  int cw = (tft.width() - 12) / 2, ch = (tft.height() - 40) / 2;
  for (int i = 0; i < 4; i++) {
    int col = i % 2, row = i / 2;
    cells[i] = {4 + col * cw, 20 + row * ch, cw - 4, ch - 4, kLabels[i]};
    tft.drawRect(cells[i].x, cells[i].y, cells[i].w, cells[i].h, ILI9341_WHITE);
    uiDrawRotatedText(cells[i].x + cells[i].w / 2, cells[i].y + cells[i].h / 2,
                       kLabels[i], i, 2, accentLabel());
  }

  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    for (int i = 0; i < 4; i++) {
      if (uiTouchInButton(t, cells[i])) {
        uiWaitForRelease();
        uiSetRotation(i);
        return;
      }
    }
  }
}

void systemShowVolumePicker() {
  Btn minusBtn = {8, 60, 60, 44, "-"};
  Btn plusBtn = {tft.width() - 68, 60, 60, 44, "+"};
  Btn testBtn = {8, 120, tft.width() - 16, 40, "test beep"};

  auto draw = [&]() {
    uiDrawTopBar("Beep volume");
    uiClearBelow(29);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(3);
    tft.setCursor(tft.width() / 2 - 24, 66);
    tft.printf("%3d%%", uiGetBeepVolume());
    uiDrawMenuButton(minusBtn);
    uiDrawMenuButton(plusBtn);
    uiDrawMenuButton(testBtn);
  };
  // Keep the DAC channel open during this screen. Rapid beep calls latch the DMA.
  // This prevents audio cutoff until reboot.
  beepHold(true);
  draw();

  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInBackButton(t)) { uiWaitForRelease(); beepHold(false); return; }
    if (uiTouchInButton(t, minusBtn)) {
      uiSetBeepVolume(uiGetBeepVolume() - 10);
      draw();
      beep(200, 1800);
      uiWaitForRelease();
    } else if (uiTouchInButton(t, plusBtn)) {
      uiSetBeepVolume(uiGetBeepVolume() + 10);
      draw();
      beep(200, 1800);
      uiWaitForRelease();
    } else if (uiTouchInButton(t, testBtn)) {
      beep(200, 1800);
      uiWaitForRelease();
    }
  }
}

// Delete all files recursively. This mimics a format without touching the filesystem structure.
static void wipeDir(const String &path) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File entry = dir.openNextFile();
  while (entry) {
    String entryPath = entry.path();
    bool isDir = entry.isDirectory();
    entry.close();
    if (isDir) { wipeDir(entryPath); SD.rmdir(entryPath); }
    else SD.remove(entryPath);
    entry = dir.openNextFile();
  }
  dir.close();
}

static void doFormat() {
  uiClearBelow(0);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 100);
  tft.print("Erasing SD card...");
  wipeDir("/");
  // Clear all saved data after wiping the card. This removes Wi-Fi profiles,
  // engagement markers, BLE bonds, and loaded keys.
  engStoreClear();
  engagementResetAll();
  ble2faBegin();
  ble2faClearBonds();
  uiClearBelow(0);
  tft.setCursor(10, 100);
  tft.print("Done.");
  delay(600);
}

// Require typing to confirm deletion. This prevents accidental taps on the touchscreen.
static bool confirmFormat() {
  uiClearBelow(0);
  tft.setTextColor(ILI9341_RED);
  tft.setTextSize(2);
  tft.setCursor(10, 60);
  tft.print("ERASE ALL DATA");
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 100);
  tft.print("This deletes every file and");
  tft.setCursor(10, 116);
  tft.print("folder on the SD card. This");
  tft.setCursor(10, 132);
  tft.print("cannot be undone.");
  // Position the cancel button below the status bar. Add a small gap for clarity.
  const int cancelY = tft.height() - UI_STATUSBAR_H - 4 - 36;
  Btn confirmBtn = {10, cancelY - 8 - 36, tft.width() - 20, 36, "type ERASE to confirm"};
  Btn cancelBtn = {10, cancelY, tft.width() - 20, 36, "cancel"};
  uiDrawButton(confirmBtn);
  uiDrawButton(cancelBtn);
  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInButton(t, confirmBtn)) {
      uiWaitForRelease();
      String typed = uiTextInput("Type ERASE to confirm", "", false);
      return typed == "ERASE";
    }
    if (uiTouchInButton(t, cancelBtn)) { uiWaitForRelease(); return false; }
  }
}

// Hide modules from their submenus. Use standard pagination.
// This prevents rows from drawing off the screen.
static void systemShowModules() {
  const int y0 = 38, rowH = 22, gap = 3, bottomMargin = UI_STATUSBAR_H + 4, pagerGap = 6;
  const int MAX_ROWS = 10;
  int itemsPerPage, pages, pagerY;
  uiPagerLayout(y0, rowH, gap, bottomMargin, pagerGap, MOD_N, MAX_ROWS, 1, itemsPerPage, pages, pagerY);

  Btn rows[MAX_ROWS], prevBtn, nextBtn;
  int page = 0;
  int n = 0;   // rows actually drawn on the current page -- read back in the touch loop below

  auto drawRow = [&](int i) {   // i = index into the CURRENT page's rows[], item = base+i
    int idx = page * itemsPerPage + i;
    int y = y0 + i * (rowH + gap);
    rows[i] = {8, y, tft.width() - 16, rowH, modvisName(idx)};
    uiDrawMenuButton(rows[i]);
    bool hidden = modvisHidden(idx);
    int cx = tft.width() - 26, cy = y + rowH / 2;
    tft.fillRect(cx - 8, cy - 8, 16, 16, uiBgColor(cy - 8));
    if (hidden) tft.fillCircle(cx, cy, 2, ILI9341_DARKGREY);      // grey dot
    else        tft.fillCircle(cx, cy, 6, ILI9341_GREEN);         // green circle
  };

  auto drawPage = [&]() {
    uiDrawTopBar("Modules");
    uiClearBelow(29);
    int base = page * itemsPerPage;
    n = min(itemsPerPage, MOD_N - base);
    for (int i = 0; i < n; i++) drawRow(i);
    uiDrawPager(pagerY, page, pages, prevBtn, nextBtn);
  };
  drawPage();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    if (t.pressed && uiTouchInButton(t, prevBtn) && page > 0) {
      uiWaitForRelease(); page--; drawPage(); continue;
    }
    if (t.pressed && uiTouchInButton(t, nextBtn) && page < pages - 1) {
      uiWaitForRelease(); page++; drawPage(); continue;
    }
    for (int i = 0; i < n; i++) {
      if (t.pressed && uiTouchInButton(t, rows[i])) {
        int idx = page * itemsPerPage + i;
        modvisSetHidden(idx, !modvisHidden(idx));
        drawRow(i);
        uiWaitForRelease();
      }
    }
    delay(15);
  }
}

// Configure add-on radio pins. Dim rows for unselected radios.
// Settings save to NVS. Changes apply on reboot. Hold BOOT to reset.
static void systemShowPins() {
  Btn cc1101Box, r24Btn[3], setBtn[PIN_N], resetBtn;
  static const char *R24[3] = { "none", "nRF24", "CC2500" };

  auto pinDim = [](int i) {
    if (i == PIN_CC1101_CS || i == PIN_CC1101_GDO0) return !pincfgCC1101();
    if (i == PIN_RADIO24_CS || i == PIN_RADIO24_IRQ)
      return pincfgRadio24() == RADIO24_NONE;
    return false;
  };

  auto draw = [&]() {
    uiDrawTopBar("SPI / IRQ pins");
    uiClearBelow(29);
    tft.setTextSize(1);

    // CC1101 installed
    bool cc = pincfgCC1101();
    cc1101Box = {8, 32, tft.width() - 16, 18, ""};
    tft.drawRect(cc1101Box.x, cc1101Box.y, 14, 14, ILI9341_WHITE);
    if (cc) {
      tft.drawLine(cc1101Box.x + 2, cc1101Box.y + 7, cc1101Box.x + 5, cc1101Box.y + 11, ILI9341_GREEN);
      tft.drawLine(cc1101Box.x + 5, cc1101Box.y + 11, cc1101Box.x + 13, cc1101Box.y + 2, ILI9341_GREEN);
    }
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(cc1101Box.x + 22, cc1101Box.y + 4);
    tft.print("CC1101 sub-GHz radio");

    // 2.4 GHz radio: none / nRF24 / CC2500 (pick one)
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(8, 58);
    tft.print("2.4 GHz:");
    int r24 = pincfgRadio24();
    int bx0 = 62, bw = (tft.width() - 8 - bx0) / 3;
    for (int k = 0; k < 3; k++) {
      r24Btn[k] = {bx0 + k * bw, 52, bw - 3, 20, R24[k]};
      if (k == r24) uiDrawButton(r24Btn[k]);
      else          uiDrawButtonDim(r24Btn[k]);
    }

    // pin rows
    int yy = 84;
    for (int i = 0; i < PIN_N; i++) {
      bool dim = pinDim(i);
      tft.setTextColor(dim ? 0x8410 : ILI9341_WHITE);
      tft.setCursor(8, yy);
      tft.print(pincfgName(i));
      int g = pincfgGet(i), d = pincfgDefault(i);
      tft.setTextColor(dim ? 0x8410 : accentLabel());
      tft.setCursor(98, yy);
      if (g < 0) tft.print("none"); else tft.printf("GPIO %d", g);
      if (d >= 0) {
        tft.setTextColor(0x630C);
        tft.setCursor(150, yy);
        tft.printf("def %d", d);
      }
      setBtn[i] = {tft.width() - 44, yy - 4, 38, 20, "set"};
      uiDrawButton(setBtn[i]);
      yy += 22;
    }

    int ry = yy + 8;
    resetBtn = {8, ry, tft.width() - 16, 24, "reset to defaults"};
    uiDrawButton(resetBtn);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(8, ry + 32);
    tft.print("Reboot to apply. BOOT on power-up resets.");
  };
  draw();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInBackButton(t)) { uiWaitForRelease(); return; }

    if (uiTouchInButton(t, cc1101Box)) {
      pincfgSetCC1101(!pincfgCC1101());
      draw(); uiWaitForRelease(); continue;
    }
    bool handled = false;
    for (int k = 0; k < 3 && !handled; k++) {
      if (uiTouchInButton(t, r24Btn[k])) {
        pincfgSetRadio24(k);
        draw(); uiWaitForRelease(); handled = true;
      }
    }
    if (handled) continue;
    if (uiTouchInButton(t, resetBtn)) {
      pincfgResetAll();
      draw(); uiWaitForRelease(); continue;
    }
    for (int i = 0; i < PIN_N && !handled; i++) {
      if (uiTouchInButton(t, setBtn[i])) {
        uiWaitForRelease();
        String prompt = String(pincfgName(i)) + " GPIO (-1 = none)";
        String in = uiTextInput(prompt.c_str(), String(pincfgGet(i)), false);
        if (in.length()) {
          int v = in.toInt();
          if ((v == 0 && in != "0") || v < -1 || v > 39) uiToast("GPIO must be -1..39");
          else pincfgSet(i, v);
        }
        draw(); handled = true;
      }
    }
  }
}

// Group sub-screens in a paginated list. Use standard row dimensions.
// Pagination replaces the old fixed array.
// label_fn (optional) overrides `label` at draw time -- used for toggle
// rows so the button text tracks the setting after run() flips it.
struct SysItem { const char *label; void (*run)(); const char *(*label_fn)(); };

static void systemSubPage(const char *title, const SysItem *items, int n) {
  const int y0 = 38, rowH = 36, gap = 6, bottomMargin = UI_STATUSBAR_H + 4, pagerGap = 6;
  const int MAX_ROWS = 10;
  int itemsPerPage, pages, pagerY;
  uiPagerLayout(y0, rowH, gap, bottomMargin, pagerGap, n, MAX_ROWS, 1, itemsPerPage, pages, pagerY);

  Btn rows[MAX_ROWS], prevBtn, nextBtn;
  int page = 0;
  int shown = 0;   // rows actually drawn on the current page -- read back in the touch loop below

  auto draw = [&]() {
    uiDrawTopBar(title);
    uiSetBgMode(UI_BG_IMAGE);   // button screen -> scene background
    uiClearBelow(29);
    int base = page * itemsPerPage;
    shown = min(itemsPerPage, n - base);
    int y = y0;
    for (int i = 0; i < shown; i++) {
      int idx = base + i;
      rows[i] = {8, y, tft.width() - 16, rowH,
                 items[idx].label_fn ? items[idx].label_fn() : items[idx].label};
      uiDrawMenuButton(rows[i]);
      y += rowH + gap;
    }
    uiDrawPager(pagerY, page, pages, prevBtn, nextBtn);
  };
  draw();
  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    if (uiTouchInButton(t, prevBtn) && page > 0) { uiWaitForRelease(); page--; draw(); continue; }
    if (uiTouchInButton(t, nextBtn) && page < pages - 1) { uiWaitForRelease(); page++; draw(); continue; }
    int base = page * itemsPerPage;
    for (int i = 0; i < shown; i++) {
      if (uiTouchInButton(t, rows[i])) {
        uiWaitForRelease();
        items[base + i].run();
        draw();
      }
    }
    delay(15);
  }
}

static void sysFormat() { if (sdOk && confirmFormat()) doFormat(); }

static void sysToggleSplash()   { splashSetEnabled(!splashEnabled()); }
static const char *splashLbl()  { return splashEnabled() ? "Boot splash (on)"
                                                         : "Boot splash (off)"; }

static void systemShowDisplayMenu() {
  static const SysItem items[] = {
    {"Screen orientation", systemShowRotationPicker,          nullptr},
    {"Theme & Color",      systemShowThemeColor,              nullptr},
    {"Timezone",           systemShowTimezone,                nullptr},
    {"Recalibrate touch",  uiRunCalibration,                  nullptr},
    {nullptr,              sysToggleSplash,                   splashLbl},
    {"Display Timeout",    powerShowDisplayTimeoutSettings,   nullptr},
  };
  systemSubPage("Display", items, 6);
}

// Calibrate battery voltage. Adjust the scaling factor.
// Type a multimeter reading to set it. Or nudge it manually.
static void systemShowBatteryCal() {
  Btn minusBtn = {8, 92, 70, 34, "-"};
  Btn plusBtn  = {tft.width() - 78, 92, 70, 34, "+"};
  Btn meterBtn = {8, 132, tft.width() - 16, 34, "Calibrate V_bat"};
  Btn resetBtn = {8, 172, tft.width() - 16, 34, "Reset cal"};

  auto drawLive = [&]() {
    int raw = uiBatteryRawMv();
    int cor = uiBatteryMv();
    int pct = uiBatteryPct();
    uiClearRect(0, 32, tft.width(), 44);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(6, 36);  tft.printf("raw:       %4d mV", raw);
    tft.setCursor(6, 50);  tft.printf("corrected: %4d mV  (%s%d%%)",
                                      cor, pct < 0 ? "USB " : "~", pct < 0 ? 0 : pct);
    tft.setCursor(6, 64);  tft.printf("factor:    x%.3f", uiBatteryCal());
  };
  auto draw = [&]() {
    uiDrawTopBar("Battery Info");
    uiClearBelow(29);
    drawLive();
    uiDrawMenuButton(minusBtn);
    uiDrawMenuButton(plusBtn);
    uiDrawMenuButton(meterBtn);
    uiDrawMenuButton(resetBtn);
  };
  draw();

  uint32_t lastLive = millis();
  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) {
      if (millis() - lastLive > 800) { drawLive(); lastLive = millis(); }
      delay(15);
      continue;
    }
    if (uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    if (uiTouchInButton(t, minusBtn)) {
      uiBatterySetCal(uiBatteryCal() - 0.005f);
      drawLive(); uiWaitForRelease();
    } else if (uiTouchInButton(t, plusBtn)) {
      uiBatterySetCal(uiBatteryCal() + 0.005f);
      drawLive(); uiWaitForRelease();
    } else if (uiTouchInButton(t, meterBtn)) {
      uiWaitForRelease();
      String s = uiNumpadInput("Pack mV read on a multimeter (4200 = full)", "4200");
      int mv = s.toInt();
      if (mv >= 2500 && mv <= 4400) uiBatterySetCalFromActual(mv);
      draw();
    } else if (uiTouchInButton(t, resetBtn)) {
      uiBatterySetCal(1.0f);
      drawLive(); uiWaitForRelease();
    }
  }
}

// Show static SD card space. Values do not change while this screen is open.
static void systemShowSdInfo() {
  uiDrawTopBar("SD Card");
  uiClearBelow(29);
  sdBusBegin();
  bool ok = SD.begin(SD_CS, sdSPI);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  if (ok) {
    // Use filesystem-level byte counts. This keeps the free space calculation consistent.
    uint64_t total = SD.totalBytes();
    uint64_t used  = SD.usedBytes();
    uint64_t freeB = total - used;
    tft.setCursor(6, 40);  tft.printf("Total: %.1f MB", total / 1048576.0);
    tft.setCursor(6, 56);  tft.printf("Used:  %.1f MB", used / 1048576.0);
    tft.setCursor(6, 72);  tft.printf("Free:  %.1f MB", freeB / 1048576.0);
  } else {
    tft.setCursor(6, 40);  tft.print("SD card not found");
  }

  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
  }
}

static void systemShowDemoMode() {
  uiDrawTopBar("Demo Mode");
  uiClearBelow(29);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(6, 38);  tft.print("Every detector screen injects");
  tft.setCursor(6, 54);  tft.print("fake sample hits on a timer");
  tft.setCursor(6, 70);  tft.print("instead of scanning real WiFi/");
  tft.setCursor(6, 86);  tft.print("BLE -- for demos/testing without");
  tft.setCursor(6, 102); tft.print("needing real activity nearby.");

  Btn toggle = {8, 136, tft.width() - 16, 34, ""};
  auto drawToggle = [&]() {
    toggle.label = demoModeEnabled() ? "Demo Mode: ON" : "Demo Mode: OFF";
    uiDrawButtonColored(toggle, demoModeEnabled() ? ILI9341_GREEN : ILI9341_DARKGREY);
  };
  drawToggle();

  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    if (uiTouchInButton(t, toggle)) {
      uiWaitForRelease();
      demoModeSetEnabled(!demoModeEnabled());
      drawToggle();
    }
  }
}

static void systemShowHardwareMenu() {
  static const SysItem items[] = {
    {"SPI / IRQ pins", systemShowPins},
    {"Battery Info",   systemShowBatteryCal},
    {"Test GPS",       systemTestGps},
    {"Format SD card", sysFormat},
    {"SD Card",        systemShowSdInfo},
    {"Demo Mode",      systemShowDemoMode},
  };
  systemSubPage("Hardware", items, 6);
}

void systemTouch(const TouchPoint &t) {
  // Check the power icon first. It uses a circular hit test.
  // The popup draws over the current screen. Redraw clears the popup pixels.
  if (t.isNewPress && touchInPowerIcon(t)) {
    // Wait for finger release before opening the menu. This prevents the popup
    // from reading the lingering touch as a dismiss command.
    uiWaitForRelease();
    powerShowMenu();
    uiDrawTopBar("System");
    drawButtons();
    uiWaitForRelease();
    return;
  }
  if (uiTouchInButton(t, mainPrevBtn) && mainPage > 0) {
    uiWaitForRelease(); mainPage--; drawButtons(); return;
  }
  if (uiTouchInButton(t, mainNextBtn) && mainPage < mainPages - 1) {
    uiWaitForRelease(); mainPage++; drawButtons(); return;
  }
  static const struct { Btn *b; void (*run)(); } route[] = {
    {&displayBtn, systemShowDisplayMenu},
    {&hwBtn,      systemShowHardwareMenu},
    {&modsBtn,    systemShowModules},
    {&volBtn,     systemShowVolumePicker},
    {&wizardBtn,  onboardingRunWizard},
    {&aboutBtn,   drawAbout},
  };
  for (auto &r : route) {
    if (uiTouchInButton(t, *r.b)) {
      r.run();
      uiDrawTopBar("System");
      drawButtons();
      uiWaitForRelease();
      return;
    }
  }
}

void systemExit() {}
