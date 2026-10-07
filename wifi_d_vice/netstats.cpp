#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>   // Persist server IP, port, and direction for LAN tests.
#include <esp_random.h>
#include <esp_wifi.h>       // Calls esp_wifi_set_ps
#include <math.h>
#include <string.h>
#include <Adafruit_GFX.h>   // Uses GFXcanvas16 for the off-screen buffer.
#include "ui.h"
#include "screens.h"
#include "accent.h"
#include "power.h"

// The menu shows three options.
// Speed test measures throughput with parallel TCP streams.
// One ESP32 stream caps at ~5 Mbps due to the lwIP window.
// Parallel streams sum the total throughput.
// LAN speed uses raw TCP to a local PC listener.
// This removes internet RTT limits.
// It measures the raw WiFi link capacity.
// Connection and NAT cards show public IP and network info.
// They run a STUN probe to detect NAT mapping.
// This feature acts as a client.
// It uses the joined WiFi network.
// It never attacks other hosts.
// Plain HTTP runs blocking modal phases.
// The chip tops out at 20-40 Mbps total.
// Swap host constants if unreachable.

// ----------------------------- config -----------------------------
// Plain HTTP avoids HTTPS redirects.
// The chip cannot follow 301 redirects.
// These hosts run single datacenters.
// Per-stream throughput stays window-bound.
// The chip lacks RAM for secure streams.
static const char *DL_HOST = "ipv4.download.thinkbroadband.com";
static const char *DL_PATH = "/50MB.zip";
static const char *UL_HOST = "speedtest.tele2.net";               // accepts a POST body
static const char *UL_PATH = "/upload.php";

static const int      N_DL         = 6;      // parallel download streams (was 5)
static const int      N_UL         = 3;      // parallel upload streams -- N_DL+N_UL
                                             // must stay under LwIP's 10-socket cap
// LAN tests use raw TCP streams.
// Four streams suffice for local networks.
// Eight total streams stay under the lwIP socket limit.
static const int      N_LAN        = 4;

static const uint16_t CONNECT_MS   = 6000;
static const uint32_t TEST_MS      = 30000;  // one fixed run, down + up together
static const uint32_t SAMPLE_MS    = 250;
static const int      NBARS        = TEST_MS / SAMPLE_MS;   // 120 evenly-spread slots
static const uint32_t STABLE_AFTER = 2000;   // ignore the ramp-up in the average

// browser-netstats "Bandwidth" palette, converted to RGB565
static const uint16_t NS_DOWN  = 0x7E7F;   // #7dcfff
static const uint16_t NS_UP    = 0xFB5A;   // #ff6ad5
static const uint16_t NS_GRID  = 0x4208;   // #404040
static const uint16_t NS_LABEL = 0xB5DC;   // #b6bfe0

// ----------------------------- state -----------------------------
enum Page { PAGE_MENU, PAGE_SPEED, PAGE_CONN, PAGE_NONET, PAGE_LAN };
static Page page = PAGE_MENU;

// The LAN page shows a config form or a running graph.
// Back from the graph returns to the form.
// Back from the form returns to the main menu.
enum LanView { LV_CFG, LV_RUN };
static LanView lanView = LV_CFG;

// The code saves LAN settings to NVS.
// The default IP points to a common desktop.
// Users change it with the on-screen numpad.
static String   lanIP   = "192.168.1.157";
static uint16_t lanPort = 5001;
static uint8_t  lanDir  = 0;   // 0 = download, 1 = upload, 2 = both

enum StState { ST_IDLE, ST_DONE, ST_ERR };
static StState st = ST_IDLE;

static const int SAMP_MAX = NBARS;
static float dvals[SAMP_MAX];
static float uvals[SAMP_MAX];
static int   nSamp = 0;              // how many bar slots filled so far

static uint32_t lastSample = 0;
static uint32_t dnTotal = 0, upTotal = 0, dnLast = 0, upLast = 0;
static double   sumStableD = 0, sumStableU = 0;
static int      cntStable = 0;
static float    avgDown = 0, avgUp = 0, peakDown = 0, peakUp = 0;
static bool     s_cancel = false;
static const char *errMsg = "";

static uint8_t zbuf[1460];              // zero-filled upload payload
static uint8_t rxbuf[2048];             // shared drain buffer (HTTP + LAN download)
// The top bar back button handles navigation.
// Sub-pages do not show a menu button.
static Btn actBtn, refreshBtn, miSpeed, miLan, miConn;
// LAN config-form hit targets (laid out in drawLanConfig()).
static Btn lanIpBtn, lanPortBtn, lanDnBtn, lanUpBtn, lanBothBtn, lanStartBtn;

struct ConnInfo {
  String pubIP, loc, colo, proto, asn, behindNat, natType;
  bool udpOk = false;
  bool have = false;
};
static ConnInfo ci;

// --------------------------- graph geom --------------------------
static const int gPadL = 42;   // left gutter for axis labels (canvas-local)
static const int gY    = 90;   // graph top on-screen (below the 2-line header)
static const int gH    = 104;  // graph height == canvas height

static inline float mbps(uint32_t bytes, uint32_t ms) {
  return ms ? (float)bytes * 0.008f / (float)ms : 0.0f;
}

// The log axis starts at 0.1 Mbps.
// The top scale ratchets up on loud samples.
// It shrinks only after a full decade drop.
// The graph redraws into an off-screen canvas.
// This prevents flicker during rescaling.
static const float GBW_LO = 0.1f;
static float gScaleTop = 1.0f;                  // reset in resetTraces()
static GFXcanvas16 *gcanv = nullptr;            // tft.width() x gH graph-body buffer

