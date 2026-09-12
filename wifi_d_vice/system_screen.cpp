// Device-level settings (setup/maintenance, not detection tools) -- reached
// from the gear icon, not the feature menu. The top level is grouped:
// "Display" (orientation, themes, touch recal), "Hardware" (SPI/IRQ pins,
// GPS test, SD format), then Modules / Beep volume / setup wizard / About
// flat. Each leaf is its own blocking sub-screen that returns on Back.
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
#include "ble_2fa.h"
// #include "demo.h"   // Outrun easter egg -- retired, kept for reference
#include "splash.h"
#include "theme.h"
#include "accent.h"
#include "tz.h"
#include "devtime.h"
#include "modvis.h"
#include "pincfg.h"
#include "gps_shared.h"

static const char *REPO_URL = "https://github.com/Vybenclave/wifi-d_vice";

// Top-level System menu is grouped: display-feel settings live behind
// "Display", add-on/hardware maintenance behind "Hardware"; the rest stay
// flat. The group pages (systemShowDisplayMenu / systemShowHardwareMenu)
// just nest the existing blocking sub-screens.
static Btn displayBtn, hwBtn, modsBtn, volBtn, wizardBtn, aboutBtn;
static bool sdOk = false;

static void drawButtons() {
  uiSetBgMode(UI_BG_IMAGE);   // button screen -> scene background
  uiClearBelow(29);
  int y = 34;
  const int h = 26, gap = 4;
  const int w = tft.width() - 16;
  displayBtn = {8, y, w, h, "Display"};          y += h + gap;
  hwBtn      = {8, y, w, h, "Hardware"};         y += h + gap;
  modsBtn    = {8, y, w, h, "Modules"};          y += h + gap;
  volBtn     = {8, y, w, h, "Beep volume"};      y += h + gap;
  wizardBtn  = {8, y, w, h, "Run setup wizard"}; y += h + gap;
  aboutBtn   = {8, y, w, h, "About"};            y += h + gap;
  uiDrawMenuButton(displayBtn); uiDrawMenuButton(hwBtn);   uiDrawMenuButton(modsBtn);
  uiDrawMenuButton(volBtn);   uiDrawMenuButton(wizardBtn); uiDrawMenuButton(aboutBtn);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, y + 3);
  tft.printf("SD: %s   theme: %s", sdOk ? "ok" : "not found", themeName(themeGet()));
}

// esp_qrcode_generate()'s display_func gets only the finished handle, no
// user-data pointer -- drawing straight to the shared `tft` from here
// (rather than stashing the handle for the caller to draw later) is fine
// since this whole call is synchronous and single-threaded, unlike the BLE
// pairing screen's cross-task case. `s_qrTop` is the wanted top edge (set
// by drawAbout); `s_qrBottom` is handed back so drawAbout knows where the
// centred code ends and the text can start.
static int s_qrTop = 34;
static int s_qrBottom = 34;
static int s_qrX = 0, s_qrPx = 0;   // QR bounding box (for the easter-egg hit test)

