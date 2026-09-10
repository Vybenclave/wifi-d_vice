#include "demo.h"
#include <Arduino.h>
#include <math.h>
#include "driver/dac_continuous.h"
#include "ui.h"
#include "pins.h"
#include "modmix.h"
#include "demo_mod.h"

// ------------------------------------------------------------------
// colour helpers (RGB565)
// ------------------------------------------------------------------
static inline uint16_t rgb(int r, int g, int b) {
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline int lp(int a, int b, float t) { return a + (int)((b - a) * t + 0.5f); }
static inline uint16_t mix(int r1,int g1,int b1, int r2,int g2,int b2, float t) {
  return rgb(lp(r1,r2,t), lp(g1,g2,t), lp(b1,b2,t));
}

// ------------------------------------------------------------------
// audio -- real 4-channel MOD, software-mixed (modmix.cpp) out through the
// SHARED GPIO26 DAC channel from ui.cpp (uiDac()). Own core-0 task. A full
// ~22kHz waveform centred at mid-rail: no DC bias on the amp, and nothing
// allocates/frees the DAC so beeps still work after the demo.
// ------------------------------------------------------------------
static volatile bool aRun = false, aDone = false;

static void demoAudioTask(void *) {
  dac_continuous_handle_t h = uiDac();
  if (!h) { aDone = true; vTaskDelete(nullptr); return; }

  modmixSetRate(uiDacRate());
  dac_continuous_enable(h);
  digitalWrite(AUDIO_EN, LOW);
  delay(15);

  static uint8_t buf[1024];
  size_t wrote;
  while (aRun) {
    modmixRender(buf, sizeof(buf));
    dac_continuous_write(h, buf, sizeof(buf), &wrote, 200);   // blocks for DMA room
  }

  memset(buf, 128, sizeof(buf));                              // settle to mid-rail
  dac_continuous_write(h, buf, sizeof(buf), &wrote, 100);
  digitalWrite(AUDIO_EN, HIGH);
  dac_continuous_disable(h);
  aDone = true;
  vTaskDelete(nullptr);
}

// ------------------------------------------------------------------
// scene
// ------------------------------------------------------------------
static int W, H, HZ, vpX;

// left-zone Art Deco skyline (x is within the left zone, h rises above HZ)
struct Bld { int x, w, h; int dr, dg, db; };  // day fill colour
static const Bld BLDS[] = {
  {  2, 22, 40,  238, 200, 190 },
  { 26, 30, 66,  180, 224, 216 },
  { 58, 18, 30,  245, 226, 188 },
  { 78, 26, 54,  214, 192, 226 },
  {106, 20, 44,  236, 210, 196 },
};
static const int NBLD = sizeof(BLDS) / sizeof(BLDS[0]);
static const uint16_t NEON[3] = { 0x5EFF, 0xF8DA, 0x8FE7 };  // cyan / magenta / lime

// Just the gradient bands. Drawn first in a sky refresh so the title (drawn
// right after) is only "missing" for the ~1-2ms it takes to paint its rows.
static void drawSkyBands(float dayl) {
  float tw = 1.0f - fabsf(dayl - 0.5f) * 2.0f;
  if (tw < 0) tw = 0;
  for (int i = 0; i < HZ; i += 6) {
    float f = (float)i / HZ;
    uint16_t nite = mix(4, 4, 26,   26, 16, 46, f);
    uint16_t day  = mix(28, 96, 220, 150, 200, 245, f);
    uint16_t c    = mix(((nite>>11)<<3),((nite>>5&0x3F)<<2),((nite&0x1F)<<3),
                        ((day>>11)<<3), ((day>>5&0x3F)<<2), ((day&0x1F)<<3), dayl);
    if (tw > 0.01f) {
      float w = tw * (0.25f + 0.75f * f);
      c = mix(((c>>11)<<3), ((c>>5&0x3F)<<2), ((c&0x1F)<<3), 255, 120, 40, w * 0.7f);
    }
    tft.fillRect(0, i, W, 6, c);
  }
}

// Sun/moon + stars -- drawn AFTER the title so a slow star loop doesn't
// stretch the title's blank window. The disc is skipped where it would
// overprint the wordmark.
static void drawSkyExtras(float tod, float dayl) {
  int tlx = (int)(W * 0.40f) - 4, tlb = 52;
  float ph = fmodf(tod * 2.0f, 1.0f);
  int dx = W - (int)(W * ph);   // east-coast, heading north: rises on the right, sets on the left
  int dy = HZ - (int)(sinf(ph * PI) * (HZ - 12));
  bool sun = dayl > 0.5f;
  if (!(dx > tlx - 22 && dy - 22 < tlb)) {          // don't cover "VICE"
    uint16_t core = sun ? rgb(255, 224, 90) : rgb(224, 228, 238);
    tft.fillCircle(dx, dy, 18, sun ? mix(255,224,90, 255,140,60, 0.4f) : rgb(70,74,96));
    tft.fillCircle(dx, dy, 12, core);
  }
  if (dayl < 0.55f) {
    float b = (0.55f - dayl) / 0.55f;
    uint16_t s = mix(20, 20, 40, 235, 235, 255, b);
    uint32_t r = 0xB16B00B5u;
    for (int i = 0; i < 60; i++) {
      r = r * 1664525u + 1013904223u; int sx = (r >> 9) % W;
      r = r * 1664525u + 1013904223u; int sy = (r >> 9) % (HZ - 6) + 2;
      if (sx > tlx && sy < tlb) continue;           // keep stars off the title
      if ((i & 3) == 0 || b > 0.6f) tft.drawPixel(sx, sy, s);
    }
  }
}

static void drawSkyline(float dayl) {
  bool night = dayl < 0.45f;
  float nf = night ? (0.45f - dayl) / 0.45f : 0.0f;   // 0..1 neon intensity
  for (int i = 0; i < NBLD; i++) {
    const Bld &b = BLDS[i];
    int by = HZ - b.h;
    uint16_t fill = mix(b.dr, b.dg, b.db, 18, 16, 34, 1.0f - dayl);
    tft.fillRect(b.x, by, b.w, b.h, fill);
    // Art Deco crown: a stepped narrower block + a mast
    int cw = b.w / 2, cx = b.x + (b.w - cw) / 2, ch = b.h / 5 + 3;
    tft.fillRect(cx, by - ch, cw, ch, fill);
    tft.drawFastVLine(b.x + b.w / 2, by - ch - 5, 5, night ? NEON[i % 3] : fill);

    if (nf > 0.15f) {
      uint16_t n = NEON[i % 3];
      tft.drawRect(b.x, by, b.w, b.h, n);
      tft.drawRect(cx, by - ch, cw, ch, n);
      uint16_t win = rgb(255, 216, 120);
      for (int wy = by + 5; wy < HZ - 3; wy += 7)
        for (int wx = b.x + 3; wx < b.x + b.w - 2; wx += 6)
          if (((wx * 7 + wy * 13 + i) % 5) < 3) tft.drawPixel(wx, wy, win);
    }
  }
}

static void drawGround(float curveT, float scroll, int waterPhase) {
  for (int y = HZ; y < H; y++) {
    float p  = (float)(y - HZ) / (H - HZ);
    float pp = p * p;
    int   half = 4 + (int)(pp * (W * 0.60f));
    int   cx = vpX + (int)(sinf(curveT + p * 2.2f) * pp * (W * 0.20f));
    int   rl = cx - half, rr = cx + half;
    int   band = ((int)(pp * 20.0f - scroll)) & 1;   // -scroll => stripes flow toward the viewer (forward)

    uint16_t road   = band ? rgb(58, 58, 66)  : rgb(72, 72, 82);
    uint16_t side   = band ? rgb(150,120, 70) : rgb(170,140, 88);   // palm-lined kerb
    uint16_t rumble = band ? rgb(210, 40, 40) : rgb(235,235,235);
    uint16_t sand   = band ? rgb(226,196,150) : rgb(240,214,170);
    uint16_t water  = ((y + waterPhase) & 3) < 2 ? rgb(40,150,190) : rgb(24,110,160);

    if (rl < 0)  rl = 0;
    if (rr > W)  rr = W;
    if (rl > 3)              tft.drawFastHLine(0, y, rl - 3, side);
    if (rl >= 0)             tft.drawFastHLine(rl - 3 < 0 ? 0 : rl - 3, y, 3, rumble);
    if (rr - 3 > rl)         tft.drawFastHLine(rl, y, (rr - 3) - rl, road);
    tft.drawFastHLine(rr - 3, y, 3, rumble);

    int sandEnd = rr + 6 + (int)(pp * 28);
    if (sandEnd > W) sandEnd = W;
    if (sandEnd > rr) tft.drawFastHLine(rr, y, sandEnd - rr, sand);
    if (sandEnd < W)  tft.drawFastHLine(sandEnd, y, W - sandEnd, water);

    // dashed centre line
    if (((int)(pp * 15.0f - scroll * 0.5f)) & 1) {
      int dw = 1 + (int)(pp * 3);
      tft.drawFastHLine(cx - dw, y, dw * 2, rgb(240, 220, 90));
    }
  }
}

static void drawCar(float curveT, float dayl) {
  float p = 0.92f;
  int cx = vpX + (int)(sinf(curveT + p * 2.2f) * (p * p) * (W * 0.20f));
  cx += (int)(sinf(millis() * 0.004f) * 5.0f);
  int y = H - 40;
  bool night = dayl < 0.5f;

  uint16_t body = rgb(238, 238, 244);
  tft.fillRect(cx - 26, y + 27, 52, 4, rgb(8, 8, 14));                 // shadow
  tft.fillRect(cx - 25, y + 16, 8, 12, rgb(10, 10, 12));              // wheels
  tft.fillRect(cx + 17, y + 16, 8, 12, rgb(10, 10, 12));
  tft.fillRect(cx - 24, y + 7,  48, 17, body);                        // lower body
  tft.fillRect(cx - 16, y - 3,  32, 12, rgb(230, 230, 238));          // cabin
  tft.fillRect(cx - 13, y - 1,  26, 7,  rgb(28, 34, 46));             // rear glass
  for (int i = 0; i < 3; i++) {                                       // side strakes
    tft.drawFastHLine(cx - 24, y + 10 + i * 3, 9, rgb(120, 122, 132));
    tft.drawFastHLine(cx + 15, y + 10 + i * 3, 9, rgb(120, 122, 132));
  }
  tft.fillRect(cx - 22, y + 18, 44, 3, rgb(200, 20, 20));             // tail bar
  if (night) {
    tft.fillRect(cx - 22, y + 18, 44, 3, rgb(255, 70, 70));
    tft.drawFastHLine(cx - 24, y + 22, 48, rgb(120, 12, 12));         // glow
    tft.fillRect(cx - 20, y + 24, 4, 3, rgb(255, 90, 40));
    tft.fillRect(cx + 16, y + 24, 4, 3, rgb(255, 90, 40));
  }
  tft.fillRect(cx - 5, y + 21, 10, 3, rgb(35, 35, 40));              // diffuser
}

static void drawTitle() {
  tft.setTextWrap(false);
  // "WIFI D_VICE" small, "VICE" as the big retro wordmark, version
  // underneath. Kept to the right of the left-side skyline.
  int tx = (int)(W * 0.40f);
  tft.setTextSize(1);
  tft.setTextColor(rgb(90, 240, 255));
  tft.setCursor(tx, 6); tft.print("WIFI D_VICE");
  tft.setTextSize(3);
  tft.setTextColor(rgb(255, 60, 200));
  tft.setCursor(tx + 8, 18); tft.print("VICE");
  tft.setTextColor(rgb(90, 240, 255));
  tft.setCursor(tx + 6, 16); tft.print("VICE");
  tft.setTextSize(1);
  tft.setTextColor(rgb(255, 60, 200));
  tft.setCursor(tx + 6, 42); tft.print("v0.9");

  tft.setTextColor(rgb(120, 120, 130));
  tft.setCursor(4, H - 10); tft.print("tap to exit");
}

// ------------------------------------------------------------------
// Foreground Art Deco buildings whizzing past on the LEFT of the road --
// separate from the static distant skyline. Each has a world depth z that
// counts down toward the camera; drawn only in the ground region (y>=HZ)
// so it never fights the flicker-gated sky repaint.
// ------------------------------------------------------------------
#define NRB 6
static const int   RB_R[3] = { 238, 176, 246 };
static const int   RB_G[3] = { 198, 226, 224 };
static const int   RB_B[3] = { 188, 214, 186 };
static const float RB_ZMAX = 66.0f;
static const float RB_DNEAR = 7.0f;
static float rbZ[NRB];
static int   rbH[NRB], rbC[NRB];

static void rbRecycle(int i) {
  rbZ[i] += RB_ZMAX;
  int seed = (int)rbZ[i] * 131 + i * 977;
  rbH[i] = 22 + (seed & 0x3F);
  rbC[i] = ((seed >> 3) & 0x7FFF) % 3;
}
static void rbInit() {
  for (int i = 0; i < NRB; i++) {
    rbZ[i] = 2.0f + i * (RB_ZMAX / NRB);
    rbH[i] = 24 + (i * 41) % 46;
    rbC[i] = i % 3;
  }
}

static void drawRoadBuildings(float curveT, float dz, float dayl) {
  bool  night = dayl < 0.5f;
  float ncol  = 1.0f - dayl;
  for (int i = 0; i < NRB; i++) {
    rbZ[i] -= dz;
    if (rbZ[i] <= 0.5f) rbRecycle(i);

    float p = RB_DNEAR / (RB_DNEAR + rbZ[i]);          // z large => p~0 (horizon); z->0 => p->1 (at the camera)
    if (p <= 0.02f || p > 1.25f) continue;
    float pp = p * p;

    int half     = 4 + (int)(pp * (W * 0.60f));
    int cxr      = vpX + (int)(sinf(curveT + p * 2.2f) * pp * (W * 0.20f));
    int roadLeft = cxr - half;
    int gy = HZ + (int)(p * (H - HZ));
    if (gy > H) gy = H;

    int bw = 8 + (int)(pp * 82);   if (bw > W / 2) bw = W / 2;
    int bh = 8 + (int)(rbH[i] * pp * 2.8f); if (bh > H - HZ) bh = H - HZ;
    int bx = roadLeft - bw - (int)(pp * 6);
    int by = gy - bh;
    if (bx + bw <= 0) continue;
    if (by < HZ) { bh -= (HZ - by); by = HZ; }          // clip into the ground band
    if (bh < 3) continue;
    if (bx < 0) { bw += bx; bx = 0; }
    if (bx + bw > W) bw = W - bx;
    if (bw < 3) continue;

    int ci = rbC[i];
    uint16_t fill = mix(RB_R[ci], RB_G[ci], RB_B[ci], 16, 16, 30, ncol);
    tft.fillRect(bx, by, bw, bh, fill);
    // stepped deco crown
    int cwd = bw / 2, ccx = bx + (bw - cwd) / 2, chh = bh / 7 + 2;
    if (by - chh >= HZ - 2) tft.fillRect(ccx, by - chh, cwd, chh, fill);
    if (night && p > 0.12f) {
      tft.drawRect(bx, by, bw, bh, NEON[ci]);
      uint16_t win = rgb(255, 214, 120);
      for (int wy = by + 4; wy < by + bh - 3; wy += 6)
        for (int wx = bx + 3; wx < bx + bw - 2; wx += 6)
          if (((wx * 5 + wy * 7 + i) & 3) == 0) tft.drawPixel(wx, wy, win);
    }
  }
}

// ------------------------------------------------------------------
void demoRun() {
  W = tft.width();
  H = tft.height();
  HZ = H * 2 / 5;
  vpX = W * 55 / 100;

  bool haveAudio = modmixLoad(DEMO_MOD, DEMO_MOD_LEN);
  if (haveAudio) {
    modmixStart();
    aRun = true; aDone = false;
    xTaskCreatePinnedToCore(demoAudioTask, "demoaud", 6144, nullptr, 3, nullptr, 0);
  }

  tft.fillScreen(0);
  rbInit();
  float curveT = 0.0f, scroll = 0.0f;
  int waterPhase = 0;
  int lastBucket = -999;
  uint32_t t0 = millis();

  for (;;) {
    TouchPoint tp = uiReadTouch();
    if (tp.pressed) break;

    float secs = (millis() - t0) / 1000.0f;
    float tod  = fmodf(secs / 24.0f, 1.0f);
    float dayl = 0.5f - 0.5f * cosf(tod * 6.2831853f);

    // Sky / skyline / title refresh only ~3x/sec, and the title is redrawn
    // right after the gradient bands (before sun/stars/skyline), so its
    // blank window is ~1ms -- no visible blink.
    int bucket = (int)(secs * 3.0f);
    if (bucket != lastBucket) {
      lastBucket = bucket;
      drawSkyBands(dayl);
      drawTitle();
      drawSkyExtras(tod, dayl);
      drawSkyline(dayl);
    }
    drawGround(curveT, scroll, waterPhase);
    drawCar(curveT, dayl);
    drawRoadBuildings(curveT, 0.95f, dayl);

    curveT += 0.028f;
    scroll += 0.35f;
    waterPhase = (waterPhase + 1) & 255;
    delay(5);
  }

  uiWaitForRelease();
  if (haveAudio) {
    aRun = false;
    uint32_t g = millis();
    while (!aDone && millis() - g < 800) delay(5);
    modmixFree();
  }
}