static float niceLog(float v) {                 // round up to next 1/2/5 x10^k, min 1
  if (v <= 1) return 1;
  float dec = powf(10.0f, floorf(log10f(v)));
  const float m[] = {1, 2, 5, 10};
  for (int i = 0; i < 4; i++) if (dec * m[i] >= v) return dec * m[i];
  return dec * 10;
}

// value -> 0..1 up the half-axis, log-spaced between GBW_LO and gScaleTop
static float gBwFrac(float v) {
  if (v <= GBW_LO) return 0.0f;
  float lo = log10f(GBW_LO);
  float f = (log10f(v) - lo) / (log10f(gScaleTop) - lo);
  return f < 0 ? 0 : (f > 1 ? 1 : f);
}

static void graphAlloc() { if (!gcanv) gcanv = new GFXcanvas16(tft.width(), gH); }
static void graphFree()  { delete gcanv; gcanv = nullptr; }

static void resetTraces() {
  nSamp = 0;
  for (int i = 0; i < SAMP_MAX; i++) { dvals[i] = 0; uvals[i] = 0; }
  avgDown = avgUp = peakDown = peakUp = 0;
  gScaleTop = 1.0f;
}

// ---------------------------- speed test -------------------------
static void drawChrome(const char *btnLabel) {
  uiClearRect(0, UI_ACTIONROW_Y, tft.width(), UI_ACTIONROW_H);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setCursor(4, UI_ACTIONROW_Y + 3);
    tft.printf("%.20s", WiFi.SSID().c_str());
    tft.setCursor(4, UI_ACTIONROW_Y + 14);
    tft.print(WiFi.localIP());
  }
  actBtn = {tft.width() - 90, UI_ACTIONROW_Y, 88, UI_ACTIONROW_H, btnLabel};
  if (!strcmp(btnLabel, "Stop")) uiDrawButtonColored(actBtn, ILI9341_MAGENTA);   // running -> stands out regardless of theme accent
  else                           uiDrawMenuButton(actBtn);
}

static void drawLegend() {
  int lx = tft.width() - 78;
  tft.fillRect(lx, UI_CONTENT_Y + 1, 10, 8, NS_DOWN);
  tft.setTextSize(1);
  tft.setTextColor(NS_LABEL);
  tft.setCursor(lx + 13, UI_CONTENT_Y + 2); tft.print("dn");
  tft.fillRect(lx + 36, UI_CONTENT_Y + 1, 10, 8, NS_UP);
  tft.setCursor(lx + 49, UI_CONTENT_Y + 2); tft.print("up");
}

// This function clears only the caption strip.
// It draws the caption text left of the legend.
static void drawCaption(const char *cap) {
  uiClearRect(0, UI_CONTENT_Y, tft.width() - 84, gY - UI_CONTENT_Y);
  tft.setTextSize(1);
  tft.setTextColor(accentLabel());
  tft.setCursor(4, UI_CONTENT_Y + 2);
  tft.print(cap);
}

// The function fits the dynamic top to the loudest sample.
// It ratchets up immediately.
// It shrinks only after a full decade drop.
static void rescale() {
  float pk = 0;
  for (int i = 0; i < nSamp; i++) {
    if (dvals[i] > pk) pk = dvals[i];
    if (uvals[i] > pk) pk = uvals[i];
  }
  float want = niceLog(pk);
  if (want > gScaleTop || want <= gScaleTop * 0.5f) gScaleTop = want;
  if (gScaleTop < 1) gScaleTop = 1;
}

// This function renders the grid and bars into the off-screen canvas.
// It blits the canvas in one pass.
// This overwrites pixels without clearing.
// A scale change causes no flicker.
static void renderGraph() {
  if (!gcanv || !gcanv->getBuffer()) return;
  GFXcanvas16 &c = *gcanv;
  const int W = c.width();
  const int rightX = W - 6;
  const int half = gH / 2;
  const int mid  = gH / 2;          // canvas-local mid-line
  c.fillScreen(ILI9341_BLACK);
  c.setTextSize(1);

  const float gl[] = {0.1f, 0.3f, 1, 3, 10, 30, 100, 300};
  for (int i = 0; i < 8; i++) {
    if (gl[i] < GBW_LO || gl[i] > gScaleTop) continue;
    int dh = (int)(half * gBwFrac(gl[i]));
    char lb[8];
    if (gl[i] < 1) snprintf(lb, sizeof(lb), "%.1f", gl[i]);
    else           snprintf(lb, sizeof(lb), "%d", (int)gl[i]);
    for (int s = -1; s <= 1; s += 2) {
      int y = mid - s * dh;
      c.drawFastHLine(gPadL, y, rightX - gPadL, dh == 0 ? NS_LABEL : NS_GRID);
      c.setTextColor(NS_LABEL);
      c.setCursor(2, y - 3);
      c.print(lb);
      if (dh == 0) break;
    }
  }

  float slot = (float)(rightX - gPadL) / NBARS;
  int bw = (int)slot; if (bw < 1) bw = 1; if (bw > 4) bw = 4;
  for (int i = 0; i < nSamp; i++) {
    int x = gPadL + (int)(i * slot);
    if (dvals[i] > 0) {
      int h = (int)(half * gBwFrac(dvals[i])); if (h < 1) h = 1;
      c.fillRect(x, mid + 1, bw, h, NS_DOWN);
    }
    if (uvals[i] > 0) {
      int h = (int)(half * gBwFrac(uvals[i])); if (h < 1) h = 1;
      c.fillRect(x, mid - h, bw, h, NS_UP);
    }
  }
  tft.drawRGBBitmap(0, gY, c.getBuffer(), W, gH);
}