static void qrDisplay(esp_qrcode_handle_t qrcode) {
  int side = esp_qrcode_get_size(qrcode);
  // Integer px-per-module only -- uniform, crisp, reliably scannable. For a
  // github-URL QR (v3/v4, 29-33 modules) scale 3 lands at ~87-99px.
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

    // No "done" button -- the top bar's universal back button closes this.
    // Tapping the QR code shows the splash art (was the Miami Vice demo).
    for (;;) {
      TouchPoint t = uiReadTouch();
      uiServiceChrome();
      if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
      if (t.pressed && t.x >= s_qrX && t.x < s_qrX + s_qrPx &&
          t.y >= s_qrTop && t.y < s_qrTop + s_qrPx) {
        uiWaitForRelease();
        showSplashArt();
        // demoRun();   // Outrun easter egg -- retired, see demo.cpp
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
  drawButtons();
}

void systemLoop() {}

// Live GPS bring-up: shows sentence count, sats, fix, position, HDOP and
// time off the shared background reader (gps_shared.h) -- it's been
// running the UART since boot for the GPS time sync, so this just reads
// its live TinyGPSPlus state instead of opening a second reader on top of
// it (that would have starved whichever one lost the race for bytes).
// Back to exit.
static void systemTestGps() {
  uiDrawTopBar("Test GPS");
  uiClearBelow(29);
  TinyGPSPlus &gps = gpsShared();
  uint32_t lastDraw = 0;

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); break; }

    if (millis() - lastDraw > 400) {
      lastDraw = millis();
      bool anyData = gps.charsProcessed() > 0;
      tft.fillRect(0, 32, tft.width(), tft.height() - 44, ILI9341_BLACK);
      tft.setTextSize(1);
      int y = 36;
      auto line = [&](uint16_t col, const char *k, const String &v) {
        tft.setTextColor(accentLabel());  tft.setCursor(4, y);   tft.print(k);
        tft.setTextColor(col);           tft.setCursor(100, y); tft.print(v);
        y += 15;
      };
      line(ILI9341_WHITE, "RX pin", String(GPS_RX));
      line(anyData ? ILI9341_GREEN : ILI9341_RED, "serial",
           anyData ? (String(gps.charsProcessed()) + " bytes") : String("no data"));
      line(gps.sentencesWithFix() ? ILI9341_GREEN : ILI9341_YELLOW, "NMEA ok", String(gps.passedChecksum()));
      line(gps.satellites.isValid() ? ILI9341_GREEN : ILI9341_YELLOW, "sats",
           gps.satellites.isValid() ? String(gps.satellites.value()) : String("--"));
      line(gps.location.isValid() ? ILI9341_GREEN : ILI9341_YELLOW, "fix",
           gps.location.isValid() ? String("yes") : String("no fix"));
      if (gps.location.isValid()) {
        line(ILI9341_WHITE, "lat", String(gps.location.lat(), 6));
        line(ILI9341_WHITE, "lon", String(gps.location.lng(), 6));
        line(ILI9341_WHITE, "alt m", gps.altitude.isValid() ? String(gps.altitude.meters(), 1) : String("--"));
      }
      line(ILI9341_WHITE, "HDOP", gps.hdop.isValid() ? String(gps.hdop.hdop(), 1) : String("--"));
      if (gps.time.isValid()) {
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
        line(ILI9341_WHITE, "UTC", b);
      }
      line(devTimeSynced() ? ILI9341_GREEN : ILI9341_YELLOW, "dev clock",
           devTimeSynced() ? devTimeNowString() : String("unsynced"));
      if (!anyData) {
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(4, tft.height() - 16);
        tft.print("no bytes -- GPS TX -> GPIO35, GND, 3V3?");
      }
    }
    delay(5);
  }
}

// Black or white, whichever reads better on an arbitrary RGB565 fill --
// so a swatch's label stays legible no matter how bright/dark a future
// accent color's fill turns out to be, without hand-tuning per color.
static uint16_t contrastTextFor(uint16_t c) {
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  int luma = (r * 255 / 31) * 299 + (g * 255 / 63) * 587 + (b * 255 / 31) * 114;
  return luma / 1000 > 140 ? ILI9341_BLACK : ILI9341_WHITE;
}

// Swatch-grid picker for the accent color (accent.h) -- each cell IS
// that color's actual fill, so the name alone doesn't have to explain
// it. 2 columns, paged so a cell never shrinks below MIN_CELL_H even
// with all 16 colors. Dropdown-style: tapping a cell selects it and
// returns immediately, no separate Apply step; tapping the empty space
// below the grid (only shown when there's more than one page) advances
// to the next page, same gesture uiDropdownPick() uses for a plain list.
static int pickAccentColor(int current) {
  const int y0 = 34, cols = 2, gap = 8, bottomMargin = UI_STATUSBAR_H + 4;
  const int MIN_CELL_H = 50;
  int rowsFit = (tft.height() - bottomMargin - y0 + gap) / (MIN_CELL_H + gap);
  if (rowsFit < 1) rowsFit = 1;
  int perPage = rowsFit * cols;
  if (perPage > ACCENT_N) perPage = ACCENT_N;
  int pages = (ACCENT_N + perPage - 1) / perPage;
  int page = (current >= 0 && current < ACCENT_N) ? current / perPage : 0;

  Btn cell[ACCENT_N];   // only [0, n) of this page's cells are ever filled in
  int n = 0, rows = 0, cellH = 0;
  auto draw = [&]() {
    uiDrawTopBar("Accent Color");
    uiClearBelow(29);
    int base = page * perPage;
    n = min(perPage, ACCENT_N - base);
    rows = (n + cols - 1) / cols;
    int cellW = (tft.width() - 16 - gap) / cols;
    cellH = (tft.height() - bottomMargin - y0 - gap * (rows - 1)) / rows;
    if (cellH > 90) cellH = 90;   // don't get silly big on a tall screen with few colors on the page

    for (int i = 0; i < n; i++) {
      int id = base + i;
      int col = i % cols, row = i / cols;
      int x = 8 + col * (cellW + gap);
      int y = y0 + row * (cellH + gap);
      cell[i] = {x, y, cellW, cellH, accentName(id)};

      uint16_t fill = accentFillFor(id);
      tft.fillRoundRect(x, y, cellW, cellH, 8, fill);
      tft.drawRoundRect(x, y, cellW, cellH, 8, accentEdgeFor(id));
      tft.setTextColor(contrastTextFor(fill));
      tft.setTextSize(cellH >= 40 ? 2 : 1);
      int16_t bx, by; uint16_t bw, bh;
      tft.getTextBounds(accentName(id), 0, 0, &bx, &by, &bw, &bh);
      tft.setCursor(x + (cellW - (int)bw) / 2 - bx, y + (cellH - (int)bh) / 2 - by);
      tft.print(accentName(id));

      if (id == current) {   // "current" marker -- a double outline reads on any fill color
        tft.drawRect(x - 3, y - 3, cellW + 6, cellH + 6, ILI9341_GREEN);
        tft.drawRect(x - 2, y - 2, cellW + 4, cellH + 4, ILI9341_GREEN);
      }
    }
    if (pages > 1) {
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(1);
      char pg[32];
      snprintf(pg, sizeof(pg), "page %d/%d -- tap to advance", page + 1, pages);
      tft.setCursor(8, y0 + rows * (cellH + gap) + 2);
      tft.print(pg);
    }
  };
  draw();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return current; }
    int base = page * perPage;
    for (int i = 0; i < n; i++)
      if (uiTouchInButton(t, cell[i])) { uiWaitForRelease(); return base + i; }
    if (t.isNewPress && pages > 1 && t.y > y0 + rows * (cellH + gap)) {
      uiWaitForRelease();
      page = (page + 1) % pages;
      draw();
    }
    delay(15);
  }
}

