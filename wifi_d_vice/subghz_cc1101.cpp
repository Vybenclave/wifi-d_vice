// CC1101-based SubGHz sweep: reads the chip's real RSSI register across a
// band, one frequency per screen column. Amplitude-vs-frequency, not a
// binary carrier-detect toy. It cannot demodulate anything (FSK/OOK radio;
// no LoRa) -- it just shows where RF energy is, with ISM-centre markers, a
// fading peak-hold, and a crude carrier / burst / broadband classifier.
//
// CC1101 shares the LCD's global-SPI bus (pins.h: TFT_SCK/MISO/MOSI) with
// its own CS -- safe because both drivers configure the same clk/miso/mosi.
#include <cc1101.h>
#include <math.h>
#include "ui.h"
#include "pins.h"
#include "pincfg.h"

// Constructed on first entry so it picks up any runtime CS override
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

static Btn bandBtn;
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
  {315.00, "315"}, {319.50, "319"}, {345.00, "345"}, {390.00, "390"},
  {433.92, "434"}, {868.35, "868"}, {908.42, "908"}, {915.00, "915"}, {925.00, "925"},
};
static const int NCENTERS = sizeof(kCenters) / sizeof(kCenters[0]);

static double freqForStep(int i) {
  const Band &b = kBands[band];
  return b.lo + (b.hi - b.lo) * i / (STEPS - 1);
}
static int stepForFreq(double mhz) {
  const Band &b = kBands[band];
  if (mhz < b.lo || mhz > b.hi) return -1;
  return (int)((mhz - b.lo) / (b.hi - b.lo) * (STEPS - 1) + 0.5);
}

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

static void drawChrome() {
  uiClearRect(0, LABEL_Y - 2, tft.width(), GRAPH_TOP - LABEL_Y + 2);
  tft.setTextSize(1);
  tft.setTextColor(ok ? ILI9341_WHITE : ILI9341_RED);
  tft.setCursor(4, LABEL_Y);
  tft.print(ok ? kBands[band].label : "CC1101 not responding");
  // centre-marker labels, roughly above their gridline column
  tft.setTextColor(ILI9341_CYAN);
  int prevEnd = -100;
  for (int c = 0; c < NCENTERS; c++) {
    int s = stepForFreq(kCenters[c].mhz);
    if (s < 0) continue;
    int x = s - 8; if (x < 0) x = 0; if (x > tft.width() - 20) x = tft.width() - 20;
    if (x < prevEnd) x = prevEnd;                 // don't overlap the previous label
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

void subghzEnter() {
  uiDrawTopBar("SubGHz Sweep");
  Btn row[1] = {{0, 0, 0, 0, "next band"}};
  uiDrawActionRow(row, 1);
  bandBtn = row[0];
  if (!ok) {
    uiShowLoading("Initializing CC1101...");
    if (!radio) radio = new CC1101::Radio(pincfgGet(PIN_CC1101_CS), TFT_SCK, TFT_MISO, TFT_MOSI);
    CC1101::Status st = radio->begin(CC1101::MOD_2FSK, kBands[band].lo, 38.4);
    ok = (st == CC1101::STATUS_OK);
    if (ok) radio->setRxBandwidth(200.0);
  }
  drawChrome();
  newSweep();
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
    if (peakRow[i] > -120) peakRow[i]--;    // fade the peak-hold
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

void subghzLoop() {
  if (!ok || !radio) return;
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

void subghzTouch(const TouchPoint &t) {
  if (uiTouchInButton(t, bandBtn)) {
    band = (band + 1) % NBANDS;
    drawChrome();
    newSweep();
    uiWaitForRelease();
  }
}

void subghzExit() { if (ok && radio) radio->idle(); }
