#include "ui.h"
#include <SPI.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "driver/dac_continuous.h"
#include "devtime.h"
#include "tz.h"
#include "theme.h"
#include "bg_landscape.h"   // BG_LANDSCAPE[19200]  160x120, upscaled x2
#include "bg_portrait.h"    // BG_PORTRAIT[19200]   120x160

// One shared DAC channel on GPIO26 for ALL audio -- UI beeps and the
// easter-egg MOD player. Created once here, enabled only while something
// is playing. Using a single continuous/DMA channel (never the one-shot
// dacWrite path) means beeps and the demo can't leave the DAC in a state
// that breaks the other -- which is what killed the sound after the demo.
static dac_continuous_handle_t g_dac = nullptr;
static const int UI_DAC_HZ = 22050;
dac_continuous_handle_t uiDac() { return g_dac; }
int uiDacRate() { return UI_DAC_HZ; }

static void dacInit() {
  dac_continuous_config_t dc = {};
  dc.chan_mask = DAC_CHANNEL_MASK_CH1;   // GPIO26
  dc.desc_num  = 2;
  dc.buf_size  = 512;                    // ~46ms ring -- small so held chirps aren't laggy
  dc.freq_hz   = UI_DAC_HZ;
  dc.offset    = 0;
  dc.clk_src   = DAC_DIGI_CLK_SRC_DEFAULT;
  dc.chan_mode = DAC_CHANNEL_MODE_SIMUL;
  if (dac_continuous_new_channels(&dc, &g_dac) != ESP_OK) {
    dc.clk_src = DAC_DIGI_CLK_SRC_APLL;
    if (dac_continuous_new_channels(&dc, &g_dac) != ESP_OK) g_dac = nullptr;
  }
}

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, -1);   // reset shared with EN, see pins.h

static bool s_batForce = false;   // uiDrawTopBar() -> uiDrawBatteryIndicator() repaint on screen change
static bool s_clockForce = false; // uiDrawStatusBar() -> uiDrawClock() repaint on screen change
static int  s_bgMode  = UI_BG_BLACK;
void uiSetBgMode(int m) { s_bgMode = m; }
static void loadBeepVolume();   // defined below beep(); forward-declared for uiInit()
static void ledBusyTask(void *); // defined below ledSet(); forward-declared for uiInit()

// ---- bit-banged XPT2046 (touch pins don't sit on either hw SPI's native
// set -- see esp32-cyd skill / cyd_selftest for why this isn't a library) ----
static uint16_t xptCmd(uint8_t cmd) {
  digitalWrite(TP_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TP_MOSI, (cmd >> i) & 1);
    digitalWrite(TP_SCK, HIGH);
    delayMicroseconds(2);
    digitalWrite(TP_SCK, LOW);
    delayMicroseconds(2);
  }
  uint16_t result = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(TP_SCK, HIGH);
    delayMicroseconds(2);
    result <<= 1;
    result |= digitalRead(TP_MISO);
    digitalWrite(TP_SCK, LOW);
    delayMicroseconds(2);
  }
  digitalWrite(TP_CS, HIGH);
  return result >> 3;
}

static bool readTouchRaw(int &rawX, int &rawY) {
  if (digitalRead(TP_IRQ) == HIGH) return false;
  xptCmd(0xD0);
  rawX = xptCmd(0xD0);
  rawY = xptCmd(0x90);
  return digitalRead(TP_IRQ) == LOW;   // still down after the read settles
}

// Calibration -- persisted in NVS, refined by uiRunCalibration(). Stored
// PER ROTATION (4 independent slots): touching is a physically fixed
// overlay that doesn't rotate with tft.setRotation(), but WHICH raw axis
// correlates with "screen X" and its direction both change with rotation
// (rotation changes what's currently drawn as "screen X"). Two attempts at
// deriving that relationship analytically from one hardware-confirmed
// baseline both turned out wrong when tested -- MADCTL semantics per
// rotation aren't simple enough to get right by inspection alone. So
// instead of deriving it, uiRunCalibration() *measures* it fresh each time:
// a 3-point (L-shaped) tap discovers which raw axis moves with screen X
// vs screen Y and in which direction, rather than assuming. This can't be
// wrong in the way the formula derivation was, since nothing is assumed.
struct TouchCal {
  bool valid;
  bool swapXY;         // true: rawY tracks screen X, rawX tracks screen Y
  int scrXMin, scrXMax; // raw value (of whichever axis tracks screen X) at screen X=0 and X=width
  int scrYMin, scrYMax; // same, for screen Y
};
static TouchCal cal[4];   // indexed by rotation

static uint8_t displayRotation = 1;   // 0-3, persisted in NVS

static void loadRotation() {
  Preferences p;
  p.begin("touchcal", true);
  displayRotation = p.getUChar("rot", 1);
  p.end();
  if (displayRotation > 3) displayRotation = 1;
}

static void loadCalFor(uint8_t r) {
  Preferences p;
  p.begin("touchcal", true);
  char k[8];
  snprintf(k, sizeof(k), "v3_%u", r);
  cal[r].valid = p.getBool(k, false);
  if (cal[r].valid) {
    snprintf(k, sizeof(k), "sw%u", r);  cal[r].swapXY  = p.getBool(k, false);
    snprintf(k, sizeof(k), "xn%u", r);  cal[r].scrXMin = p.getInt(k, 200);
    snprintf(k, sizeof(k), "xx%u", r);  cal[r].scrXMax = p.getInt(k, 3900);
    snprintf(k, sizeof(k), "yn%u", r);  cal[r].scrYMin = p.getInt(k, 200);
    snprintf(k, sizeof(k), "yx%u", r);  cal[r].scrYMax = p.getInt(k, 3900);
  }
  p.end();
}