// This function repaints the caption, legend, and graph.
// The live run uses faster partial updates.
static void drawGraph(const char *cap) {
  uiClearRect(0, UI_CONTENT_Y, tft.width(), gY - UI_CONTENT_Y);
  drawCaption(cap);
  drawLegend();
  graphAlloc();
  rescale();
  renderGraph();
}

// These variables track drawn text.
// The enterSpeed function clears them on fresh visits.
// This prevents stale text from previous screens.
static char prevDown[48] = "", prevUp[48] = "";
static bool summaryHintDrawn = false;

static void resetSummaryFields() {
  prevDown[0] = '\0';
  prevUp[0] = '\0';
  summaryHintDrawn = false;
}

static void drawSummary() {
  int y = gY + gH + 4;
  tft.setTextSize(1);
  uiDrawFieldIfChanged(4, y, tft.width() - 8, 12, NS_DOWN, 1, prevDown, sizeof(prevDown),
                        "Download %6.2f Mbps  peak %.1f", avgDown, peakDown);
  uiDrawFieldIfChanged(4, y + 12, tft.width() - 8, 12, NS_UP, 1, prevUp, sizeof(prevUp),
                        "Upload   %6.2f Mbps  peak %.1f", avgUp, peakUp);
  if (!summaryHintDrawn) {
    summaryHintDrawn = true;
    tft.setTextColor(NS_LABEL);
    tft.setCursor(4, y + 26);
    tft.print("max on this ESP32's WiFi is ~30 Mbps");
  }
}

// This function calculates Mbps from bytes since the last tick.
// It stores values in the bar grid.
// It rescales and redraws the graph.
// The canvas blit runs every other tick.
// This saves SPI transfer bandwidth.
static void takeSample(uint32_t elapsed) {
  uint32_t now = millis();
  uint32_t dt = now - lastSample;
  if (dt < SAMPLE_MS) return;
  float dm = mbps(dnTotal - dnLast, dt);
  float um = mbps(upTotal - upLast, dt);
  dnLast = dnTotal; upLast = upTotal; lastSample = now;

  int idx = elapsed / SAMPLE_MS;
  if (idx >= NBARS) idx = NBARS - 1;
  dvals[idx] = dm; uvals[idx] = um;
  if (dm > peakDown) peakDown = dm;
  if (um > peakUp)   peakUp   = um;
  if (idx + 1 > nSamp) nSamp = idx + 1;
  if (elapsed > STABLE_AFTER) { sumStableD += dm; sumStableU += um; cntStable++; }

  char cap[56];
  snprintf(cap, sizeof(cap), "%2lus  dn %.1f  up %.1f Mbps",
           (unsigned long)(elapsed / 1000), dm, um);
  drawCaption(cap);
  rescale();
  // Bars are logged every 250ms but the full-width canvas blit only every
  // other tick -- ~50KB over SPI 4x/s was stealing a real slice of the
  // transfer window. The last bar catches up on the next blit.
  if ((idx & 1) == 0) renderGraph();
}

// This function checks for a stop tap or back tap.
// It handles power management.
// It returns true if the user aborts.
static bool speedAborted() {
  TouchPoint t = uiReadTouch();
  if (t.pressed) powerNoteActivity();
  powerServiceAutoOff();
  if (!t.pressed) return false;
  if (uiTouchInButton(t, actBtn) || uiTouchInBackButton(t)) { uiWaitForRelease(); return true; }
  return false;
}

