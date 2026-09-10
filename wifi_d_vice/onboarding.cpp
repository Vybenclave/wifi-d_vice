#include "onboarding.h"
#include "ui.h"
#include "system_screen.h"
#include "ble_2fa.h"
#include <Preferences.h>

static void interstitial(const char *step) {
  uiClearBelow(0);
  tft.setTextWrap(false);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(step, 0, 0, &bx, &by, &bw, &bh);
  if (bw > (uint16_t)tft.width() - 20) {
    // doesn't fit at size 2 in the narrower orientations -- drop to size 1
    // rather than let it auto-wrap or clip
    tft.setTextSize(1);
    tft.getTextBounds(step, 0, 0, &bx, &by, &bw, &bh);
  }
  tft.setCursor((tft.width() - bw) / 2 - bx, tft.height() / 2 - 10);
  tft.print(step);
  delay(900);
}

void onboardingRunWizard() {
  interstitial("Step 1/3: orientation");
  systemShowRotationPicker();

  interstitial("Step 2/3: beep volume");
  systemShowVolumePicker();

  interstitial("Step 3/3: BLE time sync");
  ble2faPairAndWait("skip");   // best-effort -- fine to skip and do later
}

void onboardingRunIfNeeded() {
  Preferences p;
  p.begin("touchcal", true);
  bool done = p.getBool("onboarded", false);
  p.end();
  if (done) return;

  onboardingRunWizard();

  p.begin("touchcal", false);
  p.putBool("onboarded", true);
  p.end();
}