// Theme (structural: Basic/Vice, theme.h) and Accent Color (universal
// color, accent.h) are INDEPENDENT settings -- switching one never
// touches the other. Each gets its own pull-down row; picking an option
// applies immediately and closes the dropdown, so there's no separate
// Apply step or pending-vs-active distinction to track here.
static void systemShowThemeColor() {
  Btn themeBtn, accentBtn;
  auto draw = [&]() {
    uiDrawTopBar("Theme & Color");
    uiClearBelow(29);
    char tlbl[24], albl[24];
    snprintf(tlbl, sizeof(tlbl), "Theme: %s", themeName(themeGet()));
    snprintf(albl, sizeof(albl), "Accent: %s", accentName(accentGet()));
    themeBtn  = {8, 40, tft.width() - 16, 40, tlbl};
    accentBtn = {8, 88, tft.width() - 16, 40, albl};
    uiDrawButton(themeBtn);
    uiDrawButton(accentBtn);
    // A live swatch of the current accent next to its row -- under Basic
    // (whose buttons don't render the accent at all) this is the only
    // place on the whole Display menu that shows what "Amber" means.
    int sw = 24;
    int sx = accentBtn.x + accentBtn.w - sw - 10, sy = accentBtn.y + (accentBtn.h - sw) / 2;
    tft.fillRoundRect(sx, sy, sw, sw, 4, accentFill());
    tft.drawRoundRect(sx, sy, sw, sw, 4, accentEdge());
  };
  draw();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
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
    delay(15);
  }
}