static void saveCalFor(uint8_t r) {
  Preferences p;
  p.begin("touchcal", false);
  char k[8];
  snprintf(k, sizeof(k), "v3_%u", r);  p.putBool(k, true);
  snprintf(k, sizeof(k), "sw%u", r);   p.putBool(k, cal[r].swapXY);
  snprintf(k, sizeof(k), "xn%u", r);   p.putInt(k, cal[r].scrXMin);
  snprintf(k, sizeof(k), "xx%u", r);   p.putInt(k, cal[r].scrXMax);
  snprintf(k, sizeof(k), "yn%u", r);   p.putInt(k, cal[r].scrYMin);
  snprintf(k, sizeof(k), "yx%u", r);   p.putInt(k, cal[r].scrYMax);
  p.end();
  cal[r].valid = true;
}

void uiSetRotation(uint8_t r) {
  displayRotation = r % 4;
  Preferences p;
  p.begin("touchcal", false);
  p.putUChar("rot", displayRotation);
  p.end();
  tft.setRotation(displayRotation);
  loadCalFor(displayRotation);
  if (!cal[displayRotation].valid) uiRunCalibration();   // this orientation's never been calibrated
}

void uiCycleRotation() { uiSetRotation((displayRotation + 1) % 4); }

TouchPoint uiReadTouch() {
  // wasPressed tracks physical state across calls so isNewPress can detect
  // the rising edge -- every call updates it, whether or not this sample is
  // a press, so a screen that stops polling mid-hold (e.g. a modal sub-loop
  // like calibration/keyboard, which read the raw touch directly instead)
  // doesn't leave it desynced for the next uiReadTouch() call afterward.
  static bool wasPressed = false;
  TouchPoint t{false, false, 0, 0};
  int rawX, rawY;
  bool pressed = readTouchRaw(rawX, rawY);
  t.pressed = pressed;
  t.isNewPress = pressed && !wasPressed;
  wasPressed = pressed;
  if (!pressed) return t;
  const TouchCal &c = cal[displayRotation];
  int rawForX = c.swapXY ? rawY : rawX;
  int rawForY = c.swapXY ? rawX : rawY;
  int sx = map(rawForX, c.scrXMin, c.scrXMax, 0, tft.width());
  int sy = map(rawForY, c.scrYMin, c.scrYMax, 0, tft.height());
  t.x = constrain(sx, 0, tft.width() - 1);
  t.y = constrain(sy, 0, tft.height() - 1);
  return t;
}

void uiWaitForRelease(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (uiReadTouch().pressed) {
    if (millis() - start > timeoutMs) return;
    delay(10);
  }
}

static bool waitForRawTap(int &rawX, int &rawY) {
  // wait for press
  while (!readTouchRaw(rawX, rawY)) delay(10);
  delay(30);   // debounce/settle
  readTouchRaw(rawX, rawY);
  // wait for release so it doesn't bleed into the next crosshair
  int rx, ry;
  while (readTouchRaw(rx, ry)) delay(10);
  return true;
}

void uiRunCalibration() {
  // Whatever triggered this (tapping "Rotate screen", holding BOOT, tapping
  // "Recalibrate touch") may still be physically held down right now -- if
  // so, the first crosshair's waitForRawTap() would instantly "register" a
  // tap at that stale, wrong location instead of actually waiting for a
  // fresh one. Uses the raw reader, not uiReadTouch(), since calibration
  // for the current rotation may not exist yet at this point.
  int rx, ry;
  while (readTouchRaw(rx, ry)) delay(10);

  const int INSET = 24;
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, tft.height() / 2 - 20);
  tft.print("Touch calibration");
  tft.setCursor(10, tft.height() / 2);
  tft.print("Tap each crosshair");

  auto drawCross = [](int x, int y) {
    tft.drawFastHLine(x - 8, y, 17, ILI9341_CYAN);
    tft.drawFastVLine(x, y - 8, 17, ILI9341_CYAN);
  };

  // L-shaped 3 points (not 2 diagonal ones) -- p1->p2 isolates a screen-X-
  // only change, p1->p3 isolates a screen-Y-only change. That's what lets
  // this *measure* which raw axis is which instead of assuming it (see the
  // struct comment above for why assuming it twice went wrong).
  int x1 = INSET, y1 = INSET;
  int x2 = tft.width() - INSET, y2 = y1;
  int x3 = x1, y3 = tft.height() - INSET;

  drawCross(x1, y1);
  int rawX1, rawY1;
  waitForRawTap(rawX1, rawY1);

  tft.fillScreen(ILI9341_BLACK);
  drawCross(x2, y2);
  int rawX2, rawY2;
  waitForRawTap(rawX2, rawY2);

  tft.fillScreen(ILI9341_BLACK);
  drawCross(x3, y3);
  int rawX3, rawY3;
  waitForRawTap(rawX3, rawY3);

  TouchCal c;
  int dxRawX = abs(rawX2 - rawX1), dxRawY = abs(rawY2 - rawY1);
  c.swapXY = dxRawY > dxRawX;   // whichever raw axis moved more from p1->p2 (X-only change) is the X axis

  int rXp1 = c.swapXY ? rawY1 : rawX1, rXp2 = c.swapXY ? rawY2 : rawX2;
  c.scrXMin = rXp1 - (rXp2 - rXp1) * x1 / (x2 - x1);
  c.scrXMax = rXp2 + (rXp2 - rXp1) * (tft.width() - 1 - x2) / (x2 - x1);

  int rYp1 = c.swapXY ? rawX1 : rawY1, rYp3 = c.swapXY ? rawX3 : rawY3;
  c.scrYMin = rYp1 - (rYp3 - rYp1) * y1 / (y3 - y1);
  c.scrYMax = rYp3 + (rYp3 - rYp1) * (tft.height() - 1 - y3) / (y3 - y1);

  cal[displayRotation] = c;
  saveCalFor(displayRotation);

  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(10, tft.height() / 2);
  tft.print("Calibrated.");
  delay(600);
}