// This function runs parallel downloads and uploads.
// It logs both directions every sample tick.
// It aborts on stop or back taps.
static void runTest() {
  esp_wifi_set_ps(WIFI_PS_NONE);   // no modem sleep for the duration -- restored at the end
  WiFiClient dc[N_DL], uc[N_UL];
  int  hs[N_DL];          // per-download CRLFCRLF matcher state
  bool hdone[N_DL];
  int dAlive = 0, uAlive = 0;

  for (int i = 0; i < N_DL; i++) {
    hs[i] = 0; hdone[i] = false;
    if (dc[i].connect(DL_HOST, 80, CONNECT_MS)) {
      dc[i].setNoDelay(true);
      dc[i].printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: cyd-multitool\r\n"
                   "Accept: */*\r\nConnection: close\r\n\r\n", DL_PATH, DL_HOST);
      dAlive++;
    }
    delay(0);   // feed the WDT across the blocking connects
  }
  for (int i = 0; i < N_UL; i++) {
    delay(0);
    if (uc[i].connect(UL_HOST, 80, CONNECT_MS)) {
      uc[i].setNoDelay(true);
      uc[i].printf("POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: cyd-multitool\r\n"
                   "Content-Type: application/octet-stream\r\nContent-Length: %lu\r\n"
                   "Connection: close\r\n\r\n", UL_PATH, UL_HOST, 256UL * 1024 * 1024);
      uAlive++;
    }
  }
  if (dAlive == 0 && uAlive == 0) { errMsg = "host unreachable"; st = ST_ERR; return; }

  dnTotal = upTotal = dnLast = upLast = 0;
  sumStableD = sumStableU = 0; cntStable = 0;
  uint32_t start = millis();
  lastSample = start;
  bool gotDown = false;

  while (true) {
    uint32_t el = millis() - start;
    if (el > TEST_MS) break;
    bool activity = false;

    for (int i = 0; i < N_DL; i++) {
      if (!dc[i].available()) continue;
      activity = true;
      if (!hdone[i]) {
        while (dc[i].available() && !hdone[i]) {
          char ch = (char)dc[i].read();
          if (ch == '\r' && (hs[i] == 0 || hs[i] == 2)) hs[i]++;
          else if (ch == '\n' && (hs[i] == 1 || hs[i] == 3)) { if (++hs[i] == 4) hdone[i] = true; }
          else hs[i] = (ch == '\r') ? 1 : 0;
        }
        continue;
      }
      int n = dc[i].read(rxbuf, sizeof(rxbuf));
      if (n > 0) { dnTotal += n; gotDown = true; }
    }
    for (int i = 0; i < N_UL; i++) {
      if (!uc[i].connected()) continue;
      int w = uc[i].write(zbuf, sizeof(zbuf));
      if (w > 0) { upTotal += w; activity = true; }
    }

    // Always yield -- when the streams run flat out `activity` stays true
    // every iteration and without this the loop task never feeds the WDT
    // (~5s -> reboot). delay(1) is only for the truly-idle case.
    if (!activity) delay(1); else delay(0);
    if (millis() - lastSample >= SAMPLE_MS) {
      takeSample(el);
      if (speedAborted()) { s_cancel = true; break; }
    }
  }

  for (int i = 0; i < N_DL; i++) dc[i].stop();
  for (int i = 0; i < N_UL; i++) uc[i].stop();
  if (!gotDown && upTotal == 0) { errMsg = "no data transferred"; st = ST_ERR; return; }
  avgDown = cntStable ? (float)(sumStableD / cntStable) : peakDown;
  avgUp   = cntStable ? (float)(sumStableU / cntStable) : peakUp;
}

static void runSpeedTest() {
  resetTraces();
  s_cancel = false;
  st = ST_IDLE;
  ledBusy(true);
  drawChrome("Stop");
  drawGraph("connecting...");
  drawSummary();

  runTest();
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // restore the default after the test
  ledBusy(false);
  if (st == ST_ERR) {
    drawChrome("Start"); drawGraph(errMsg); drawSummary();
    uiToast("check WiFi / host, then Start");
    return;
  }

  st = ST_DONE;
  drawChrome("Start");   // button is a plain Start/Stop toggle now
  drawGraph(s_cancel ? "stopped" : "done");
  drawSummary();
  if (!s_cancel) beep(30, 1800);
}

static void enterSpeed() {
  page = PAGE_SPEED;
  resetTraces();
  st = ST_IDLE;
  uiDrawTopBar("Speed Test");
  drawChrome("Start");
  drawGraph("idle");
  int sy = gY + gH + 4;
  uiClearRect(0, sy, tft.width(), tft.height() - sy - 2);   // wipe whatever this screen (or the previous one) last left here
  resetSummaryFields();
  drawSummary();
}

static void speedTouch(const TouchPoint &t) {
  if (uiTouchInButton(t, actBtn)) {
    runSpeedTest();           // blocking; Stop/back handled inside
    uiWaitForRelease();
  }
}

// ------------------------- LAN speed test ----------------------
// The internet test caps at the lwIP window.
// The LAN test removes this limit.
// It blasts raw TCP to a local PC listener.
// The result shows true radio capacity.
// The code uses the same sampling machinery.
// It avoids the iperf protocol.
// A simple socat command suffices on the PC.

static void lanCfgLoad() {
  Preferences p;
  p.begin("netstats", true);
  lanIP   = p.getString("lan_ip",   lanIP);
  lanPort = p.getUShort("lan_port", lanPort);
  lanDir  = p.getUChar ("lan_dir",  lanDir);
  p.end();
  if (lanDir > 2) lanDir = 0;
}
static void lanCfgSave() {
  Preferences p;
  p.begin("netstats", false);
  p.putString("lan_ip",   lanIP);
  p.putUShort("lan_port", lanPort);
  p.putUChar ("lan_dir",  lanDir);
  p.end();
}

// This function draws the PC listener command.
// The fork option handles parallel streams.
// The summary shows a trimmed one-line hint.
static void drawLanHint(int x, int y, bool oneLine) {
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(NS_LABEL);
  const char *who  = (lanDir == 0) ? "download" : (lanDir == 1) ? "upload" : "both";
  // upload = the listener just sinks bytes; download/both = it also sources
  // an endless zero stream. `both`'s upload half is approximate: `cat`
  // doesn't drain its stdin, so socat back-pressures once its pipe fills.
  const char *sink = (lanDir == 1) ? "/dev/null" : "EXEC:'cat /dev/zero'";
  char lb[72];
  if (oneLine) {                       // summary: one trimmed line (~52 col budget)
    snprintf(lb, sizeof(lb), "PC: socat TCP-LISTEN:%u,fork %s",
             (unsigned)lanPort, (lanDir == 1) ? "/dev/null" : "EXEC:cat /dev/zero");
    tft.setCursor(x, y); tft.print(lb);
    return;
  }
  snprintf(lb, sizeof(lb), "PC listener (%s):", who);
  tft.setCursor(x, y); tft.print(lb);
  snprintf(lb, sizeof(lb), " socat TCP-LISTEN:%u,fork,reuseaddr", (unsigned)lanPort);
  tft.setCursor(x, y + 11); tft.print(lb);
  snprintf(lb, sizeof(lb), "   %s%s", sink, (lanDir == 2) ? "   (up half approx)" : "");
  tft.setCursor(x, y + 22); tft.print(lb);
}

