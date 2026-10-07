#include "splash.h"
#include "ui.h"
#include "pins.h"
#include <Preferences.h>
#include "driver/dac_continuous.h"
#include "modmix.h"
#include "demo_mod.h"            // DEMO_MOD and DEMO_MOD_LEN define the chase track.
#include "splash_landscape.h"   // SPLASH_LANDSCAPE holds 19200 pixels. The renderer upscales it 2x.
#include "splash_portrait.h"    // SPLASH_PORTRAIT holds 19200 pixels. The renderer upscales it 2x.
#include "power.h"

bool splashEnabled() {
  Preferences p;
  p.begin("disp", true);
  bool on = p.getBool("splash", true);
  p.end();
  return on;
}

void splashSetEnabled(bool on) {
  Preferences p;
  p.begin("disp", false);
  p.putBool("splash", on);
  p.end();
}

void showSplash(uint32_t holdMs) {
  if (!splashEnabled()) return;   // The user disabled the splash image in the display settings.
  bool landscape = tft.width() >= tft.height();
  const uint16_t *img = landscape ? SPLASH_LANDSCAPE : SPLASH_PORTRAIT;
  int srcW = landscape ? 160 : 120, srcH = landscape ? 120 : 160;
  int iw = landscape ? 320 : 240;
  int ih = landscape ? 240 : 320;
  int w = iw < tft.width()  ? iw : tft.width();
  int h = ih < tft.height() ? ih : tft.height();

  static uint16_t line[320];
  tft.startWrite();
  tft.setAddrWindow(0, 0, w, h);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) line[x] = uiBgSample(img, srcW, srcH, x, y);
    tft.writePixels(line, w, true, false);
  }
  tft.endWrite();

  uint32_t start = millis();
  while (millis() - start < holdMs) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed) break;   // The tap skips the splash.
    delay(15);
  }
}

// The software mixer sends four channels to the shared GPIO26 DAC.
// The task runs on core 0.
// The waveform centers at mid-rail.
// This prevents allocation conflicts.
// Beep sounds continue to work.
static volatile bool aRun = false, aDone = false;

static void splashAudioTask(void *) {
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
    dac_continuous_write(h, buf, sizeof(buf), &wrote, 200);
  }

  // The short timeout prevents a starved DMA ring from hanging the task.
  memset(buf, 128, sizeof(buf));
  dac_continuous_write(h, buf, sizeof(buf), &wrote, 20);
  digitalWrite(AUDIO_EN, HIGH);
  dac_continuous_disable(h);
  aDone = true;
  vTaskDelete(nullptr);
}

void showSplashArt() {
  bool haveAudio = modmixLoad(DEMO_MOD, DEMO_MOD_LEN);
  if (haveAudio) {
    modmixStart();
    aRun = true; aDone = false;
    xTaskCreatePinnedToCore(splashAudioTask, "splashaud", 6144, nullptr, 3, nullptr, 0);
  }

  // The renderer scales the art to match the panel orientation.
  // It upscales the half-resolution source 2x.
  // The code skips rotation and letterboxing.
  bool landscape = tft.width() >= tft.height();
  const uint16_t *img = landscape ? SPLASH_LANDSCAPE : SPLASH_PORTRAIT;
  int srcW = landscape ? 160 : 120, srcH = landscape ? 120 : 160;
  int sw = tft.width(), sh = tft.height();

  static uint16_t line[320];
  tft.startWrite();
  tft.setAddrWindow(0, 0, sw, sh);
  for (int y = 0; y < sh; y++) {
    for (int x = 0; x < sw; x++) line[x] = uiBgSample(img, srcW, srcH, x, y);
    tft.writePixels(line, sw, true, false);
  }
  tft.endWrite();

  for (;;) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed) break;
    delay(15);
  }
  uiWaitForRelease();

  if (haveAudio) {
    aRun = false;
    uint32_t g = millis();
    while (!aDone && millis() - g < 1500) delay(5);
    modmixFree();
    // The render loop competes with the splash blit for SPI and DMA.
    // This can latch the DAC channel.
    // Plain enable and disable commands then fail.
    // The code destroys and recreates the channel.
    uiAudioReset();
  }
}
