#include "ui.h"
#include <SPI.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include "driver/dac_continuous.h"
#include "devtime.h"
#include "tz.h"
#include "demomode.h"
#include "theme.h"
#include "accent.h"
#include "debuglog.h"
#include "bg_landscape.h"   // 160x120 landscape image array.
#include "bg_portrait.h"    // 120x160 portrait image array.

// One shared DAC channel on GPIO26 handles all audio. The system enables it only during playback.
static dac_continuous_handle_t g_dac = nullptr;
static const int UI_DAC_HZ = 22050;
dac_continuous_handle_t uiDac() { return g_dac; }
int uiDacRate() { return UI_DAC_HZ; }

uint16_t uiSevColor(uint8_t sev) {
  return sev == UI_SEV_ALERT ? ILI9341_RED : (sev == UI_SEV_WATCH ? ILI9341_YELLOW : ILI9341_GREEN);
}
const char *uiSevTag(uint8_t sev) {
  return sev == UI_SEV_ALERT ? "ALERT" : (sev == UI_SEV_WATCH ? "watch" : "ok");
}

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

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, -1);   // Reset pin shares the EN line. See pins.h.

static bool s_batForce = false;   // Hoist this value so both battery and clock functions share it.
static bool s_clockForce = false; // Hoist this value so both battery and clock functions share it.
static int s_batPct = -2;         // -2 means the value is unread.
static int  s_bgMode  = UI_BG_BLACK;
void uiSetBgMode(int m) { s_bgMode = m; }

// Cache this flag in RAM for fast access during every screen draw.
static bool s_listBg = true;
static void loadListBg() {
  Preferences p;
  p.begin("disp", true);
  s_listBg = p.getBool("listbg", true);
  p.end();
}
bool uiListBgEnabled() { return s_listBg; }
void uiSetListBgEnabled(bool on) {
  s_listBg = on;
  Preferences p;
  p.begin("disp", false);
  p.putBool("listbg", on);
  p.end();
}
static void loadBeepVolume();   // defined below beep(); forward-declared for uiInit()
static void ledBusyTask(void *); // defined below ledSet(); forward-declared for uiInit()

// Bit-bang the XPT2046 touch controller. The pins lack hardware SPI support.
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

// Persist touch calibration in NVS. The physical overlay stays fixed. Screen rotation changes the raw axis mapping. The system measures the mapping per rotation. It uses a three-point tap to find the correct axes. This avoids incorrect mathematical assumptions.
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
  DLOG("cal", "uiSetRotation rot=%u valid=%d", displayRotation, (int)cal[displayRotation].valid);
  if (!cal[displayRotation].valid) uiRunCalibration();   // this orientation's never been calibrated
}

void uiCycleRotation() { uiSetRotation((displayRotation + 1) % 4); }