// This function draws the config form.
// It shows IP, port, and direction toggles.
// It replaces the form with graph chrome on start.
static void drawLanConfig() {
  page = PAGE_LAN;
  lanView = LV_CFG;
  // Redraw the top bar too: this is called back from uiNumpadInput(), which
  // clears the whole screen (top bar included) for its modal.
  uiDrawTopBar("LAN speed");
  uiClearBelow(UI_TOPBAR_H + 1);
  tft.setTextWrap(false);
  tft.setTextSize(1);

  tft.setTextColor(NS_LABEL); tft.setCursor(6, 42); tft.print("Server IP");
  lanIpBtn = {96, 34, 210, 28, ""};
  uiDrawButton(lanIpBtn);
  tft.setTextColor(ILI9341_WHITE); tft.setCursor(104, 44); tft.print(lanIP);

  tft.setTextColor(NS_LABEL); tft.setCursor(6, 76); tft.print("Port");
  lanPortBtn = {96, 68, 120, 28, ""};
  uiDrawButton(lanPortBtn);
  { char pb[8]; snprintf(pb, sizeof(pb), "%u", lanPort);
    tft.setTextColor(ILI9341_WHITE); tft.setCursor(104, 78); tft.print(pb); }

  tft.setTextColor(NS_LABEL); tft.setCursor(6, 110); tft.print("Dir");
  lanDnBtn   = {96,  102, 66, 28, "Down"};
  lanUpBtn   = {166, 102, 66, 28, "Up"};
  lanBothBtn = {236, 102, 66, 28, "Both"};
  uiDrawButton(lanDnBtn); uiDrawButton(lanUpBtn); uiDrawButton(lanBothBtn);
  const Btn *a = (lanDir == 0) ? &lanDnBtn : (lanDir == 1) ? &lanUpBtn : &lanBothBtn;
  tft.drawRect(a->x - 2, a->y - 2, a->w + 4, a->h + 4, ILI9341_CYAN);
  tft.drawRect(a->x - 3, a->y - 3, a->w + 6, a->h + 6, ILI9341_CYAN);

  lanStartBtn = {12, 140, tft.width() - 24, 34, "Start"};
  uiDrawButton(lanStartBtn);

  drawLanHint(4, 184, false);
}

static void drawLanSummary() {
  int y = gY + gH + 4;
  uiClearRect(0, y, tft.width(), tft.height() - y - 2);
  tft.setTextSize(1);
  tft.setTextColor(NS_DOWN);
  tft.setCursor(4, y);      tft.printf("Down %7.2f Mbps  peak %.1f", avgDown, peakDown);
  tft.setTextColor(NS_UP);
  tft.setCursor(4, y + 12); tft.printf("Up   %7.2f Mbps  peak %.1f", avgUp, peakUp);
  drawLanHint(4, y + 26, true);
}

// This function runs raw TCP streams for TEST_MS.
// It drains downloads and writes uploads.
// It uses the same sampling cadence as the internet test.
// The stop button aborts the run.
static void runLanTest() {
  esp_wifi_set_ps(WIFI_PS_NONE);   // no modem sleep for the run -- caller restores it
  IPAddress ip;
  if (!ip.fromString(lanIP.c_str())) { errMsg = "bad server IP"; st = ST_ERR; return; }
  const bool doDn = (lanDir == 0 || lanDir == 2);
  const bool doUp = (lanDir == 1 || lanDir == 2);

  WiFiClient dc[N_LAN], uc[N_LAN];
  int dAlive = 0, uAlive = 0;
  if (doDn) for (int i = 0; i < N_LAN; i++) {
    delay(0);                       // feed the WDT across the blocking connects
    if (dc[i].connect(ip, lanPort, CONNECT_MS)) { dc[i].setNoDelay(true); dAlive++; }
  }
  if (doUp) for (int i = 0; i < N_LAN; i++) {
    delay(0);
    if (uc[i].connect(ip, lanPort, CONNECT_MS)) { uc[i].setNoDelay(true); uAlive++; }
  }
  if (dAlive == 0 && uAlive == 0) { errMsg = "server unreachable"; st = ST_ERR; return; }

  dnTotal = upTotal = dnLast = upLast = 0;
  sumStableD = sumStableU = 0; cntStable = 0;
  uint32_t start = millis();
  lastSample = start;
  bool gotData = false;

  while (true) {
    uint32_t el = millis() - start;
    if (el > TEST_MS) break;
    bool activity = false;

    if (doDn) for (int i = 0; i < N_LAN; i++) {
      if (!dc[i].available()) continue;
      int n = dc[i].read(rxbuf, sizeof(rxbuf));
      if (n > 0) { dnTotal += n; gotData = true; activity = true; }
    }
    if (doUp) for (int i = 0; i < N_LAN; i++) {
      if (!uc[i].connected()) continue;
      int w = uc[i].write(zbuf, sizeof(zbuf));
      if (w > 0) { upTotal += w; gotData = true; activity = true; }
    }

    if (!activity) delay(1); else delay(0);   // WDT-safe yield every iteration
    if (millis() - lastSample >= SAMPLE_MS) {
      takeSample(el);
      if (speedAborted()) { s_cancel = true; break; }
    }
  }

  for (int i = 0; i < N_LAN; i++) { dc[i].stop(); uc[i].stop(); }
  if (!gotData) { errMsg = "no data transferred"; st = ST_ERR; return; }
  avgDown = cntStable ? (float)(sumStableD / cntStable) : peakDown;
  avgUp   = cntStable ? (float)(sumStableU / cntStable) : peakUp;
}

