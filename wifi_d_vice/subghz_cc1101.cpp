// CC1101 SubGHz screen. Three passive modes, cycled with the "mode" button:
//
//   Sweep     -- RSSI vs frequency across one synthesiser band, one column
//                per screen pixel. Amplitude-vs-frequency with ISM-centre
//                markers, a fading peak-hold, and a carrier / burst /
//                broadband classifier. (The original behaviour.)
//   Analyzer  -- Flipper-style frequency analyzer: dwell on each discrete
//                ISM centre in turn, hold a per-centre RSSI EWMA + peak,
//                and call the strongest. Band-independent -- it watches
//                every centre at once. No extra wiring.
//   Raw       -- OOK/ASK pulse capture on one frequency (default 433.92).
//                Records the demodulated bit-stream edge timing off the
//                CC1101 GDO0 pin, draws the pulse train, and writes a
//                Flipper-compatible .sub to the SD card. REQUIRES the GDO0
//                line wired to a spare GPIO and set in System > Hardware;
//                the base CYD build only breaks out CS, so this mode is
//                unavailable until that wire exists. A live probe at entry
//                (toggle IOCFG0 constant-0 / constant-1 and read the GPIO
//                back) decides whether it is really connected.
//
// Everything here is receive-only: no strobe ever puts the chip in TX.
//
// CC1101 shares the LCD's global-SPI bus (pins.h: TFT_SCK/MISO/MOSI) with
// its own CS -- safe because both drivers configure the same clk/miso/mosi.
#include <cc1101.h>
#include <SD.h>
#include <math.h>
#include "ui.h"
#include "pins.h"
#include "pincfg.h"
#include "sd_bus.h"
#include "theme.h"

// Constructed on first entry so it picks up any runtime CS / GDO0 override
// (System > SPI / IRQ pins) rather than the compile-time default.
static CC1101::Radio *radio = nullptr;
static bool ok = false;
static int  band = 0;
static const int STEPS = 240;

static int8_t  rssiRow[STEPS];
static int8_t  peakRow[STEPS];
static uint8_t consecHot[STEPS];
static bool    isCenter[STEPS];
static int     stepIdx = 0;

enum Mode { MODE_SWEEP = 0, MODE_ANALYZER, MODE_RAW, MODE_N };
static Mode mode = MODE_SWEEP;
static const char *MODE_NAME[MODE_N] = { "Sweep", "Analyzer", "Raw" };

static Btn modeBtn, actBtn;   // actBtn = "band" (sweep/analyzer) or "capture" (raw)
static const int LABEL_Y   = UI_CONTENT_Y + 2;
static const int STAT_Y    = UI_CONTENT_Y + 12;
static const int GRAPH_TOP = UI_CONTENT_Y + 24;
static const int GRAPH_H   = 142;
static const uint16_t GRID_DIM = 0x2104;   // faint centre gridline
static const uint16_t PEAK_DIM = 0x630C;   // peak-hold trace

// The CC1101 synthesiser covers three disjoint ranges -- one preset each.
struct Band { double lo, hi; const char *label; };
static const Band kBands[] = {
  {300.0, 348.0, "300-348 MHz  (315 fobs, TPMS)"},
  {387.0, 464.0, "387-464 MHz  (433.92 ISM, alarms)"},
  {779.0, 928.0, "779-928 MHz  (868 EU, 915 US ISM)"},
};
static const int NBANDS = sizeof(kBands) / sizeof(kBands[0]);

struct Center { double mhz; const char *lbl; };
static const Center kCenters[] = {
  {315.00, "315"}, {318.00, "318"}, {330.00, "330"}, {345.00, "345"},
  {390.00, "390"}, {418.00, "418"}, {433.92, "434"}, {434.42, "434b"},
  {868.35, "868"}, {915.00, "915"},
};
static const int NCENTERS = sizeof(kCenters) / sizeof(kCenters[0]);