void uiInit() {
  pinMode(TP_CS, OUTPUT);   digitalWrite(TP_CS, HIGH);
  pinMode(TP_SCK, OUTPUT);  digitalWrite(TP_SCK, LOW);
  pinMode(TP_MOSI, OUTPUT);
  pinMode(TP_MISO, INPUT);
  pinMode(TP_IRQ, INPUT);

  pinMode(LED_STATUS, OUTPUT); ledSet(false);
  pinMode(LED_GREEN, OUTPUT);  digitalWrite(LED_GREEN, HIGH);   // off (active low)
  pinMode(AUDIO_EN, OUTPUT);   digitalWrite(AUDIO_EN, HIGH);
  pinMode(BOOT_KEY, INPUT_PULLUP);

  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  loadRotation();
  tft.setRotation(displayRotation);
  tft.fillScreen(ILI9341_BLACK);

  loadCalFor(displayRotation);
  if (!cal[displayRotation].valid) uiRunCalibration();

  loadBeepVolume();
  uiBatteryCalLoad();
  dacInit();
  xTaskCreatePinnedToCore(ledBusyTask, "ledbusy", 1536, nullptr, 1, nullptr, 0);
}

static const Btn kBackBtn = {0, 0, 60, 28, "<<<"};

uint16_t uiBgColor(int y) {
  if (!themeIsVice()) return ILI9341_BLACK;
  int h = tft.height(); if (h < 1) h = 1;
  float f = (float)y / h; if (f < 0) f = 0; if (f > 1) f = 1;
  uint16_t a = TH_GRAD_TOP, b = TH_GRAD_BOT;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r  = ar + (int)((br - ar) * f + 0.5f);
  int g  = ag + (int)((bg - ag) * f + 0.5f);
  int bl = ab + (int)((bb - ab) * f + 0.5f);
  return (r << 11) | (g << 5) | bl;
}

// Half-res dimmed scene art. Landscape panel -> BG_LANDSCAPE (160x120),
// portrait -> BG_PORTRAIT (120x160); flipped orientations reuse the same
// array, same as showSplash().
static const uint16_t *bgSrc(int &sw, int &sh) {
  if (tft.width() >= tft.height()) { sw = 160; sh = 120; return BG_LANDSCAPE; }
  sw = 120; sh = 160; return BG_PORTRAIT;
}

void uiClearRect(int x, int y, int w, int h) {
  if (!themeIsVice()) { tft.fillRect(x, y, w, h, ILI9341_BLACK); return; }

  if (s_bgMode == UI_BG_IMAGE) {
    int sw, sh;
    const uint16_t *img = bgSrc(sw, sh);
    static uint16_t line[320];
    tft.startWrite();
    for (int yy = y; yy < y + h; yy++) {
      int sy = yy >> 1; if (sy >= sh) sy = sh - 1; if (sy < 0) sy = 0;
      const uint16_t *srow = img + sy * sw;
      int n = 0;
      for (int xx = x; xx < x + w && n < 320; xx++, n++) {
        int sx = xx >> 1; if (sx >= sw) sx = sw - 1; if (sx < 0) sx = 0;
        line[n] = pgm_read_word(&srow[sx]);
      }
      tft.setAddrWindow(x, yy, n, 1);
      tft.writePixels(line, n, true, false);
    }
    tft.endWrite();
    return;
  }

  for (int yy = y; yy < y + h; yy += 4) {
    int bh = (y + h - yy) < 4 ? (y + h - yy) : 4;
    tft.fillRect(x, yy, w, bh, uiBgColor(yy));
  }
}

void uiDrawStatusBar() {
  int y = tft.height() - UI_STATUSBAR_H;
  tft.fillRect(0, y, tft.width(), UI_STATUSBAR_H, ILI9341_BLACK);
  tft.drawFastHLine(0, y, tft.width(), ILI9341_WHITE);
  s_batForce = true;     // the battery glyph lives in the bar -- repaint it
  s_clockForce = true;   // ditto the clock -- this just painted over it
}

void uiClearBelow(int y0) {
  uiClearRect(0, y0, tft.width(), tft.height() - y0);
  uiDrawStatusBar();
}