static void runLanSpeedTest() {
  resetTraces();
  s_cancel = false;
  st = ST_IDLE;
  ledBusy(true);
  drawChrome("Stop");
  drawGraph("connecting...");
  drawLanSummary();

  runLanTest();
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // restore the default after the test
  ledBusy(false);
  if (st == ST_ERR) {
    drawChrome("Start"); drawGraph(errMsg); drawLanSummary();
    uiToast("start the PC listener, then Start");
    return;
  }

  st = ST_DONE;
  drawChrome("Start");
  drawGraph(s_cancel ? "stopped" : "done");
  drawLanSummary();
  if (!s_cancel) beep(30, 1800);
}

static void enterLan() {
  lanCfgLoad();
  resetTraces();
  st = ST_IDLE;
  drawLanConfig();   // draws the top bar itself
}

static void lanTouch(const TouchPoint &t) {
  if (lanView == LV_RUN) {                       // graph view: Start<->Stop toggle only
    if (uiTouchInButton(t, actBtn)) { runLanSpeedTest(); uiWaitForRelease(); }
    return;
  }
  // --- config form ---
  if (uiTouchInButton(t, lanIpBtn)) {
    String s = uiNumpadInput("Server IP  (e.g. 192.168.1.157)", lanIP);
    IPAddress tmp;
    if (s.length() && tmp.fromString(s.c_str())) { lanIP = s; lanCfgSave(); }
    else if (s.length() && s != lanIP)           uiToast("invalid IP - kept previous");
    drawLanConfig();
    uiWaitForRelease();
  } else if (uiTouchInButton(t, lanPortBtn)) {
    String s = uiNumpadInput("Port  (1-65535)", String(lanPort));
    long v = s.toInt();
    if (v >= 1 && v <= 65535) { lanPort = (uint16_t)v; lanCfgSave(); }
    drawLanConfig();
    uiWaitForRelease();
  } else if (uiTouchInButton(t, lanDnBtn))    { lanDir = 0; lanCfgSave(); drawLanConfig(); }
    else if (uiTouchInButton(t, lanUpBtn))    { lanDir = 1; lanCfgSave(); drawLanConfig(); }
    else if (uiTouchInButton(t, lanBothBtn))  { lanDir = 2; lanCfgSave(); drawLanConfig(); }
    else if (uiTouchInButton(t, lanStartBtn)) {
      lanView = LV_RUN;
      uiClearBelow(UI_TOPBAR_H + 1);   // wipe the form before the graph chrome paints
      runLanSpeedTest();               // blocking; Stop / back handled inside
      uiWaitForRelease();
    }
}

// --------------------- connection + NAT card --------------------
static bool httpGet(const char *host, const char *path, String &body, uint32_t toMs = 6000) {
  WiFiClient c;
  body = "";
  if (!c.connect(host, 80, 4000)) return false;
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: cyd-multitool\r\n"
           "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
  uint32_t t0 = millis();
  bool hdr = true;
  int m = 0;
  while ((c.connected() || c.available()) && millis() - t0 < toMs) {
    while (c.available()) {
      char ch = (char)c.read();
      if (hdr) {
        if (ch == '\r' && (m == 0 || m == 2)) m++;
        else if (ch == '\n' && (m == 1 || m == 3)) { if (++m == 4) hdr = false; }
        else m = (ch == '\r') ? 1 : 0;
      } else {
        body += ch;
        if (body.length() > 1800) { c.stop(); return true; }
      }
    }
    delay(2);
  }
  c.stop();
  return body.length() > 0;
}

// This function extracts a value from a key=value string.
static String traceVal(const String &s, const char *key) {
  String k = String(key) + "=";
  int i = s.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  int e = s.indexOf('\n', i);
  String v = s.substring(i, e < 0 ? s.length() : e);
  v.trim();
  return v;
}

// This function extracts a string field from flat JSON.
static String jsonStr(const String &s, const char *key) {
  String k = String("\"") + key + "\"";
  int i = s.indexOf(k);
  if (i < 0) return "";
  i = s.indexOf(':', i + k.length());
  if (i < 0) return "";
  i++;
  while (i < (int)s.length() && (s[i] == ' ' || s[i] == '"')) i++;
  int e = i;
  while (e < (int)s.length() && s[e] != '"' && s[e] != ',' && s[e] != '}') e++;
  String v = s.substring(i, e);
  v.trim();
  return v;
}