// ---- analyzer state --------------------------------------------------
static int8_t   cRssi[NCENTERS];   // EWMA per centre
static int8_t   cPeak[NCENTERS];
static int      anIdx = 0;
static uint32_t anLastDraw = 0;

// ---- raw-capture state ---------------------------------------------
static int      gdoPin   = -1;
static bool     gdoWired  = false;
static const int RAW_MAX = 600;
static uint16_t rawDur[RAW_MAX];   // pulse duration, microseconds (capped 65535)
static bool     rawLvl[RAW_MAX];   // level held during that pulse (true = high)
static int      rawN = 0;
static bool     rawHave = false;
static double   rawFreqMhz = 433.92;
static char     rawSavedPath[40];

static double freqForStep(int i) {
  const Band &b = kBands[band];
  return b.lo + (b.hi - b.lo) * i / (STEPS - 1);
}
static int stepForFreq(double mhz) {
  const Band &b = kBands[band];
  if (mhz < b.lo || mhz > b.hi) return -1;
  return (int)((mhz - b.lo) / (b.hi - b.lo) * (STEPS - 1) + 0.5);
}

// ---- GDO0 presence probe -----------------------------------------
//
// IOCFG0 (register 0x02): bit 6 = GDO0_INV, bits 5:0 = GDO0_CFG. 0x2F is
// "hardwired to 0"; OR in 0x40 (invert) for a hardwired 1. If the pin
// really is wired to gdoPin the GPIO reads track the register; a floating
// or unconnected pin will not toggle cleanly.
static bool probeGdo0() {
  gdoPin = pincfgGet(PIN_CC1101_GDO0);
  if (gdoPin < 0 || !radio || !ok) return false;
  pinMode(gdoPin, INPUT);
  radio->writeReg(CC1101_REG_IOCFG0, 0x2F);
  delayMicroseconds(400);
  int lo = digitalRead(gdoPin);
  radio->writeReg(CC1101_REG_IOCFG0, (uint8_t)(0x2F | 0x40));
  delayMicroseconds(400);
  int hi = digitalRead(gdoPin);
  radio->writeReg(CC1101_REG_IOCFG0, 0x2F);
  return lo == 0 && hi == 1;
}

// ---- sweep ---------------------------------------------------------

static void drawColumn(int i) {
  tft.drawFastVLine(i, GRAPH_TOP, GRAPH_H, isCenter[i] ? GRID_DIM : ILI9341_BLACK);
  int pk = peakRow[i];
  if (pk > -120) {
    int ph = map(constrain(pk, -100, -30), -100, -30, 0, GRAPH_H);
    if (ph > 0) tft.drawPixel(i, GRAPH_TOP + GRAPH_H - ph, PEAK_DIM);
  }
  int rssi = rssiRow[i];
  int barH = map(constrain(rssi, -100, -30), -100, -30, 0, GRAPH_H);
  uint16_t col = rssi > -55 ? ILI9341_RED : (rssi > -75 ? ILI9341_YELLOW : ILI9341_GREEN);
  if (barH > 0) tft.drawFastVLine(i, GRAPH_TOP + GRAPH_H - barH, barH, col);
}

static void drawSweepChrome() {
  uiClearRect(0, LABEL_Y - 2, tft.width(), GRAPH_TOP - LABEL_Y + 2);
  tft.setTextSize(1);
  tft.setTextColor(ok ? ILI9341_WHITE : ILI9341_RED);
  tft.setCursor(4, LABEL_Y);
  tft.print(ok ? kBands[band].label : "CC1101 not responding");
  tft.setTextColor(thLabel());
  int prevEnd = -100;
  for (int c = 0; c < NCENTERS; c++) {
    int s = stepForFreq(kCenters[c].mhz);
    if (s < 0) continue;
    int x = s - 8; if (x < 0) x = 0; if (x > tft.width() - 20) x = tft.width() - 20;
    if (x < prevEnd) x = prevEnd;
    tft.setCursor(x, STAT_Y);
    tft.print(kCenters[c].lbl);
    prevEnd = x + 20;
  }
}