// Display-only UTC offset + 12h/24h format + auto-DST for the bottom-bar
// clock (tz.h) -- never touches devtime.h or any SD log, which always stay
// UTC. Paged since the offset list is too long for one page on this
// display; ROWS is computed from the screen height (not a fixed constant)
// so this doesn't overrun the Apply button in landscape's shorter 240px
// height.
static void systemShowTimezone() {
  const int y0 = 34, rowH = 22, gap = 3;
  // BOTTOM_MARGIN clears the full status bar -- see the comment on
  // pickAccentColor()'s copy of this same constant.
  const int PAGEROW_H = 26, TOGGLE_H = 28, APPLY_H = 34, STACK_GAP = 6, BOTTOM_MARGIN = UI_STATUSBAR_H + 4;
  const int MAX_ROWS = 10;
  // Two toggle rows now (12h/24h and Auto DST) between the pager and Apply.
  int stackTop = tft.height() - BOTTOM_MARGIN - APPLY_H - STACK_GAP
                 - TOGGLE_H - STACK_GAP - TOGGLE_H - STACK_GAP - PAGEROW_H;
  int ROWS = (stackTop - gap - y0) / (rowH + gap);
  if (ROWS < 3) ROWS = 3;
  if (ROWS > MAX_ROWS) ROWS = MAX_ROWS;
  if (ROWS > tzCount()) ROWS = tzCount();

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
    prevBtn = {8, py, 60, PAGEROW_H, "< prev"};
    nextBtn = {tft.width() - 68, py, 60, PAGEROW_H, "next >"};
    if (page > 0)          uiDrawButton(prevBtn);    else uiDrawButtonDim(prevBtn);
    if (page < pages - 1)  uiDrawButton(nextBtn);    else uiDrawButtonDim(nextBtn);
    tft.setTextColor(ILI9341_WHITE);
    char pg[16];
    snprintf(pg, sizeof(pg), "%d / %d", page + 1, pages);
    int16_t bx, by; uint16_t bw, bh;
    tft.setTextSize(1);
    tft.getTextBounds(pg, 0, 0, &bx, &by, &bw, &bh);
    tft.setCursor((tft.width() - (int)bw) / 2, py + (PAGEROW_H - (int)bh) / 2 - by);
    tft.print(pg);

    toggleBtn = {8, py + PAGEROW_H + STACK_GAP, tft.width() - 16, TOGGLE_H,
                 tzUse24h() ? "24-hour clock" : "12-hour clock"};
    uiDrawMenuButton(toggleBtn);

    // Status reflects the PENDING selection (sel), not just the applied
    // zone, so browsing the list previews whether that zone is even
    // DST-eligible before tapping Apply.
    static char dstLbl[40];
    if (!tzAutoDst())            strcpy(dstLbl, "Auto DST: off");
    else if (!tzZoneHasDst(sel)) strcpy(dstLbl, "Auto DST: on (n/a here)");
    else if (tzDstActiveFor(sel)) strcpy(dstLbl, "Auto DST: on (+1h now)");
    else                          strcpy(dstLbl, "Auto DST: on (not active)");
    dstBtn = {8, py + PAGEROW_H + STACK_GAP + TOGGLE_H + STACK_GAP, tft.width() - 16, TOGGLE_H, dstLbl};
    uiDrawMenuButton(dstBtn);

    applyBtn = {8, py + PAGEROW_H + STACK_GAP + TOGGLE_H + STACK_GAP + TOGGLE_H + STACK_GAP,
                tft.width() - 16, APPLY_H,
                sel == tzGetIndex() ? "Apply (no change)" : "Apply"};
    uiDrawButton(applyBtn);
  };
  drawPicker();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
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
    // Border only, NOT uiDrawButton() -- that also prints its own
    // unrotated white label, which was landing right on top of the
    // rotated cyan one drawn just below and looked like corrupted
    // double-exposed text (confirmed report, not a photo artifact).
    tft.drawRect(cells[i].x, cells[i].y, cells[i].w, cells[i].h, ILI9341_WHITE);
    uiDrawRotatedText(cells[i].x + cells[i].w / 2, cells[i].y + cells[i].h / 2,
                       kLabels[i], i, 2, accentLabel());
  }

  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
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
  // Hold the DAC channel open for the whole page. This screen fires rapid
  // one-off beeps; letting each beep() enable+disable the continuous channel
  // latches the DMA after a few cycles (sound cuts out until reboot / reset).
  beepHold(true);
  draw();

  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
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

// Recursive wipe -- this is NOT a low-level FAT reformat (the bundled SD
// library doesn't expose one), it's a full delete of everything on the
// card. Framed to the user as "format" since that's the effect that
// matters, but worth being precise about in code: the filesystem
// structure itself is untouched, just its contents.
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
  // The card wipe is also the "clear everything" gesture: forget saved
  // Wi-Fi profiles + the engagement marker (both lived under /eng/, now
  // gone), drop the BLE bond, and zero any loaded key.
  engStoreClear();
  engagementResetAll();
  ble2faBegin();
  ble2faClearBonds();
  uiClearBelow(0);
  tft.setCursor(10, 100);
  tft.print("Done.");
  delay(600);
}

// Typing the word, not just tapping a button -- a tap-only "ERASE
// EVERYTHING" confirm button is exactly the kind of thing a misclick could
// hit by accident on a touchscreen.
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
  // cancelBtn's bottom edge must clear UI_STATUSBAR_H (+ a small gap) --
  // it used to sit at height-44 (bottom = height-8), inside the status bar.
  const int cancelY = tft.height() - UI_STATUSBAR_H - 4 - 36;
  Btn confirmBtn = {10, cancelY - 8 - 36, tft.width() - 20, 36, "type ERASE to confirm"};
  Btn cancelBtn = {10, cancelY, tft.width() - 20, 36, "cancel"};
  uiDrawButton(confirmBtn);
  uiDrawButton(cancelBtn);
  while (true) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInButton(t, confirmBtn)) {
      uiWaitForRelease();
      String typed = uiTextInput("Type ERASE to confirm", "", false);
      return typed == "ERASE";
    }
    if (uiTouchInButton(t, cancelBtn)) { uiWaitForRelease(); return false; }
  }
}