TouchPoint uiReadTouch() {
  // Track the physical press state across calls. This keeps the rising-edge detector in sync.
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
  // Drain any stale touch state before starting. The raw reader avoids using uncalibrated coordinates.
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

  // Use three points in an L shape. The first two points isolate horizontal movement. The first and third isolate vertical movement. This measures the axis mapping directly.
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

  ledcAttach(LED_STATUS, 5000, 8);
  ledcAttach(LED_BUSY, 5000, 8);
  ledcAttach(LED_GREEN, 5000, 8);   // all 3 channels PWM-capable -- see ledColorRGB()/ledColorAccent()
  ledColorRGB(0, 0, 0);             // all off (active low, handled inside ledColorRGB())
  pinMode(AUDIO_EN, OUTPUT);   digitalWrite(AUDIO_EN, HIGH);
  pinMode(BOOT_KEY, INPUT_PULLUP);

  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  loadRotation();
  tft.setRotation(displayRotation);
  tft.fillScreen(ILI9341_BLACK);

  loadCalFor(displayRotation);
  DLOG("cal", "uiInit rot=%u valid=%d", displayRotation, (int)cal[displayRotation].valid);
  if (!cal[displayRotation].valid) uiRunCalibration();

  loadBeepVolume();
  uiBatteryCalLoad();
  loadListBg();
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

// Return the half-resolution scene art pointer. Landscape and portrait arrays share the same data.
static const uint16_t *bgSrc(int &sw, int &sh) {
  if (tft.width() >= tft.height()) { sw = 160; sh = 120; return BG_LANDSCAPE; }
  sw = 120; sh = 160; return BG_PORTRAIT;
}

static inline uint16_t avg565(uint16_t a, uint16_t b) {
  uint16_t r = (((a >> 11) & 31) + ((b >> 11) & 31)) / 2;
  uint16_t g = (((a >> 5)  & 63) + ((b >> 5)  & 63)) / 2;
  uint16_t bl = ((a & 31) + (b & 31)) / 2;
  return (r << 11) | (g << 5) | bl;
}
static inline uint16_t avg565_4(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
  uint16_t r = (((a >> 11) & 31) + ((b >> 11) & 31) + ((c >> 11) & 31) + ((d >> 11) & 31)) / 4;
  uint16_t g = (((a >> 5)  & 63) + ((b >> 5)  & 63) + ((c >> 5)  & 63) + ((d >> 5)  & 63)) / 4;
  uint16_t bl = ((a & 31) + (b & 31) + (c & 31) + (d & 31)) / 4;
  return (r << 11) | (g << 5) | bl;
}

// Soften the 2x upscale of half-resolution art. Blending reduces blockiness. The function uses at most four integer reads. Other modules reuse this routine.
uint16_t uiBgSample(const uint16_t *img, int sw, int sh, int xx, int yy) {
  int sx = xx >> 1; if (sx >= sw) sx = sw - 1; if (sx < 0) sx = 0;
  int sy = yy >> 1; if (sy >= sh) sy = sh - 1; if (sy < 0) sy = 0;
  bool blendX = xx & 1, blendY = yy & 1;
  uint16_t p00 = pgm_read_word(&img[sy * sw + sx]);
  if (!blendX && !blendY) return p00;
  int sxN = sx + 1; if (sxN >= sw) sxN = sw - 1;
  int syN = sy + 1; if (syN >= sh) syN = sh - 1;
  if (blendX && !blendY) return avg565(p00, pgm_read_word(&img[sy * sw + sxN]));
  if (!blendX && blendY) return avg565(p00, pgm_read_word(&img[syN * sw + sx]));
  return avg565_4(p00, pgm_read_word(&img[sy * sw + sxN]),
                   pgm_read_word(&img[syN * sw + sx]), pgm_read_word(&img[syN * sw + sxN]));
}

void uiClearRect(int x, int y, int w, int h) {
  if (!themeIsVice()) { tft.fillRect(x, y, w, h, ILI9341_BLACK); return; }

  if (s_bgMode == UI_BG_IMAGE) {
    int sw, sh;
    const uint16_t *img = bgSrc(sw, sh);
    static uint16_t line[320];
    tft.startWrite();
    for (int yy = y; yy < y + h; yy++) {
      int n = 0;
      for (int xx = x; xx < x + w && n < 320; xx++, n++)
        line[n] = uiBgSample(img, sw, sh, xx, yy);
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

bool uiFieldChanged(char *prevBuf, size_t prevBufSz, const char *newText) {
  if (strncmp(prevBuf, newText, prevBufSz) == 0) return false;
  strncpy(prevBuf, newText, prevBufSz - 1);
  prevBuf[prevBufSz - 1] = '\0';
  return true;
}

bool uiDrawFieldIfChanged(int x, int y, int w, int h, uint16_t fg, uint8_t sz,
                          char *prevBuf, size_t prevBufSz, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (!uiFieldChanged(prevBuf, prevBufSz, buf)) return false;
  uiClearRect(x, y, w, h);
  tft.setTextColor(fg);
  tft.setTextSize(sz);
  tft.setCursor(x, y);
  tft.print(buf);
  return true;
}

void uiDrawListIfChanged(int x, int rowY0, int rowW, int rowH, int count, int maxRows,
                         char prevBufs[][UI_LIST_SIG_LEN],
                         const std::function<void(int i, char *sigOut, size_t sigCap)> &rowSignature,
                         const std::function<void(int i)> &drawRow) {
  char sig[UI_LIST_SIG_LEN];
  for (int i = 0; i < maxRows; i++) {
    if (i < count) rowSignature(i, sig, sizeof(sig));
    else sig[0] = '\0';   // slot no longer in use -- treat as a blank row
    if (!uiFieldChanged(prevBufs[i], UI_LIST_SIG_LEN, sig)) continue;
    uiClearRect(x, rowY0 + i * rowH, rowW, rowH);
    if (i < count) drawRow(i);
  }
}

void uiDrawTopBar(const char *title) {
  // Set the background mode. List screens use the scene image if enabled. Menu screens override this setting.
  s_bgMode = uiListBgEnabled() ? UI_BG_IMAGE : UI_BG_BLACK;
  tft.fillRect(0, 0, tft.width(), 28, accentTitleBar());   // universal -- Basic used a fixed navy before
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

// Test the button area without checking for a new press. This detects held touches.
bool uiTouchInBackArea(const TouchPoint &t) {
  return t.pressed && t.x >= kBackBtn.x && t.x < kBackBtn.x + kBackBtn.w &&
         t.y >= kBackBtn.y && t.y < kBackBtn.y + kBackBtn.h;
}

void uiDrawButton(const Btn &b) {
  if (themeIsVice()) {
    int r = b.h / 2; if (r > 10) r = 10; if (r < 3) r = 3;
    tft.fillRoundRect(b.x, b.y, b.w, b.h, r, accentFill());
    tft.drawRoundRect(b.x, b.y, b.w, b.h, r, accentEdge());
    if (b.w > 4 && b.h > 4)
      tft.drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, r - 1, accentEdge());
    tft.drawFastHLine(b.x + r, b.y + 2, b.w - 2 * r, accentBevel());

    // Scale the label down until it fits the button. This applies a global text size limit.
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
    tft.setTextColor(accentEmboss());
    tft.setCursor(tx - 1, ty - 1); tft.print(b.label);
    tft.setTextColor(TH_BTN_TEXT);
    tft.setCursor(tx, ty);         tft.print(b.label);
    tft.setTextSize(1);
    return;
  }

  tft.setTextSize(1);
  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(b.label, 0, 0, &bx, &by, &bw, &bh);
  tft.drawRect(b.x, b.y, b.w, b.h, accentLabel());   // universal -- was a fixed white border/text
  tft.setTextColor(accentLabel());
  tft.setCursor(b.x + (b.w - (int)bw) / 2, b.y + (b.h - (int)bh) / 2);
  tft.print(b.label);
}

void uiDrawMenuButton(const Btn &b) { uiDrawButton(b); }

uint16_t uiContrastText(uint16_t c) {
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  int luma = (r * 255 / 31) * 299 + (g * 255 / 63) * 587 + (b * 255 / 31) * 114;
  return luma / 1000 > 140 ? ILI9341_BLACK : ILI9341_WHITE;
}

void uiDrawButtonColored(const Btn &b, uint16_t fill) {
  if (themeIsVice()) {
    int r = b.h / 2; if (r > 10) r = 10; if (r < 3) r = 3;
    tft.fillRoundRect(b.x, b.y, b.w, b.h, r, fill);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, r, fill);
  } else {
    tft.drawRect(b.x, b.y, b.w, b.h, fill);
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
  int tx = b.x + (b.w - (int)bw) / 2 - bx;
  int ty = b.y + (b.h - (int)bh) / 2 - by;
  tft.setTextColor(themeIsVice() ? uiContrastText(fill) : fill);
  tft.setCursor(tx, ty);
  tft.print(b.label);
  tft.setTextSize(1);
}

void uiDrawButtonTricolor(const Btn &b, uint16_t fill, uint16_t edge, uint16_t text) {
  if (themeIsVice()) {
    int r = b.h / 2; if (r > 10) r = 10; if (r < 3) r = 3;
    tft.fillRoundRect(b.x, b.y, b.w, b.h, r, fill);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, r, edge);
  } else {
    tft.fillRect(b.x, b.y, b.w, b.h, fill);
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
  int tx = b.x + (b.w - (int)bw) / 2 - bx;
  int ty = b.y + (b.h - (int)bh) / 2 - by;
  tft.setTextColor(text);
  tft.setCursor(tx, ty);
  tft.print(b.label);
  tft.setTextSize(1);
}

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

void uiDrawPager(int y, int page, int pages, Btn &prevBtn, Btn &nextBtn) {
  prevBtn = {8, y, 60, UI_PAGER_H, "< prev"};
  nextBtn = {tft.width() - 68, y, 60, UI_PAGER_H, "next >"};
  if (page > 0)          uiDrawButton(prevBtn);    else uiDrawButtonDim(prevBtn);
  if (page < pages - 1)  uiDrawButton(nextBtn);    else uiDrawButtonDim(nextBtn);
  tft.setTextColor(ILI9341_WHITE);
  char pg[16];
  snprintf(pg, sizeof(pg), "%d / %d", page + 1, pages);
  int16_t bx, by; uint16_t bw, bh;
  tft.setTextSize(1);
  tft.getTextBounds(pg, 0, 0, &bx, &by, &bw, &bh);
  tft.setCursor((tft.width() - (int)bw) / 2, y + (UI_PAGER_H - (int)bh) / 2 - by);
  tft.print(pg);
}

int uiPagerLayout(int y0, int rowH, int gap, int bottomMargin, int pagerGap,
                  int count, int hardCap, int cols,
                  int &itemsPerPage, int &pages, int &pagerY) {
  int rowsPerPage = (tft.height() - bottomMargin - UI_PAGER_H - pagerGap - y0 + gap) / (rowH + gap);
  if (rowsPerPage < 1) rowsPerPage = 1;
  if (rowsPerPage > hardCap) rowsPerPage = hardCap;
  itemsPerPage = rowsPerPage * cols;
  if (count > 0 && itemsPerPage > count) itemsPerPage = count;
  pages = count > 0 ? (count + itemsPerPage - 1) / itemsPerPage : 1;
  pagerY = y0 + rowsPerPage * (rowH + gap) - gap + pagerGap;
  return rowsPerPage;
}

int uiDropdownPick(const char *title, int count, const char *(*itemLabel)(int), int current) {
  if (count <= 0) return current;
  const int MAX_ROWS = 10;
  const int y0 = 34, rowH = 30, gap = 4, bottomMargin = UI_STATUSBAR_H + 4, pagerGap = 6;
  // Room is always reserved for the pager row -- see uiDrawPager()'s
  // comment on why it's drawn even at one page, rather than the layout
  // changing shape depending on whether paging is actually needed.
  int maxRows, pages, pagerY;
  uiPagerLayout(y0, rowH, gap, bottomMargin, pagerGap, count, MAX_ROWS, 1, maxRows, pages, pagerY);
  int page = (current >= 0 && current < count) ? current / maxRows : 0;

  Btn items[MAX_ROWS], prevBtn, nextBtn;
  int n = 0;   // rows actually drawn on the current page -- read back in the touch loop below
  auto draw = [&]() {
    uiDrawTopBar(title);
    uiClearBelow(29);
    int base = page * maxRows;
    n = min(maxRows, count - base);
    int y = y0;
    for (int i = 0; i < n; i++) {
      int idx = base + i;
      items[i] = {8, y, tft.width() - 16, rowH, itemLabel(idx)};
      uiDrawButton(items[i]);
      if (idx == current)
        tft.drawRect(items[i].x - 3, items[i].y - 3, items[i].w + 6, items[i].h + 6, ILI9341_GREEN);
      y += rowH + gap;
    }
    uiDrawPager(pagerY, page, pages, prevBtn, nextBtn);
  };
  draw();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return current; }
    int base = page * maxRows;
    for (int i = 0; i < n; i++) {
      if (uiTouchInButton(t, items[i])) {
        uiWaitForRelease();
        return base + i;   // tap = select AND close, dropdown-style
      }
    }
    if (uiTouchInButton(t, prevBtn) && page > 0) { uiWaitForRelease(); page--; draw(); continue; }
    if (uiTouchInButton(t, nextBtn) && page < pages - 1) { uiWaitForRelease(); page++; draw(); continue; }
    delay(15);
  }
}

bool uiTouchInButton(const TouchPoint &t, const Btn &b) {
  // Check for a new press only. A held touch must not trigger repeated actions.
  return t.isNewPress && t.x >= b.x && t.x < b.x + b.w && t.y >= b.y && t.y < b.y + b.h;
}

// --- toast (status-bar message), with a step-scroll ticker for anything
// too long to fit -----------------------------------------------------
// Scroll long messages in the status bar. Character stepping avoids pixel-level canvas blitting. This saves CPU and SPI bandwidth during screen updates.
static String   s_toastMsg;
static bool     s_toastScrolling = false;
static int      s_toastCharsFit = 0;
static int      s_toastOffset = 0;
static uint32_t s_toastLastStep = 0;
static const uint32_t TOAST_STEP_MS = 350;
static const char *TOAST_LOOP_GAP = "     ";   // separates the end from the restart

static void toastDrawWindow(const String &window) {
  int y = tft.height() - UI_STATUSBAR_H + 1;
  int w = tft.width() - UI_RIGHTZONE_W;   // clock (uiDrawClock) + battery glyph live past here
  tft.fillRect(0, y, w, UI_STATUSBAR_H - 1, ILI9341_BLACK);
  tft.setTextWrap(false);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(4, y + 3);
  tft.print(window);
}

void uiToast(const char *msg) {
  int w = tft.width() - UI_RIGHTZONE_W;
  s_toastMsg = msg;
  s_toastCharsFit = (w - 4) / 6;   // default font: 6px advance/char at text size 1
  if (s_toastCharsFit < 1) s_toastCharsFit = 1;

  if ((int)s_toastMsg.length() <= s_toastCharsFit) {
    s_toastScrolling = false;
    toastDrawWindow(s_toastMsg);
  } else {
    s_toastScrolling = true;
    s_toastOffset = 0;
    s_toastLastStep = millis();
    toastDrawWindow(s_toastMsg.substring(0, s_toastCharsFit));
  }
}

void uiClearToast() {
  // Stop the ticker when clearing the toast. The ticker runs continuously across all screens.
  s_toastScrolling = false;
  s_toastMsg = "";
  int y = tft.height() - UI_STATUSBAR_H + 1;
  int w = tft.width() - UI_RIGHTZONE_W;
  tft.fillRect(0, y, w, UI_STATUSBAR_H - 1, ILI9341_BLACK);
}

// Advance the ticker if a long message is active. The function throttles updates to prevent flickering.
static void uiTickToast() {
  if (!s_toastScrolling) return;
  uint32_t now = millis();
  if (now - s_toastLastStep < TOAST_STEP_MS) return;
  s_toastLastStep = now;

  String loopMsg = s_toastMsg + TOAST_LOOP_GAP;
  int total = loopMsg.length();
  s_toastOffset = (s_toastOffset + 1) % total;

  String window;
  for (int i = 0; i < s_toastCharsFit; i++) window += loopMsg[(s_toastOffset + i) % total];
  toastDrawWindow(window);
}

// Define a brown color. The display driver lacks a built-in brown constant.
static const uint16_t UI_BROWN = 0xA145;

// Return the battery status color. Warning colors stay fixed for functional clarity. The healthy state uses the accent color.
static uint16_t battStatusColor() {
  if (s_batPct < 0) return ILI9341_DARKGREY;              // USB / never read yet
  if (s_batPct < 15) {                                     // low: pulse through these
    static const uint16_t cyc[5] = {ILI9341_WHITE, ILI9341_YELLOW, ILI9341_ORANGE, UI_BROWN, ILI9341_RED};
    uint32_t st = (millis() / 250) % 8;
    return cyc[st <= 4 ? st : 8 - st];
  }
  if (s_batPct < 40) return ILI9341_YELLOW;
  return accentFill();
}

void uiDrawClock() {
  // Draw the clock next to the battery indicator. The function only redraws when the time string changes.
  static char last[7] = "";
  char buf[7] = "--:--";   // unsynced: no time to show yet
  if (devTimeSynced()) {
    time_t now = devTimeNow() + (time_t)tzOffsetMinutes() * 60;
    struct tm t;
    gmtime_r(&now, &t);   // `now` was already shifted by the tz offset above
    if (tzUse24h()) {
      snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    } else {
      int h12 = t.tm_hour % 12;
      if (h12 == 0) h12 = 12;
      snprintf(buf, sizeof(buf), "%d:%02d%c", h12, t.tm_min, t.tm_hour < 12 ? 'a' : 'p');
    }
  }
  bool changed = uiFieldChanged(last, sizeof(last), buf);
  if (!s_clockForce && !changed) return;
  s_clockForce = false;

  const int rightEdge = tft.width() - 55 - 4;   // 4px gap before the battery label
  const int textW = 6 * 6;                       // widest case, "12:34p", at text size 1
  int x = rightEdge - textW, y = tft.height() - UI_STATUSBAR_H + 5;
  tft.fillRect(x - 1, y - 1, textW + 2, 10, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(battStatusColor());   // same color as the battery glyph/label next to it
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

bool uiDrawLocateReading(int x, int y, int w, uint16_t presentColor,
                         int rssiMin, int rssiMax, int *prevRssi, int rssi) {
  if (rssi == *prevRssi) return false;
  *prevRssi = rssi;
  bool present = rssi > -127;
  const int numH = 34, gap = 4, barH = 24;

  uiClearRect(x, y, w, numH);
  tft.setTextSize(3);
  tft.setTextColor(present ? presentColor : ILI9341_DARKGREY);
  tft.setCursor(x + 2, y);
  if (present) tft.printf("%4d dBm", rssi); else tft.print(" -- lost");

  int barY = y + numH + gap;
  int barW = present ? map(constrain(rssi, rssiMin, rssiMax), rssiMin, rssiMax, 0, w - 2) : 0;
  uiClearRect(x + 1, barY, w - 2, barH);
  int third = (rssiMax - rssiMin) / 3;
  uint16_t barCol = rssi > rssiMax - third      ? ILI9341_GREEN :
                    rssi > rssiMax - 2 * third  ? ILI9341_YELLOW : ILI9341_RED;
  if (barW > 0) tft.fillRect(x + 1, barY, barW, barH, barCol);
  return true;
}

// Control the status LED via the PWM channel. Direct digital writes detach the channel.
void ledSet(bool on)   { ledcWrite(LED_STATUS, on ? 0 : 255); }   // red, active low
// Control the green LED via the PWM channel. The pin supports hard switching and fading. Full brightness uses a zero duty cycle.
void ledGreenPwm(uint8_t brightness) { ledcWrite(LED_GREEN, 255 - brightness); }
void ledGreen(bool on) { ledGreenPwm(on ? 255 : 0); }

// Write values to all three LED channels. The system uses an active-low convention.
void ledColorRGB(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(LED_STATUS, 255 - r);
  ledcWrite(LED_BUSY,   255 - b);
  ledcWrite(LED_GREEN,  255 - g);
}

// Apply the accent color to all LEDs. The intensity scales the output.
void ledColorAccent(float intensity) {
  uint8_t r, g, b;
  accentRGB(accentFill(), r, g, b);
  ledColorRGB((uint8_t)(r * intensity), (uint8_t)(g * intensity), (uint8_t)(b * intensity));
}

// Run a background task to render all LED channels. This prevents timer conflicts. The task follows a strict priority order. A suspension flag lets external code take full control.
static volatile bool s_busyReq = false, s_busyScreen = false;
static volatile bool s_alertActive = false;
static volatile bool s_heartbeatOn = false;
static volatile bool s_connectedActive = false;
static volatile bool s_renderSuspended = false;

static void ledBusyTask(void *) {
  for (;;) {
    if (s_renderSuspended) {
      // do nothing -- a caller elsewhere owns the shared channels right now
    } else if (s_alertActive) {
      uint32_t cyclePos = millis() % 500;
      bool isRed = cyclePos < 250;
      uint32_t t = cyclePos % 250;
      float k = (t < 125) ? (t / 125.0f) : ((250 - t) / 125.0f);
      uint8_t v = (uint8_t)(255 * k);
      if (isRed) ledColorRGB(v, 0, 0);
      else       ledColorRGB(v, v, 0);
    } else if (s_busyReq || s_busyScreen) {
      uint32_t t = millis() % 750;
      float k = (t < 500) ? (t / 500.0f) : 0.0f;
      ledColorAccent(k);
    } else if (s_connectedActive) {
      ledColorAccent(1.0f);
    } else {
      ledColorRGB(s_heartbeatOn ? 255 : 0, 0, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void ledBusy(bool on)       { s_busyReq = on; }
void ledBusyScreen(bool on) { s_busyScreen = on; }
void ledHeartbeat(bool on)  { s_heartbeatOn = on; }
void ledAlert(bool on)      { s_alertActive = on; }
void ledConnected(bool on)  { s_connectedActive = on; }
void ledSuspendRender(bool suspend) {
  s_renderSuspended = suspend;
  if (suspend) ledColorRGB(0, 0, 0);   // hand off from a known-off state, not whatever was mid-fade
}

// Play a detection alert. The function blocks for a quarter second.
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

// Generate a PCM sine tone on the shared DAC. The mid-rail offset prevents DC current. The attack and release phases stop audio clicks.
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
      // Handle a DMA underrun. Rebuild the channel and exit to prevent UI stalls.
      uiAudioReset();
      return;
    }
    done += chunk;
  }
  delay(2 + (uint32_t)nSamp * 1000 / SR);      // let the DMA tail play out

  if (own) {
    // Disable the channel after a one-off beep. Skipping the silence queue prevents DMA latching.
    digitalWrite(AUDIO_EN, HIGH);
    dac_continuous_disable(g_dac);
  } else {
    primeMidRail();                            // held open: loop silence, not the tone tail
  }
}

// Rebuild the DAC channel to recover from errors. DMA underruns or killed tasks can latch the hardware. A full teardown restores normal operation.
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
  // Play a chirp at a fixed duration. The pitch avoids the speaker's quiet range.
  uint32_t tone = 750 + (uint32_t)(s * s * 2050.0f);   // ~750 -> ~2800 Hz
  beep(45, tone);
}

// --- battery ---------------------------------------------------------------
// Read the battery voltage through a voltage divider. The system applies ADC calibration. A user factor scales the result. The board lacks a charge-status pin.
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

// Estimate charge from a resting voltage table. The function returns -1 for missing cells. High voltage indicates an unconnected charger. Low voltage indicates a dead pack.
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
static int s_batMv = 0;                 // s_batPct itself lives up top -- see the comment there
static char s_batSig[8] = "";           // last-painted {pct, pulse-colour}

// Draw the battery indicator in the bottom-right corner. The function throttles ADC reads but redraws frequently to survive screen clears.
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
  else                     col = accentFill();   // "all is well" -- tied to the accent, see battStatusColor()

  // Skip redraws when the content matches the previous frame. Force a refresh on screen changes and every 1.5 seconds.
  char sig[8];
  snprintf(sig, sizeof(sig), "%d", s_batPct * 16 + (low ? cycIdx : 8));
  bool changed = uiFieldChanged(s_batSig, sizeof(s_batSig), sig);
  if (!s_batForce && !changed && now - s_batPaintAt < 1500) return;
  s_batForce = false;
  s_batPaintAt = now;

  const int nubW = 2, bodyW = 20, bodyH = 10;
  int bx = tft.width() - 3 - nubW - bodyW;    // body left edge
  int by = tft.height() - 3 - bodyH;

  // Draw the percentage label over a black background. This covers the same pixels every time.
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

// Blink a demo mode indicator in the status bar. The function shares the left half with the toast ticker.
static void uiTickDemoBadge() {
  static int8_t lastState = -1;   // -1/0/1: not-yet-drawn / off / on -- forces a first draw
  if (!demoModeEnabled() || s_toastScrolling || s_toastMsg.length()) { lastState = -1; return; }
  // Throttle the blink rate to prevent flickering.
  bool on = (millis() / 600) % 2 == 0;
  if ((int8_t)on == lastState) return;
  lastState = on;
  int y = tft.height() - UI_STATUSBAR_H + 1;
  // Leave space for the main menu gear icon. The fill rectangle starts after this clearance.
  const int GEAR_CLEARANCE = 38;
  int w = tft.width() - UI_RIGHTZONE_W - GEAR_CLEARANCE;
  tft.fillRect(GEAR_CLEARANCE, y, w, UI_STATUSBAR_H - 1, ILI9341_BLACK);
  if (on) {
    tft.setTextWrap(false);
    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(1);
    // Centered within that same gear-clear zone.
    int16_t bx, by; uint16_t bw, bh;
    tft.getTextBounds("DEMO MODE", 0, 0, &bx, &by, &bw, &bh);
    tft.setCursor(GEAR_CLEARANCE + (w - (int)bw) / 2 - bx, y + 3);
    tft.print("DEMO MODE");
  }
}

void uiServiceChrome() {
  uiDrawClock();
  uiDrawBatteryIndicator();
  uiTickToast();
  uiTickDemoBadge();
}

// --- numeric keypad ------------------------------------------------------
// Implement a dedicated numeric keypad. The grid uses large tap targets for IP addresses. Each key fires once per tap. Cancellation returns the initial value.
String uiNumpadInput(const char *prompt, const String &initial) {
  // Define 14 keypad cells. Control codes reuse label slots.
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
  tft.setTextColor(accentLabel());
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.print(prompt);
  for (int i = 0; i < 14; i++) uiDrawButton(k[i].b);

  auto redrawField = [&]() {
    uiClearRect(2, 16, tft.width() - 4, 26);
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
