#include "splash.h"
#include "ui.h"
#include "pins.h"
#include <Preferences.h>
#include "driver/dac_continuous.h"
#include "modmix.h"
#include "demo_mod.h"            // DEMO_MOD / DEMO_MOD_LEN (the chase track)
#include "splash_landscape.h"   // SPLASH_LANDSCAPE[76800]  (320x240)
#include "splash_portrait.h"    // SPLASH_PORTRAIT[76800]   (240x320)

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
  if (!splashEnabled()) return;   // image disabled in System > Display
  bool landscape = tft.width() >= tft.height();
  const uint16_t *img = landscape ? SPLASH_LANDSCAPE : SPLASH_PORTRAIT;
  int iw = landscape ? 320 : 240;
  int ih = landscape ? 240 : 320;
  int w = iw < tft.width()  ? iw : tft.width();
  int h = ih < tft.height() ? ih : tft.height();

  tft.startWrite();
  tft.setAddrWindow(0, 0, w, h);
  // Row by row so a panel smaller than 320x240 still lands the top-left.
  for (int y = 0; y < h; y++)
    tft.writePixels((uint16_t *)(img + y * iw), w, true, false);
  tft.endWrite();

  // Fade out via a couple of dim overlays would need alpha we don't have;
  // just hold then let the caller draw the menu over it.
  uint32_t start = millis();
  while (millis() - start < holdMs) {
    if (uiReadTouch().pressed) break;   // tap to skip
    delay(15);
  }
}

// Real 4-channel MOD, software-mixed (modmix.cpp) out through the SHARED
// GPIO26 DAC channel from ui.cpp -- own core-0 task, full ~22kHz waveform
// centred at mid-rail so nothing allocates/frees the DAC and beeps keep
// working afterwards. Lifted from the retired Outrun demo.
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

  // Short timeout on the settle write so a starved DMA ring can't wedge
  // this task -- a hung shutdown here is what left the DAC dead on exit.
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

  // Full-screen, orientation-matched: the landscape art (320x240) on a
  // landscape panel, the portrait art (240x320) on a portrait one -- both
  // are a straight 1:1 blit, no rotation / letterbox.
  bool landscape = tft.width() >= tft.height();
  const uint16_t *img = landscape ? SPLASH_LANDSCAPE : SPLASH_PORTRAIT;
  int iw = landscape ? 320 : 240;
  int sw = tft.width(), sh = tft.height();

  tft.startWrite();
  tft.setAddrWindow(0, 0, sw, sh);
  for (int y = 0; y < sh; y++)
    tft.writePixels((uint16_t *)(img + y * iw), sw, true, false);
  tft.endWrite();

  while (!uiReadTouch().pressed) delay(15);
  uiWaitForRelease();

  if (haveAudio) {
    aRun = false;
    uint32_t g = millis();
    while (!aDone && millis() - g < 1500) delay(5);
    modmixFree();
    // Rebuild the DAC channel from scratch: the render loop competing with
    // the splash blit for SPI/DMA can latch the channel into a state where
    // plain enable/disable no longer produces sound. del + recreate does.
    uiAudioReset();
  }
}