// Per-module show/hide. Hidden modules drop out of their WiFi/BLE/Privacy
// submenu; an emptied category hides its top-level button too.
static void systemShowModules() {
  const int y0 = 38, step = 25;
  Btn rows[MOD_N];

  auto drawRow = [&](int i) {
    int y = y0 + i * step;
    rows[i] = {8, y, tft.width() - 16, step - 3, modvisName(i)};
    uiDrawMenuButton(rows[i]);
    bool hidden = modvisHidden(i);
    int cx = tft.width() - 26, cy = y + (step - 3) / 2;
    tft.fillRect(cx - 8, cy - 8, 16, 16, uiBgColor(cy - 8));
    if (hidden) tft.fillCircle(cx, cy, 2, ILI9341_DARKGREY);      // grey dot
    else        tft.fillCircle(cx, cy, 6, ILI9341_GREEN);         // green circle
  };

  uiDrawTopBar("Modules");
  uiClearBelow(29);
  for (int i = 0; i < MOD_N; i++) drawRow(i);

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    for (int i = 0; i < MOD_N; i++) {
      if (t.pressed && uiTouchInButton(t, rows[i])) {
        modvisSetHidden(i, !modvisHidden(i));
        drawRow(i);
        uiWaitForRelease();
      }
    }
    delay(15);
  }
}

// Which add-on radios are installed + the SPI/IRQ GPIOs they use. A pin row
// is dimmed when its radio is not selected. Everything persists in NVS and
// takes effect on the next boot; holding BOOT at power-on wipes it.
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

// A grouped System sub-page: a short vertical list, each row runs one of
// the existing blocking sub-screens and comes back. Modelled on
// systemShowModules().
// label_fn (optional) overrides `label` at draw time -- used for toggle
// rows so the button text tracks the setting after run() flips it.
struct SysItem { const char *label; void (*run)(); const char *(*label_fn)(); };

static void systemSubPage(const char *title, const SysItem *items, int n) {
  Btn rows[6];
  auto draw = [&]() {
    uiDrawTopBar(title);
    uiSetBgMode(UI_BG_IMAGE);   // button screen -> scene background
    uiClearBelow(29);
    int y = 38;
    const int h = 30, gap = 4;
    for (int i = 0; i < n; i++) {
      rows[i] = {8, y, tft.width() - 16, h,
                 items[i].label_fn ? items[i].label_fn() : items[i].label};
      uiDrawMenuButton(rows[i]);
      y += h + gap;
    }
  };
  draw();
  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    for (int i = 0; i < n; i++) {
      if (t.pressed && uiTouchInButton(t, rows[i])) {
        uiWaitForRelease();
        items[i].run();
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
    {"Screen orientation", systemShowRotationPicker, nullptr},
    {"Theme & Color",      systemShowThemeColor,     nullptr},
    {"Timezone",           systemShowTimezone,       nullptr},
    {"Recalibrate touch",  uiRunCalibration,         nullptr},
    {nullptr,              sysToggleSplash,          splashLbl},
  };
  systemSubPage("Display", items, 5);
}

// V_bat calibration. The reading is raw_ADC * BAT_DIV * factor; this page
// tunes `factor`. "Set from meter": type the pack voltage read on a
// multimeter and factor becomes measured/raw. Or nudge it by hand.
static void systemShowBatteryCal() {
  Btn minusBtn = {8, 92, 70, 34, "-"};
  Btn plusBtn  = {tft.width() - 78, 92, 70, 34, "+"};
  Btn meterBtn = {8, 132, tft.width() - 16, 34, "Calibrate V_bat"};
  Btn resetBtn = {8, 172, tft.width() - 16, 34, "Reset cal"};

  auto drawLive = [&]() {
    int raw = uiBatteryRawMv();
    int cor = uiBatteryMv();
    int pct = uiBatteryPct();
    tft.fillRect(0, 32, tft.width(), 44, ILI9341_BLACK);
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

static void systemShowHardwareMenu() {
  static const SysItem items[] = {
    {"SPI / IRQ pins", systemShowPins},
    {"Battery Info",   systemShowBatteryCal},
    {"Test GPS",       systemTestGps},
    {"Format SD card", sysFormat},
  };
  systemSubPage("Hardware", items, 4);
}

void systemTouch(const TouchPoint &t) {
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