void uiDrawTopBar(const char *title) {
  s_bgMode = UI_BG_BLACK;   // opt-in per screen; default back to plain
  tft.fillRect(0, 0, tft.width(), 28, ILI9341_NAVY);
  tft.drawFastHLine(0, 28, tft.width(), ILI9341_WHITE);
  uiDrawStatusBar();
  uiDrawButton(kBackBtn);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextWrap(false);   // clip rather than silently wrap into the row below
  uint8_t size = 2;
  int16_t bx, by; uint16_t bw, bh;
  tft.setTextSize(size);
  tft.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
  if (bw > (uint16_t)tft.width() - 72) {   // doesn't fit next to the back button
    size = 1;
    tft.setTextSize(size);
  }
  tft.setCursor(68, size == 2 ? 6 : 10);
  tft.print(title);
  s_batForce = true;   // repaint the battery glyph on the new screen
}

bool uiTouchInBackButton(const TouchPoint &t) { return uiTouchInButton(t, kBackBtn); }

// Area-only hit test (ignores isNewPress) -- for detecting a *held* back
// button, where uiTouchInBackButton()'s new-press guard would miss it.
bool uiTouchInBackArea(const TouchPoint &t) {
  return t.pressed && t.x >= kBackBtn.x && t.x < kBackBtn.x + kBackBtn.w &&
         t.y >= kBackBtn.y && t.y < kBackBtn.y + kBackBtn.h;
}

void uiDrawButton(const Btn &b) {
  if (themeIsVice()) {
    int r = b.h / 2; if (r > 10) r = 10; if (r < 3) r = 3;
    tft.fillRoundRect(b.x, b.y, b.w, b.h, r, TH_BTN_FILL);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, r, TH_BTN_EDGE);
    if (b.w > 4 && b.h > 4)
      tft.drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, r - 1, TH_BTN_EDGE);
    tft.drawFastHLine(b.x + r, b.y + 2, b.w - 2 * r, TH_BTN_BEVEL);

    // Label starts at the global cap (UI_MENU_BTN_MAXSIZE, see ui.h) and
    // steps down until it fits -- one knob for button text size app-wide.
    int16_t bx, by; uint16_t bw, bh;
    int ts = UI_MENU_BTN_MAXSIZE;
    for (; ts > 1; ts--) {
      tft.setTextSize(ts);
      tft.getTextBounds(b.label, 0, 0, &bx, &by, &bw, &bh);
      if ((int)bw <= b.w - 8 && (int)bh <= b.h - 4) break;
    }
    tft.setTextSize(ts);
    tft.getTextBounds(b.label, 0, 0, &bx, &by, &bw, &bh);
    int tx = b.x + (b.w - (int)bw) / 2 - bx;
    int ty = b.y + (b.h - (int)bh) / 2 - by;
    tft.setTextColor(TH_BTN_EMBOSS);
    tft.setCursor(tx - 1, ty - 1); tft.print(b.label);
    tft.setTextColor(TH_BTN_TEXT);
    tft.setCursor(tx, ty);         tft.print(b.label);
    tft.setTextSize(1);
    return;
  }

  tft.setTextSize(1);
  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(b.label, 0, 0, &bx, &by, &bw, &bh);
  tft.drawRect(b.x, b.y, b.w, b.h, ILI9341_WHITE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(b.x + (b.w - (int)bw) / 2, b.y + (b.h - (int)bh) / 2);
  tft.print(b.label);
}

// uiDrawButton() now caps label size at UI_MENU_BTN_MAXSIZE app-wide, so
// this is just an alias kept for the call sites that name it explicitly.
void uiDrawMenuButton(const Btn &b) { uiDrawButton(b); }

void uiDrawButtonDim(const Btn &b) {
  const uint16_t fill = 0x2124;   // dark grey
  const uint16_t edge = 0x4A69;   // mid-dark grey border
  const uint16_t txt  = 0x8410;   // grey label
  if (themeIsVice()) {
    int r = b.h / 2; if (r > 10) r = 10; if (r < 3) r = 3;
    tft.fillRoundRect(b.x, b.y, b.w, b.h, r, fill);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, r, edge);
  } else {
    tft.fillRect(b.x, b.y, b.w, b.h, ILI9341_BLACK);
    tft.drawRect(b.x, b.y, b.w, b.h, edge);
  }
  int16_t bx, by; uint16_t bw, bh;
  int ts = UI_MENU_BTN_MAXSIZE;
  for (; ts > 1; ts--) {
    tft.setTextSize(ts);
    tft.getTextBounds(b.label, 0, 0, &bx, &by, &bw, &bh);
    if ((int)bw <= b.w - 8 && (int)bh <= b.h - 4) break;
  }
  tft.setTextSize(ts);
  tft.getTextBounds(b.label, 0, 0, &bx, &by, &bw, &bh);
  tft.setTextColor(txt);
  tft.setCursor(b.x + (b.w - (int)bw) / 2 - bx, b.y + (b.h - (int)bh) / 2 - by);
  tft.print(b.label);
  tft.setTextSize(1);
}

void uiDrawActionRow(Btn *btns, int count) {
  // Clear the row band first: Vice pill buttons are rounded rects that
  // leave their corners unpainted, so without this the previous screen
  // bleeds through around every action-row button.
  uiClearRect(0, UI_ACTIONROW_Y, tft.width(), UI_ACTIONROW_H);
  int w = tft.width() / count;
  for (int i = 0; i < count; i++) {
    btns[i].x = i * w;
    btns[i].y = UI_ACTIONROW_Y;
    btns[i].w = (i == count - 1) ? tft.width() - i * w : w;
    btns[i].h = UI_ACTIONROW_H;
    uiDrawMenuButton(btns[i]);
  }
}