static void newSweep() {
  for (int i = 0; i < STEPS; i++) { rssiRow[i] = -128; peakRow[i] = -128; consecHot[i] = 0; isCenter[i] = false; }
  for (int c = 0; c < NCENTERS; c++) {
    int s = stepForFreq(kCenters[c].mhz);
    if (s >= 0 && s < STEPS) isCenter[s] = true;
  }
  stepIdx = 0;
  tft.fillRect(0, GRAPH_TOP, tft.width(), GRAPH_H, ILI9341_BLACK);
}

// After each full pass: fold peaks, classify carrier / burst / broadband.
static void endOfSweep() {
  int floorR = 0;
  for (int i = 0; i < STEPS; i++) if (rssiRow[i] > -120 && rssiRow[i] < floorR) floorR = rssiRow[i];
  if (floorR == 0) floorR = -100;
  int hot = 0, strong = -128, strongI = 0, maxConsec = 0;
  for (int i = 0; i < STEPS; i++) {
    bool h = rssiRow[i] > floorR + 16;
    consecHot[i] = h ? (consecHot[i] < 250 ? consecHot[i] + 1 : 250) : 0;
    if (h) hot++;
    if (consecHot[i] > maxConsec) maxConsec = consecHot[i];
    if (rssiRow[i] > strong) { strong = rssiRow[i]; strongI = i; }
    if (peakRow[i] > -120) peakRow[i]--;
  }

  char msg[48];
  if (maxConsec >= 8 && strong > -90)
    snprintf(msg, sizeof(msg), "CARRIER @ %.2f  %ddBm", freqForStep(strongI), strong);
  else if (hot > STEPS / 3)
    snprintf(msg, sizeof(msg), "broadband noise (jammer?)  floor %d", floorR);
  else if (hot > 0)
    snprintf(msg, sizeof(msg), "burst activity  peak %.2f  %ddBm", freqForStep(strongI), strong);
  else
    snprintf(msg, sizeof(msg), "quiet  floor %ddBm", floorR);

  tft.fillRect(0, GRAPH_TOP + GRAPH_H + 2, tft.width(), 12, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(maxConsec >= 8 ? ILI9341_RED : (hot ? ILI9341_YELLOW : ILI9341_GREEN));
  tft.setCursor(4, GRAPH_TOP + GRAPH_H + 3);
  tft.print(msg);
}

static void sweepLoop() {
  radio->setFrequency(freqForStep(stepIdx));
  radio->setState(CC1101::STATE_RX);
  delay(3);   // synth settle + AGC
  int8_t r = radio->getRSSI();
  rssiRow[stepIdx] = r;
  if (r > peakRow[stepIdx]) peakRow[stepIdx] = r;
  drawColumn(stepIdx);
  stepIdx++;
  if (stepIdx >= STEPS) { stepIdx = 0; endOfSweep(); }
}

// ---- analyzer ----------------------------------------------------

static void newAnalyzer() {
  for (int c = 0; c < NCENTERS; c++) { cRssi[c] = -120; cPeak[c] = -120; }
  anIdx = 0;
  tft.fillRect(0, LABEL_Y - 2, tft.width(), tft.height() - (LABEL_Y - 2) - UI_STATUSBAR_H, ILI9341_BLACK);
}

static void drawAnalyzer() {
  int strongC = 0;
  for (int c = 1; c < NCENTERS; c++) if (cRssi[c] > cRssi[strongC]) strongC = c;

  tft.setTextSize(1);
  tft.fillRect(0, LABEL_Y - 2, tft.width(), 12, ILI9341_BLACK);
  tft.setCursor(4, LABEL_Y);
  if (cRssi[strongC] > -110) {
    tft.setTextColor(cRssi[strongC] > -70 ? ILI9341_RED : ILI9341_YELLOW);
    tft.printf("strongest: %.2f MHz  %ddBm", kCenters[strongC].mhz, cRssi[strongC]);
  } else {
    tft.setTextColor(ILI9341_GREEN);
    tft.print("all ISM centres quiet");
  }

  int rowH = (GRAPH_H) / NCENTERS;
  int x0 = 60, barMax = tft.width() - x0 - 40;
  for (int c = 0; c < NCENTERS; c++) {
    int y = GRAPH_TOP + c * rowH;
    tft.fillRect(0, y, tft.width(), rowH - 1, ILI9341_BLACK);
    tft.setTextColor(c == strongC ? ILI9341_RED : thLabel());
    tft.setCursor(2, y + (rowH - 8) / 2);
    tft.printf("%.2f", kCenters[c].mhz);
    int v  = constrain(cRssi[c], -110, -30);
    int pv = constrain(cPeak[c], -110, -30);
    int w  = map(v,  -110, -30, 0, barMax);
    int pw = map(pv, -110, -30, 0, barMax);
    uint16_t col = cRssi[c] > -55 ? ILI9341_RED : (cRssi[c] > -75 ? ILI9341_YELLOW : ILI9341_GREEN);
    if (w  > 0) tft.fillRect(x0, y + 2, w, rowH - 5, col);
    if (pw > w) tft.drawFastVLine(x0 + pw, y + 2, rowH - 5, PEAK_DIM);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(x0 + barMax + 4, y + (rowH - 8) / 2);
    if (cRssi[c] > -115) tft.printf("%d", cRssi[c]);
  }
}

static void analyzerLoop() {
  radio->setFrequency(kCenters[anIdx].mhz);
  radio->setState(CC1101::STATE_RX);
  delay(4);
  int8_t r = radio->getRSSI();
  cRssi[anIdx] = (int8_t)((cRssi[anIdx] * 3 + r) / 4);
  if (r > cPeak[anIdx]) cPeak[anIdx] = r;
  anIdx = (anIdx + 1) % NCENTERS;
  if (anIdx == 0 && millis() - anLastDraw > 250) { drawAnalyzer(); anLastDraw = millis(); }
}

// ---- raw capture -----------------------------------------------

static void drawRawChrome() {
  uiClearRect(0, LABEL_Y - 2, tft.width(), tft.height() - (LABEL_Y - 2) - UI_STATUSBAR_H);
  tft.setTextSize(1);
  tft.setCursor(4, LABEL_Y);
  if (!gdoWired) {
    tft.setTextColor(ILI9341_RED);
    tft.print("RAW needs the CC1101 GDO0 line wired");
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(4, LABEL_Y + 12);
    if (pincfgGet(PIN_CC1101_GDO0) < 0)
      tft.print("set the GPIO in System > Hardware");
    else
      tft.printf("GPIO%d set but no signal toggling", pincfgGet(PIN_CC1101_GDO0));
    return;
  }
  tft.setTextColor(ILI9341_WHITE);
  tft.printf("OOK @ %.2f MHz   GDO0=GPIO%d", rawFreqMhz, gdoPin);
  tft.setTextColor(thLabel());
  tft.setCursor(4, LABEL_Y + 12);
  tft.print(rawHave ? "captured -- tap graph to save .sub" : "press capture and send a signal");
}

static void drawRawTrace() {
  tft.fillRect(0, GRAPH_TOP, tft.width(), GRAPH_H, ILI9341_BLACK);
  if (!rawHave || rawN < 2) return;

  uint32_t total = 0;
  for (int i = 0; i < rawN; i++) total += rawDur[i];
  if (total == 0) return;
  double xs = (double)(tft.width() - 2) / (double)total;

  int yHi = GRAPH_TOP + 20, yLo = GRAPH_TOP + GRAPH_H - 20;
  double x = 1;
  int prevY = rawLvl[0] ? yHi : yLo;
  tft.drawFastVLine((int)x, GRAPH_TOP, GRAPH_H, ILI9341_BLACK);
  for (int i = 0; i < rawN; i++) {
    int y = rawLvl[i] ? yHi : yLo;
    int x1 = (int)x, x2 = (int)(x + rawDur[i] * xs);
    if (x2 > tft.width() - 1) x2 = tft.width() - 1;
    tft.drawFastVLine(x1, min(prevY, y), abs(prevY - y) + 1, ILI9341_GREENYELLOW);   // edge
    tft.drawFastHLine(x1, y, x2 - x1 + 1, ILI9341_GREENYELLOW);                      // level
    prevY = y;
    x += rawDur[i] * xs;
    if (x >= tft.width() - 1) break;
  }

  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.fillRect(0, GRAPH_TOP + GRAPH_H + 2, tft.width(), 12, ILI9341_BLACK);
  tft.setCursor(4, GRAPH_TOP + GRAPH_H + 3);
  tft.printf("%d edges  %lu us span", rawN, (unsigned long)total);
}

// Tight-poll GDO0 in OOK async-serial mode for a bounded window. ~300 ms
// cap keeps this well under the 5 s task watchdog; there are no allocations
// or blocking calls in the loop, so no interior yield() is needed.
static void captureRaw() {
  if (!gdoWired || !radio) return;
  rawN = 0; rawHave = false;

  radio->setModulation(CC1101::MOD_ASK_OOK);
  radio->setFrequency(rawFreqMhz);
  radio->setRxBandwidth(200.0);
  radio->setPacketFormat(CC1101::PKT_FORMAT_ASYNC_SERIAL);
  radio->writeReg(CC1101_REG_IOCFG0, 0x0D);   // GDO0 = async serial data out
  radio->setState(CC1101::STATE_RX);
  delay(3);

  uint32_t tStart = micros();
  uint32_t tEdge  = tStart;
  int last = digitalRead(gdoPin);
  while ((int32_t)(micros() - tStart) < 300000 && rawN < RAW_MAX) {
    int cur = digitalRead(gdoPin);
    if (cur != last) {
      uint32_t now = micros();
      uint32_t d = now - tEdge;
      rawDur[rawN] = d > 65535 ? 65535 : (uint16_t)d;
      rawLvl[rawN] = last;
      rawN++;
      last = cur;
      tEdge = now;
    }
  }

  radio->writeReg(CC1101_REG_IOCFG0, 0x2F);   // GDO0 quiet
  radio->setPacketFormat(CC1101::PKT_FORMAT_NORMAL);
  radio->setModulation(CC1101::MOD_2FSK);     // restore for sweep / analyzer
  radio->setRxBandwidth(200.0);
  radio->idle();
  yield();

  rawHave = rawN >= 8;                        // fewer edges than this = no real signal
}

// Flipper-compatible RAW .sub: positive = high (mark) us, negative = low
// (space) us. Written plaintext (RF capture, not an engagement log) to
// /subghz on the SD card.
static bool saveRawSub() {
  if (!rawHave) return false;
  sdBusBegin();
  if (!SD.begin(SD_CS, sdSPI)) return false;
  if (!SD.exists("/subghz")) SD.mkdir("/subghz");
  char path[40];
  int n = 0;
  do { snprintf(path, sizeof(path), "/subghz/raw_%03d.sub", n++); } while (SD.exists(path) && n < 1000);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.println("Filetype: Flipper SubGhz RAW File");
  f.println("Version: 1");
  f.printf("Frequency: %lu\n", (unsigned long)(rawFreqMhz * 1e6));
  f.println("Preset: FuriHalSubGhzPresetOok650Async");
  f.println("Protocol: RAW");
  f.print("RAW_Data: ");
  for (int i = 0; i < rawN; i++) {
    long v = rawLvl[i] ? (long)rawDur[i] : -(long)rawDur[i];
    f.printf("%ld ", v);
    if ((i & 63) == 63) { f.print("\nRAW_Data: "); }
  }
  f.println();
  f.close();
  strncpy(rawSavedPath, path, sizeof rawSavedPath);
  rawSavedPath[sizeof rawSavedPath - 1] = 0;
  return true;
}

// ---- screen lifecycle -----------------------------------------

static void drawActionRow() {
  Btn r[2] = {{0, 0, 0, 0, "mode"},
              {0, 0, 0, 0, mode == MODE_RAW ? "capture" : "band"}};
  uiDrawActionRow(r, 2);
  modeBtn = r[0];
  actBtn  = r[1];
}

static void enterMode() {
  drawActionRow();
  if (mode == MODE_SWEEP) {
    if (ok) { radio->setModulation(CC1101::MOD_2FSK); radio->setRxBandwidth(200.0); }
    drawSweepChrome();
    newSweep();
  } else if (mode == MODE_ANALYZER) {
    if (ok) { radio->setModulation(CC1101::MOD_2FSK); radio->setRxBandwidth(200.0); }
    uiClearRect(0, STAT_Y, tft.width(), 12);
    newAnalyzer();
    anLastDraw = 0;
    drawAnalyzer();
  } else {  // MODE_RAW
    drawRawChrome();
    drawRawTrace();
  }
}

void subghzEnter() {
  uiDrawTopBar("SubGHz");
  if (!ok) {
    uiShowLoading("Initializing CC1101...");
    if (!radio)
      radio = new CC1101::Radio(pincfgGet(PIN_CC1101_CS), TFT_SCK, TFT_MISO, TFT_MOSI,
                                pincfgGet(PIN_CC1101_GDO0) < 0 ? PIN_UNUSED
                                                              : pincfgGet(PIN_CC1101_GDO0));
    CC1101::Status st = radio->begin(CC1101::MOD_2FSK, kBands[band].lo, 38.4);
    ok = (st == CC1101::STATUS_OK);
    if (ok) radio->setRxBandwidth(200.0);
  }
  gdoWired = probeGdo0();
  enterMode();
}

void subghzLoop() {
  if (!ok || !radio) return;
  if      (mode == MODE_SWEEP)    sweepLoop();
  else if (mode == MODE_ANALYZER) analyzerLoop();
  // RAW does nothing per-loop -- capture is an explicit button press.
}

void subghzTouch(const TouchPoint &t) {
  if (uiTouchInButton(t, modeBtn)) {
    mode = (Mode)((mode + 1) % MODE_N);
    enterMode();
    uiWaitForRelease();
    return;
  }
  if (uiTouchInButton(t, actBtn)) {
    if (mode == MODE_RAW) {
      if (gdoWired) {
        uiToast("capturing...");
        captureRaw();
        drawRawChrome();
        drawRawTrace();
        uiToast(rawHave ? "got it -- tap graph to save" : "nothing captured");
      } else {
        gdoWired = probeGdo0();   // re-probe in case it was just wired
        drawRawChrome();
      }
    } else {
      band = (band + 1) % NBANDS;
      if (mode == MODE_SWEEP) { drawSweepChrome(); newSweep(); }
    }
    uiWaitForRelease();
    return;
  }
  // Tap the graph area in RAW mode with a capture held -> save .sub
  if (mode == MODE_RAW && rawHave && t.y >= GRAPH_TOP && t.y < GRAPH_TOP + GRAPH_H) {
    if (saveRawSub()) uiToast(rawSavedPath);
    else              uiToast("SD save failed");
    uiWaitForRelease();
  }
}

void subghzExit() {
  if (ok && radio) {
    radio->writeReg(CC1101_REG_IOCFG0, 0x2F);
    radio->idle();
  }
}