// This function sends one STUN request.
// It parses the mapped address from the reply.
// It reuses one socket across two servers.
// This enables cone versus symmetric mapping.
static bool stunOnce(WiFiUDP &udp, const char *host, uint16_t port,
                     IPAddress &mapped, uint16_t &mport) {
  IPAddress dst;
  if (!WiFi.hostByName(host, dst)) return false;
  uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
  for (int i = 8; i < 20; i++) req[i] = (uint8_t)esp_random();
  udp.beginPacket(dst, port);
  udp.write(req, 20);
  udp.endPacket();

  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    int sz = udp.parsePacket();
    if (sz >= 20) {
      uint8_t b[256];
      int n = udp.read(b, sizeof(b));
      if (n >= 20 && b[0] == 0x01 && b[1] == 0x01) {
        int p = 20;
        while (p + 4 <= n) {
          uint16_t atype = (b[p] << 8) | b[p + 1];
          uint16_t alen  = (b[p + 2] << 8) | b[p + 3];
          int v = p + 4;
          if (v + alen > n) break;
          if ((atype == 0x0020 || atype == 0x0001) && alen >= 8) {
            bool xored = (atype == 0x0020);
            uint16_t rport = (b[v + 2] << 8) | b[v + 3];
            uint8_t a0 = b[v + 4], a1 = b[v + 5], a2 = b[v + 6], a3 = b[v + 7];
            if (xored) {
              rport ^= 0x2112;
              a0 ^= 0x21; a1 ^= 0x12; a2 ^= 0xA4; a3 ^= 0x42;
            }
            mport = rport;
            mapped = IPAddress(a0, a1, a2, a3);
            return true;
          }
          p = v + alen + ((4 - (alen & 3)) & 3);
        }
      }
    }
    delay(10);
  }
  return false;
}

static void drawConnChrome() {
  uiClearRect(0, UI_ACTIONROW_Y, tft.width(), UI_ACTIONROW_H);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(4, UI_ACTIONROW_Y + 9);
  tft.print("network info");
  refreshBtn = {tft.width() - 84, UI_ACTIONROW_Y, 82, UI_ACTIONROW_H, "Refresh"};
  uiDrawMenuButton(refreshBtn);
}

static void drawConnCard() {
  tft.setTextWrap(false);
  uiClearRect(0, UI_CONTENT_Y, tft.width(), tft.height() - UI_CONTENT_Y - 18);
  tft.setTextSize(1);
  const int VX = 86;                       // value column
  int y = UI_CONTENT_Y + 1;

  auto row = [&](uint16_t col, const char *k, const String &v) {
    tft.setTextColor(NS_LABEL); tft.setCursor(4, y);  tft.print(k);
    tft.setTextColor(col);      tft.setCursor(VX, y); tft.print(v);
    y += 12;
  };
  auto hdr = [&](const char *h) {
    tft.setTextColor(accentLabel()); tft.setCursor(4, y); tft.print(h);
    y += 12;
  };
  // Wrapping row: the ASN / org string ("AS7922 Comcast Cable
  // Communications, LLC") often overruns one line -- spill onto a second,
  // breaking at a space where possible.
  auto rowWrap = [&](uint16_t col, const char *k, const String &v) {
    tft.setTextColor(NS_LABEL); tft.setCursor(4, y); tft.print(k);
    tft.setTextColor(col);
    int maxChars = (tft.width() - VX - 4) / 6;   // 6px per glyph at size 1
    if ((int)v.length() <= maxChars) {
      tft.setCursor(VX, y); tft.print(v);
      y += 12;
      return;
    }
    int brk = v.lastIndexOf(' ', maxChars);
    if (brk < maxChars / 2) brk = maxChars;       // no usable space -> hard split
    String l2 = v.substring(brk == maxChars ? brk : brk + 1);
    if ((int)l2.length() > maxChars) l2 = l2.substring(0, maxChars);
    tft.setCursor(VX, y);      tft.print(v.substring(0, brk));
    tft.setCursor(VX, y + 11); tft.print(l2);
    y += 23;
  };

  hdr("-- internet --");
  row(ILI9341_WHITE, "public IP", ci.pubIP.length() ? ci.pubIP : String("?"));
  rowWrap(ILI9341_WHITE, "network", ci.asn.length() ? ci.asn : String("?"));
  row(ILI9341_WHITE, "location",  ci.loc.length()   ? ci.loc   : String("?"));
  row(ILI9341_WHITE, "CF edge",   ci.colo.length()  ? ci.colo  : String("?"));
  row(ILI9341_WHITE, "HTTP/TLS",  ci.proto.length() ? ci.proto : String("?"));
  hdr("-- LAN --");
  row(ILI9341_WHITE, "local IP",  WiFi.localIP().toString());
  row(ILI9341_WHITE, "gateway",   WiFi.gatewayIP().toString());
  hdr("-- NAT (STUN) --");
  row(ci.udpOk ? ILI9341_GREEN : ILI9341_RED, "UDP out", ci.udpOk ? "reachable" : "blocked / no reply");
  row(ILI9341_WHITE, "behind NAT", ci.behindNat.length() ? ci.behindNat : String("?"));
  row(ILI9341_WHITE, "mapping",    ci.natType.length()  ? ci.natType  : String("?"));
}