bool uiTouchInButton(const TouchPoint &t, const Btn &b) {
  // isNewPress, not pressed -- a held touch must not keep re-triggering
  // this button/screen change on every poll. See TouchPoint's comment in
  // ui.h for where to deliberately opt back into repeat-while-held.
  return t.isNewPress && t.x >= b.x && t.x < b.x + b.w && t.y >= b.y && t.y < b.y + b.h;
}

void uiToast(const char *msg) {
  // Live inside the status bar, but leave its 1px white top rule intact.
  int y = tft.height() - UI_STATUSBAR_H + 1;
  // Right edge is short -- the bottom-right corner carries the clock
  // (uiDrawClock) and the battery glyph (uiDrawBatteryIndicator) on every
  // screen. Without this the toast text runs under them.
  int w = tft.width() - UI_RIGHTZONE_W;
  tft.fillRect(0, y, w, UI_STATUSBAR_H - 1, ILI9341_BLACK);
  tft.setTextWrap(false);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(4, y + 3);
  tft.print(msg);
}

// Manually-computed RGB565 -- Adafruit_ILI9341's color set has no BROWN.
// (Used by uiDrawBatteryIndicator()'s low-battery pulse, below.)
static const uint16_t UI_BROWN = 0xA145;

void uiDrawClock() {
  // Right-aligned, just left of the battery glyph (uiDrawBatteryIndicator),
  // which occupies roughly the rightmost 55px of the bottom edge -- see
  // UI_RIGHTZONE_W. Repainted every loop() tick like the battery glyph, but
  // only actually redraws when the printed string changes (once a minute),
  // same "cheap and idempotent" contract as uiDrawBatteryIndicator.
  static char last[6] = "";
  char buf[6] = "--:--";   // unsynced: no time to show yet
  if (devTimeSynced()) {
    time_t now = devTimeNow() + (time_t)tzOffsetMinutes() * 60;
    struct tm t;
    gmtime_r(&now, &t);   // `now` was already shifted by the tz offset above
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  }
  if (!s_clockForce && strcmp(buf, last) == 0) return;
  s_clockForce = false;
  strcpy(last, buf);

  const int rightEdge = tft.width() - 55 - 4;   // 4px gap before the battery label
  const int textW = 5 * 6;                       // "HH:MM" at text size 1
  int x = rightEdge - textW, y = tft.height() - UI_STATUSBAR_H + 5;
  tft.fillRect(x - 1, y - 1, textW + 2, 10, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(devTimeSynced() ? ILI9341_WHITE : ILI9341_DARKGREY);
  tft.setCursor(x, y);
  tft.print(buf);
}

void uiShowLoading(const char *msg) {
  uiClearBelow(29);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(4, 40);
  tft.print(msg);
}

void uiDrawRotatedText(int cx, int cy, const char *text, uint8_t rotSteps, uint8_t textSize, uint16_t color) {
  int16_t bx, by; uint16_t cw, ch;
  tft.setTextSize(textSize);
  tft.getTextBounds(text, 0, 0, &bx, &by, &cw, &ch);
  if (cw == 0 || ch == 0) return;

  GFXcanvas1 canvas(cw, ch);
  canvas.setTextSize(textSize);
  canvas.setTextColor(1);
  canvas.setCursor(-bx, -by);   // getTextBounds' offset -- draw flush to (0,0)
  canvas.print(text);

  rotSteps %= 4;
  int rw = (rotSteps % 2) ? ch : cw;
  int rh = (rotSteps % 2) ? cw : ch;
  int ox = cx - rw / 2, oy = cy - rh / 2;

  for (int y = 0; y < ch; y++) {
    for (int x = 0; x < cw; x++) {
      if (!canvas.getPixel(x, y)) continue;
      int dx, dy;
      switch (rotSteps) {
        case 1: dx = ch - 1 - y; dy = x; break;                    // 90 CW
        case 2: dx = cw - 1 - x; dy = ch - 1 - y; break;            // 180
        case 3: dx = y;          dy = cw - 1 - x; break;            // 270 CW
        default: dx = x; dy = y; break;                             // 0
      }
      tft.drawPixel(ox + dx, oy + dy, color);
    }
  }
}

void ledSet(bool on)   { digitalWrite(LED_STATUS, on ? LOW : HIGH); }   // red, active low
void ledGreen(bool on) { digitalWrite(LED_GREEN,  on ? LOW : HIGH); }   // green, active low

// "Working" heartbeat: 50ms on / 25ms off while a scan / speed test /
// other job runs. Its own core-0 task so it keeps blinking through
// blocking loops, and a separate GPIO from the red armed-engagement
// blinker so the two never interfere.
//
// Two independent inputs, OR'd: ledBusy() for explicit "job running"
// callers (WiFi scan/connect, speed test, net query) and ledBusyScreen()
// which loop() drives from currentScreen for the always-scanning screens.
// Keeping them separate stops loop()'s per-iteration ledBusyScreen(false)
// from stomping a scan's ledBusy(true). ledBusyAlt() makes each flash
// alternate blue/green (Skimmer).
static volatile bool s_busyReq = false, s_busyScreen = false, s_busyAlt = false;

static void ledBusyTask(void *) {
  pinMode(LED_BUSY, OUTPUT);
  digitalWrite(LED_BUSY, HIGH);            // off (active low)
  bool green = false;
  for (;;) {
    if (s_busyReq || s_busyScreen) {
      int pin = (s_busyAlt && green) ? LED_GREEN : LED_BUSY;
      digitalWrite(pin, LOW);  vTaskDelay(pdMS_TO_TICKS(50));
      digitalWrite(pin, HIGH); vTaskDelay(pdMS_TO_TICKS(25));
      green = !green;
    } else {
      digitalWrite(LED_BUSY, HIGH);
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

void ledBusy(bool on)       { s_busyReq = on; }
void ledBusyScreen(bool on) { s_busyScreen = on; }
void ledBusyAlt(bool on)    { s_busyAlt = on; if (!on) digitalWrite(LED_GREEN, HIGH); }

// "Target detected" cue: 3 quick chirps, each with a green blink. Blocks
// ~0.25s -- meant to be fired on a detection edge, not every scan cycle.
void alertDetected() {
  for (int i = 0; i < 3; i++) {
    ledGreen(true);
    beep(45, 2600);
    ledGreen(false);
    if (i < 2) delay(45);
  }
}

static uint8_t beepVolume = 100;

static void loadBeepVolume() {
  Preferences p;
  p.begin("touchcal", true);
  beepVolume = p.getUChar("vol", 100);
  p.end();
}

int uiGetBeepVolume() { return beepVolume; }

void uiSetBeepVolume(int v) {
  beepVolume = constrain(v, 0, 100);
  Preferences p;
  p.begin("touchcal", false);
  p.putUChar("vol", beepVolume);
  p.end();
}

// PCM sine tone through the shared DAC channel (was a rail-to-rail LEDC
// square -- the "PC speaker" timbre). A sine centred at mid-rail has no DC
// component (the amp stays cool), amplitude scales cleanly with volume, and
// a short attack/release stops the clicks. Blocks the caller until the
// tone has played out.
static int8_t s_sinLut[256];
static bool   s_sinReady = false;
static bool   s_held = false;   // amp + DAC kept powered between beeps

static void primeMidRail() {    // fill the DMA ring with silence
  if (!g_dac) return;
  static uint8_t z[512];
  memset(z, 128, sizeof(z));
  size_t w;
  dac_continuous_write(g_dac, z, sizeof(z), &w, 30);
}

void beepHold(bool on) {
  if (on == s_held || !g_dac) return;
  s_held = on;
  if (on) {
    dac_continuous_enable(g_dac);
    digitalWrite(AUDIO_EN, LOW);
    primeMidRail();
  } else {
    digitalWrite(AUDIO_EN, HIGH);
    dac_continuous_disable(g_dac);
  }
}

void beep(uint32_t ms, uint32_t toneHz) {
  if (beepVolume == 0 || !g_dac) { delay(ms); return; }
  if (!s_sinReady) {
    for (int i = 0; i < 256; i++) s_sinLut[i] = (int8_t)(sinf(i * 6.2831853f / 256.0f) * 127.0f);
    s_sinReady = true;
  }

  bool own = !s_held;                          // one-off beep vs. inside a beepHold() window
  if (own) {
    dac_continuous_enable(g_dac);
    digitalWrite(AUDIO_EN, LOW);
    delay(6);                                  // amp unmute settle
  }

  const int SR = UI_DAC_HZ;
  int nSamp = SR * (int)(ms ? ms : 1) / 1000; if (nSamp < 64) nSamp = 64;
  int atk = nSamp / 6; if (atk < 1) atk = 1; if (atk > 96) atk = 96;
  int amp = beepVolume * 127 / 100;
  uint32_t phase = 0;
  uint32_t inc = (uint32_t)((double)toneHz / SR * 4294967296.0);

  static uint8_t bb[512];
  int done = 0;
  while (done < nSamp) {
    int chunk = nSamp - done; if (chunk > (int)sizeof(bb)) chunk = sizeof(bb);
    for (int k = 0; k < chunk; k++) {
      int i = done + k;
      int env = 64;
      if (i < atk)              env = 64 * i / atk;
      else if (i + atk > nSamp) env = 64 * (nSamp - i) / atk;
      int s = s_sinLut[(phase >> 24) & 0xFF];
      int v = 128 + ((s * amp * env) >> 13);
      bb[k] = v < 0 ? 0 : (v > 255 ? 255 : v);
      phase += inc;
    }
    size_t w = 0;
    esp_err_t e = dac_continuous_write(g_dac, bb, chunk, &w, 100);
    if (e != ESP_OK || w == 0) {
      // Channel latched (DMA underrun -- often after a WiFi/SPI-heavy
      // screen). Rebuild it so the *next* beep works, and bail rather than
      // spin the whole tone at 100ms/chunk and stall the UI.
      uiAudioReset();
      return;
    }
    done += chunk;
  }
  delay(2 + (uint32_t)nSamp * 1000 / SR);      // let the DMA tail play out

  if (own) {
    // One-off beep: tear the channel down clean. No primeMidRail() here --
    // queuing 512 bytes and then immediately disabling was underrunning the
    // DMA and latching the channel.
    digitalWrite(AUDIO_EN, HIGH);
    dac_continuous_disable(g_dac);
  } else {
    primeMidRail();                            // held open: loop silence, not the tone tail
  }
}

// Full teardown + rebuild of the shared DAC channel. A DMA underrun (e.g.
// the MOD task competing with a big SPI blit) or a task killed mid-write
// can latch the continuous channel into a state where dac_continuous_enable()
// quietly fails and every later beep() is silent until reboot. Deleting and
// recreating the channel is the only reliable way back.
void uiAudioReset() {
  s_held = false;
  digitalWrite(AUDIO_EN, HIGH);
  if (g_dac) {
    dac_continuous_disable(g_dac);        // harmless if already disabled
    dac_continuous_del_channels(g_dac);
    g_dac = nullptr;
  }
  dacInit();                              // recreate, left disabled (beep() enables on demand)
}

void rangeBeep(int rssi) {
  static uint32_t last = 0;
  const int LO = -95, HI = -35;                 // "undetectable" .. "on top of it"
  int r = rssi < LO ? LO : (rssi > HI ? HI : rssi);
  float s = (float)(r - LO) / (HI - LO);        // 0 far .. 1 close
  // Geometric rate so the acceleration is unmistakable as you home in:
  // ~900ms between chirps when far, ~150ms when right on the target.
  uint32_t interval = (uint32_t)(900.0f * powf(0.17f, s));
  if (interval < 150) interval = 150;
  if (millis() - last < interval) return;
  last = millis();
  // 45ms chirp -- a 12ms one was mostly swallowed by the amp unmute ramp
  // and inaudible. Pitch kept off the very low end (quiet on this speaker).
  uint32_t tone = 750 + (uint32_t)(s * s * 2050.0f);   // ~750 -> ~2800 Hz
  beep(45, tone);
}

// --- battery ---------------------------------------------------------------
// CYD wires the LiPo through a ~2:1 divider to GPIO34 (BAT_ADC, ADC1_CH6).
// analogReadMilliVolts() applies the chip's eFuse ADC calibration; x2 for
// the divider (LCDWiki board's usual 100k/100k). The real divider + ADC cal
// drift unit to unit, so a user factor (System > Hardware > Battery
// calibrate) scales the result: uiBatteryMv() = raw * s_batCal.
// No charge-status line is broken out to a GPIO on this board.
static const float BAT_DIV = 2.0f;
static float s_batCal  = 1.0f;
static bool  s_batCalLoaded = false;

void uiBatteryCalLoad() {
  Preferences p;
  p.begin("batcal", true);
  s_batCal = p.getFloat("k", 1.0f);
  p.end();
  if (!(s_batCal >= 0.5f && s_batCal <= 2.0f)) s_batCal = 1.0f;   // NaN-safe clamp
  s_batCalLoaded = true;
}
static void batCalPersist() {
  Preferences p;
  p.begin("batcal", false);
  p.putFloat("k", s_batCal);
  p.end();
}

int uiBatteryRawMv() {
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(BAT_ADC);
  return (int)((acc / 16.0f) * BAT_DIV + 0.5f);
}

int uiBatteryMv() {
  if (!s_batCalLoaded) uiBatteryCalLoad();
  return (int)(uiBatteryRawMv() * s_batCal + 0.5f);
}

float uiBatteryCal() { if (!s_batCalLoaded) uiBatteryCalLoad(); return s_batCal; }

void uiBatterySetCal(float k) {
  if (k < 0.5f) k = 0.5f;
  if (k > 2.0f) k = 2.0f;
  s_batCal = k;
  s_batCalLoaded = true;
  batCalPersist();
}

void uiBatterySetCalFromActual(int actualMv) {
  int raw = uiBatteryRawMv();
  if (raw < 500 || actualMv < 500) return;   // nonsense guard (no pack / typo)
  uiBatterySetCal((float)actualMv / (float)raw);
}

// Rough 1S state-of-charge from a resting-voltage LUT (no load compensation
// -- reads low under a heavy scan). Returns -1 for "no cell": >4300mV is the
// charger rail with nothing attached; <2800mV is a dead/absent pack.
static int battPct(int mv) {
  if (mv > 4300 || mv < 2800) return -1;
  static const int lut[][2] = {
    {4200,100},{4100,90},{4000,80},{3900,68},{3800,55},{3750,45},
    {3700,35},{3650,25},{3600,15},{3500,8},{3400,3},{3300,0},
  };
  const int N = sizeof(lut) / sizeof(lut[0]);
  if (mv >= lut[0][0]) return 100;
  for (int i = 1; i < N; i++)
    if (mv >= lut[i][0]) {
      int v0 = lut[i-1][0], p0 = lut[i-1][1], v1 = lut[i][0], p1 = lut[i][1];
      return p1 + (p0 - p1) * (mv - v1) / (v0 - v1);
    }
  return 0;
}

int uiBatteryPct() { return battPct(uiBatteryMv()); }

static uint32_t s_batReadAt = 0, s_batPaintAt = 0;
static int s_batMv = 0, s_batPct = -2;   // -2 = never read
static int s_batSig = -9999;            // last-painted {pct, pulse-colour}

// Bottom-right corner, rightmost. The clock (uiDrawClock) sits just to its
// left. Drawn from loop() every iteration like the clock -- only the ADC
// read is throttled (5s); the ~50px repaint is cheap and keeps the glyph
// alive after any screen's uiClearBelow().
void uiDrawBatteryIndicator() {
  uint32_t now = millis();
  if (s_batPct == -2 || now - s_batReadAt > 5000) {
    s_batReadAt = now;
    s_batMv  = uiBatteryMv();
    s_batPct = battPct(s_batMv);
  }
  bool low = (s_batPct >= 0 && s_batPct < 15);

  static const uint16_t cyc[5] = {ILI9341_WHITE, ILI9341_YELLOW, ILI9341_ORANGE, UI_BROWN, ILI9341_RED};
  int cycIdx = 0;
  uint16_t col;
  if (s_batPct < 0)        col = ILI9341_DARKGREY;
  else if (low) { uint32_t st = (now / 250) % 8; cycIdx = st <= 4 ? st : 8 - st; col = cyc[cycIdx]; }
  else if (s_batPct < 40)  col = ILI9341_YELLOW;
  else                     col = ILI9341_GREEN;

  // Only repaint when the rendered content actually changes -- redrawing
  // every loop() strobed the icon. Force one on a screen change, and one
  // at least every 1.5s so a screen that clears this corner every frame
  // (wardrive) gets the glyph back.
  int sig = s_batPct * 16 + (low ? cycIdx : 8);
  if (!s_batForce && sig == s_batSig && now - s_batPaintAt < 1500) return;
  s_batForce = false;
  s_batSig = sig;
  s_batPaintAt = now;

  const int nubW = 2, bodyW = 20, bodyH = 10;
  int bx = tft.width() - 3 - nubW - bodyW;    // body left edge
  int by = tft.height() - 3 - bodyH;

  // Fixed 4-char label ("100%", " 42%", "  5%", " USB") drawn OPAQUE (fg on
  // black) so no separate clear step -- the char cells always cover the
  // same pixels regardless of value.
  char lbl[6];
  if (s_batPct < 0) snprintf(lbl, sizeof(lbl), " USB");
  else              snprintf(lbl, sizeof(lbl), "%3d%%", s_batPct);
  tft.setTextSize(1);
  tft.setTextColor(col, ILI9341_BLACK);
  tft.setCursor(bx - 3 - 4 * 6, by + 1);
  tft.print(lbl);

  tft.drawRect(bx, by, bodyW, bodyH, col);
  tft.fillRect(bx + bodyW, by + 3, nubW, bodyH - 6, col);
  int fw = (s_batPct >= 0) ? (bodyW - 4) * s_batPct / 100 : 0;
  tft.fillRect(bx + 2, by + 2, bodyW - 4, bodyH - 4, ILI9341_BLACK);   // interior
  if (fw > 0) tft.fillRect(bx + 2, by + 2, fw, bodyH - 4, col);        // charge bar
}

// --- numeric keypad ------------------------------------------------------
// Deliberately NOT folded into keyboard.cpp's uiTextInput(): that grid is
// all about letter layers + case + punctuation for Wi-Fi passphrases, none
// of which apply here, and an IP/port field wants far bigger tap targets
// than the 10-per-row QWERTY. This is its own tiny modal: prompt line, a
// size-2 field, and a 3-wide grid of 1-9 / . 0 <BKSP> / Cancel OK. Single-
// fire per tap for every key (backspace included -- IP strings are short,
// repeat-while-held isn't worth the state). Cancel and a tap in the top-
// left back-button area both return `initial` unchanged, matching
// uiTextInput()'s contract so callers can treat the two the same way.
String uiNumpadInput(const char *prompt, const String &initial) {
  // 14 cells. Control codes that can never appear in a numeric string
  // (\b, \n, ESC) share the label slot the way keyboard.cpp does it.
  const char C_BKSP = '\b', C_OK = '\n', C_CANCEL = 27;
  struct NKey { Btn b; char c; };
  NKey k[14];
  static const char *DIG[9] = {"1","2","3","4","5","6","7","8","9"};

  const int gx = 4, gTop = 48;
  const int cw = (tft.width() - 2 * gx) / 3;      // ~104 px columns
  const int chh = 34, gap = 4;
  auto place = [&](int idx, int col, int row, int span, const char *lbl, char code) {
    k[idx].b = { gx + col * cw, gTop + row * (chh + gap), span * cw - gap, chh, lbl };
    k[idx].c = code;
  };
  for (int i = 0; i < 9; i++) place(i, i % 3, i / 3, 1, DIG[i], (char)('1' + i));
  place(9,  0, 3, 1, ".",      '.');
  place(10, 1, 3, 1, "0",      '0');
  place(11, 2, 3, 1, "<",      C_BKSP);
  place(12, 0, 4, 1, "Cancel", C_CANCEL);
  place(13, 1, 4, 2, "OK",     C_OK);

  String buf = initial;
  bool done = false, cancelled = false;

  uiClearBelow(0);
  tft.setTextWrap(false);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.print(prompt);
  for (int i = 0; i < 14; i++) uiDrawButton(k[i].b);

  auto redrawField = [&]() {
    tft.fillRect(2, 16, tft.width() - 4, 26, ILI9341_BLACK);
    tft.drawRect(2, 16, tft.width() - 4, 26, ILI9341_WHITE);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 20);
    tft.print(buf);
  };
  redrawField();

  while (!done && !cancelled) {
    TouchPoint t = uiReadTouch();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInBackArea(t)) { cancelled = true; break; }
    for (int i = 0; i < 14; i++) {
      if (!uiTouchInButton(t, k[i].b)) continue;
      char c = k[i].c;
      if (c == C_OK)           done = true;
      else if (c == C_CANCEL)  cancelled = true;
      else if (c == C_BKSP)  { if (buf.length()) buf.remove(buf.length() - 1); }
      else if (c == '.')     { if (buf.indexOf('.') < 0 && buf.length() < 20) buf += '.'; }
      else                   { if (buf.length() < 20) buf += c; }
      redrawField();
      uiWaitForRelease();   // one character per finger-down, like the keyboard
      break;
    }
  }
  return cancelled ? initial : buf;
}