static void runConnQuery() {
  ci = ConnInfo();
  ledBusy(true);
  uiClearRect(0, UI_CONTENT_Y, tft.width(), tft.height() - UI_CONTENT_Y - 18);
  auto prog = [&](const char *msg) {
    uiClearRect(0, UI_CONTENT_Y, tft.width(), 14);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(4, UI_CONTENT_Y + 2);
    tft.print(msg);
  };

  prog("public IP / edge ...");
  String body;
  if (httpGet("www.cloudflare.com", "/cdn-cgi/trace", body)) {
    ci.pubIP = traceVal(body, "ip");
    ci.colo  = traceVal(body, "colo");
    ci.loc   = traceVal(body, "loc");
    String h = traceVal(body, "http");
    String tls = traceVal(body, "tls");
    ci.proto = h + (tls.length() ? "  " + tls : "");
  }

  prog("ASN / network ...");
  if (httpGet("ip-api.com", "/json/?fields=as,country,regionName,city,query", body)) {
    ci.asn = jsonStr(body, "as");
    if (!ci.pubIP.length()) ci.pubIP = jsonStr(body, "query");
    if (!ci.loc.length()) {
      String c = jsonStr(body, "city"), cc = jsonStr(body, "country");
      ci.loc = c.length() ? (c + ", " + cc) : cc;
    }
  }

  prog("NAT probe (STUN) ...");
  WiFiUDP udp;
  udp.begin(40000 + (esp_random() % 15000));
  IPAddress m1, m2;
  uint16_t p1 = 0, p2 = 0;
  bool s1 = stunOnce(udp, "stun.l.google.com", 19302, m1, p1);
  bool s2 = stunOnce(udp, "stun.cloudflare.com", 3478, m2, p2);
  udp.stop();

  ci.udpOk = s1 || s2;
  if (!ci.udpOk) {
    ci.behindNat = "unknown";
    ci.natType   = "n/a (UDP blocked)";
  } else {
    IPAddress seen = s1 ? m1 : m2;
    ci.behindNat = (seen == WiFi.localIP()) ? "no (public)" : "yes";
    if (s1 && s2) ci.natType = (p1 == p2) ? "endpoint-indep (cone)"
                                          : "endpoint-dep (symmetric)";
    else          ci.natType = "cone? (1 server only)";
    if (!ci.pubIP.length()) ci.pubIP = seen.toString();
  }

  ci.have = true;
  ledBusy(false);
  drawConnCard();
}

static void enterConn() {
  page = PAGE_CONN;
  drawConnChrome();
  runConnQuery();
}

// ------------------------ internal menu -------------------------
static void drawMenu() {
  uiClearBelow(UI_ACTIONROW_Y);
  int w = tft.width() - 24;
  miSpeed = {12, 40,  w, 40, "Speed test"};
  miLan   = {12, 88,  w, 40, "LAN speed"};
  miConn  = {12, 136, w, 40, "Network info"};
  uiDrawMenuButton(miSpeed);
  uiDrawMenuButton(miLan);
  uiDrawMenuButton(miConn);
  tft.setTextSize(1);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(12, 190);
    tft.print("on "); tft.print(WiFi.SSID());
  } else {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(12, 190);
    tft.print("not connected - join via WiFi scan > Connect");
  }
}

static void showNoNetPopup() {
  page = PAGE_NONET;
  uiClearBelow(29);
  int bxw = tft.width() - 40, bxh = 92;
  int bx = 20, by = (tft.height() - bxh) / 2;
  tft.fillRect(bx, by, bxw, bxh, ILI9341_NAVY);
  tft.drawRect(bx, by, bxw, bxh, ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(bx + 10, by + 12);
  tft.print("Select a network first.");
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(bx + 10, by + 34);
  tft.print("Open 'WiFi scan', tap an SSID,");
  tft.setCursor(bx + 10, by + 46);
  tft.print("then tap Connect. Come back here");
  tft.setCursor(bx + 10, by + 58);
  tft.print("once it says connected.");
}

static void gotoMenu() {
  st = ST_IDLE;
  page = PAGE_MENU;
  graphFree();                 // release the ~50-66KB canvas
  uiDrawTopBar("Net stats");   // sub-pages retitle the bar; restore it here
  drawMenu();
}

// --------------------------- contract --------------------------
void netstatsEnter() {
  uiDrawTopBar("Net stats");
  resetTraces();
  st = ST_IDLE;
  page = PAGE_MENU;
  drawMenu();
}

// This function skips the menu.
// It opens a sub-page directly after connection.
void netstatsGoSpeedTest() {
  if (WiFi.status() == WL_CONNECTED) enterSpeed();
}
void netstatsGoConn() {
  if (WiFi.status() == WL_CONNECTED) enterConn();
}

void netstatsLoop() {
  // These functions run as blocking modals.
  // The loop has nothing to service.
}

void netstatsTouch(const TouchPoint &t) {
  switch (page) {
    case PAGE_MENU:
      if (uiTouchInButton(t, miSpeed)) {
        if (WiFi.status() != WL_CONNECTED) { showNoNetPopup(); return; }
        enterSpeed();
      } else if (uiTouchInButton(t, miLan)) {
        if (WiFi.status() != WL_CONNECTED) { showNoNetPopup(); return; }
        enterLan();
      } else if (uiTouchInButton(t, miConn)) {
        if (WiFi.status() != WL_CONNECTED) { showNoNetPopup(); return; }
        enterConn();
      }
      return;
    case PAGE_NONET:
      return;
    case PAGE_SPEED:
      speedTouch(t);
      return;
    case PAGE_LAN:
      lanTouch(t);
      return;
    case PAGE_CONN:
      if (uiTouchInButton(t, refreshBtn)) { runConnQuery(); uiWaitForRelease(); return; }
      return;
  }
}

// This function handles back navigation.
// It steps back one level at a time.
bool netstatsHandleBack() {
  if (page == PAGE_MENU) return false;
  // LAN graph view steps back to its own config form first; everything else
  // (and the LAN form itself) steps back to the netstats menu.
  if (page == PAGE_LAN && lanView == LV_RUN) {
    graphFree();
    drawLanConfig();
    return true;
  }
  gotoMenu();
  return true;
}

void netstatsExit() {
  st = ST_IDLE;
  page = PAGE_MENU;
  graphFree();
  // The code keeps WiFi connected.
  // The user joined deliberately.
  // They may run tests again.
}
