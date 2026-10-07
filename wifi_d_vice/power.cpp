#include "power.h"
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp_random.h>
#include <Preferences.h>
#include "ui.h"
#include "pins.h"
#include "engagement.h"
#include <Adafruit_ILI9341.h>

// The LED heartbeat stops when the CPU sleeps. Light sleep wakes briefly to redraw it. Deep sleep reboots the chip. The LED stays off during deep sleep. All tiers debounce touch wake against TP_IRQ. Electrical noise on this line causes false wake events.
static bool s_pendingWake = false;
bool powerTakePendingWake() { bool w = s_pendingWake; s_pendingWake = false; return w; }

static void panelAsleep(bool asleep);   // defined below -- forward-declared for wakeGestureChallenge()

static bool realTouchPresent() {
  pinMode(TP_IRQ, INPUT);
  delay(5);   // let the line settle after an edge
  return digitalRead(TP_IRQ) == LOW;
}

// A simple swipe distance triggers accidentally. The code requires a drag challenge instead. It shows a dot and a target circle on opposite edges. The user must drag the dot to the circle. Early release resets the dot. The function times out after ten seconds. It returns true on success. It returns false on timeout.
static bool wakeGestureChallenge() {
  int w = tft.width(), h = tft.height();
  int startX, startY, targetX, targetY;
  switch (esp_random() % 4) {
    case 0: startX = 8;     startY = 20 + esp_random() % (h - 40); targetX = w - 8; targetY = startY; break;
    case 1: startX = w - 8; startY = 20 + esp_random() % (h - 40); targetX = 8;     targetY = startY; break;
    case 2: startY = 8;     startX = 20 + esp_random() % (w - 40); targetY = h - 8; targetX = startX; break;
    default: startY = h - 8; startX = 20 + esp_random() % (w - 40); targetY = 8;    targetX = startX; break;
  }

  panelAsleep(false);
  ledHeartbeat(false);   // screen's lit and interactive now -- not "ambient asleep" any more

  uiClearBelow(0);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(8, h / 2 - 10);
  tft.print("Drag the dot to the circle");
  tft.setCursor(8, h / 2 + 4);
  tft.print("to wake.");
  const int DOT_R = 7, TARGET_R = 10;
  tft.drawCircle(targetX, targetY, TARGET_R, ILI9341_WHITE);
  int curX = startX, curY = startY;
  tft.fillCircle(curX, curY, DOT_R, ILI9341_YELLOW);

  // The user must grab the dot to move it. Tapping near the target does not win instantly. The `dragging` flag activates only on initial touch. The touch must land within the grab radius. Touches outside this radius count as activity. They do not move the dot.
  const int GRAB_R = DOT_R + 14;
  bool dragging = false;
  uint32_t lastTouchMs = millis();
  for (;;) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) {
      lastTouchMs = millis();
      if (t.isNewPress) {
        int gdx = t.x - curX, gdy = t.y - curY;
        dragging = (gdx * gdx + gdy * gdy <= GRAB_R * GRAB_R);
      }
      if (dragging && (t.x != curX || t.y != curY)) {
        uiClearRect(curX - DOT_R - 1, curY - DOT_R - 1, DOT_R * 2 + 2, DOT_R * 2 + 2);
        curX = t.x; curY = t.y;
        tft.fillCircle(curX, curY, DOT_R, ILI9341_YELLOW);
        tft.drawCircle(targetX, targetY, TARGET_R, ILI9341_WHITE);   // re-draw in case the dot just covered it
      }
      if (dragging) {
        int dx = curX - targetX, dy = curY - targetY;
        if (dx * dx + dy * dy < (TARGET_R + DOT_R) * (TARGET_R + DOT_R)) return true;
      }
    } else {
      if (dragging && (curX != startX || curY != startY)) {
        // released mid-drag before reaching the target -- reset the dot, same target, try again
        uiClearRect(curX - DOT_R - 1, curY - DOT_R - 1, DOT_R * 2 + 2, DOT_R * 2 + 2);
        curX = startX; curY = startY;
        tft.fillCircle(curX, curY, DOT_R, ILI9341_YELLOW);
      }
      dragging = false;
    }
    if (millis() - lastTouchMs > 10000) {
      panelAsleep(true);
      return false;   // gave up -- caller's light-sleep loop resumes
    }
    delay(20);
  }
}

static void panelAsleep(bool asleep) {
  if (asleep) {
    tft.sendCommand(ILI9341_DISPOFF);
    tft.sendCommand(ILI9341_SLPIN);
    digitalWrite(TFT_BL, LOW);
  } else {
    tft.sendCommand(ILI9341_SLPOUT);
    delay(120);   // Wait for the panel to settle after sleep.
    tft.sendCommand(ILI9341_DISPON);
    digitalWrite(TFT_BL, HIGH);
  }
}

// The display panel stores the last frame in GRAM. Turning it back on resumes the screen in place. The CPU stays awake during this mode.
static void blankUntilTouch() {
  panelAsleep(true);
  for (;;) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) break;
    delay(50);
  }
  panelAsleep(false);
  uiWaitForRelease();
}

// Zero means never. The code saves this value in NVS. It survives reboots.
static const uint32_t kTimeoutOptionsMs[] = {30000, 60000, 180000, 300000, 600000, 900000};
static const char *const kTimeoutLabels[] = {"30 seconds", "1 minute", "3 minutes", "5 minutes", "10 minutes", "15 minutes"};
static const int kTimeoutOptionCount = 6;
static const char *timeoutItemLabel(int i) { return kTimeoutLabels[i]; }
static int msToIndex(uint32_t ms) {
  for (int i = 0; i < kTimeoutOptionCount; i++) if (kTimeoutOptionsMs[i] == ms) return i;
  return 0;
}

static uint32_t s_timeoutMs = 0;
static uint32_t s_lastActivityMs = 0;

void powerDisplayTimeoutLoad() {
  Preferences p;
  p.begin("power", true);
  s_timeoutMs = p.getUInt("dispoff_ms", 0);
  p.end();
  s_lastActivityMs = millis();   // don't let a stale/zero timestamp fire immediately on boot
}

static void powerDisplayTimeoutSave(uint32_t ms) {
  s_timeoutMs = ms;
  Preferences p;
  p.begin("power", false);
  p.putUInt("dispoff_ms", ms);
  p.end();
}

void powerNoteActivity() { s_lastActivityMs = millis(); }

// This creates a forced awake window. It prevents alerts from disappearing. The duration extends existing windows. It never shortens them.
static uint32_t s_wakeUntilMs = 0;
void powerWakeFor(uint32_t ms) {
  uint32_t until = millis() + ms;
  if ((int32_t)(until - s_wakeUntilMs) > 0) s_wakeUntilMs = until;
  powerNoteActivity();
}

void powerServiceAutoOff() {
  if (s_timeoutMs == 0) return;
  if ((int32_t)(s_wakeUntilMs - millis()) > 0) { s_lastActivityMs = millis(); return; }
  if (millis() - s_lastActivityMs >= s_timeoutMs) {
    blankUntilTouch();
    s_lastActivityMs = millis();   // the touch that just woke it counts as activity
  }
}

// This screen sets the inactivity duration. Selecting Never dims the picker. Apply saves the setting. Back cancels changes.
void powerShowDisplayTimeoutSettings() {
  bool never = (s_timeoutMs == 0);
  int idx = never ? 0 : msToIndex(s_timeoutMs);

  uiDrawTopBar("Display Timeout");
  uiSetBgMode(UI_BG_IMAGE);

  Btn timeoutBtn, neverBtn, applyBtn;
  auto draw = [&]() {
    uiClearBelow(29);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(1);
    tft.setCursor(8, 38);
    tft.print("Turn off the display after:");
    timeoutBtn = {8, 56, tft.width() - 16, 30, timeoutItemLabel(idx)};
    neverBtn   = {8, 94, tft.width() - 16, 30, never ? "Never (on)" : "Never (off)"};
    applyBtn   = {8, 140, tft.width() - 16, 34, "Apply"};
    if (never) uiDrawButtonDim(timeoutBtn); else uiDrawMenuButton(timeoutBtn);
    uiDrawMenuButton(neverBtn);
    uiDrawMenuButton(applyBtn);
  };
  draw();

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    // The main loop does not run during this modal screen. This loop must service the auto-off timeout manually. It prevents the timeout from piling up.
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    else if (t.pressed && uiTouchInButton(t, neverBtn)) { uiWaitForRelease(); never = !never; draw(); }
    else if (t.pressed && !never && uiTouchInButton(t, timeoutBtn)) {
      uiWaitForRelease();
      idx = uiDropdownPick("Timeout", kTimeoutOptionCount, timeoutItemLabel, idx);
      uiDrawTopBar("Display Timeout");
      draw();
    }
    else if (t.pressed && uiTouchInButton(t, applyBtn)) {
      uiWaitForRelease();
      powerDisplayTimeoutSave(never ? 0 : kTimeoutOptionsMs[idx]);
      return;
    }
    delay(15);
  }
}

// Light sleep wakes every 50 milliseconds. It redraws the LED heartbeat. It returns to sleep immediately. Full wake occurs only on touch.
static void sleepMode() {
  bool wasArmed = engagementIsArmed();
  panelAsleep(true);

  pinMode(TP_IRQ, INPUT);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TP_IRQ, 0);   // wake on LOW (touch)

  uint32_t heartbeatStart = millis();
  bool woke = false;
  while (!woke) {
    uint32_t t = (millis() - heartbeatStart) % 3050;
    bool on = (t < 50) || (wasArmed && t >= 100 && t < 150);
    ledHeartbeat(on);

    esp_sleep_enable_timer_wakeup(50000);   // 50ms, in microseconds
    esp_light_sleep_start();

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 && realTouchPresent() && wakeGestureChallenge())
      woke = true;
    // The loop handles timer ticks and false wake events. It continues until a valid touch occurs.
  }

  ledHeartbeat(false);
  // Timer wakeup stays armed across sleep calls. It would trigger deep sleep instantly. The code disables it here.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  panelAsleep(false);
  uiWaitForRelease();
  s_pendingWake = true;
}

// Deep sleep wakes on the EN pin. The RESET button connects to this pin. Hardware triggers a full chip reset. No software wake source requires configuration.
static void deepSleepMode() {
  panelAsleep(true);
  ledColorRGB(0, 0, 0);   // no strobe in this tier, by design -- see header comment
  WiFi.mode(WIFI_OFF);
  btStop();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);   // defensive -- see sleepMode()'s own comment
  esp_deep_sleep_start();
}

// This popup overlays the current screen. The caller redraws its interface afterward. It erases the popup automatically.
void powerShowMenu() {
  int boxW = tft.width() - 60; if (boxW > 220) boxW = 220;
  const int boxH = 110;
  int bx = (tft.width() - boxW) / 2, by = (tft.height() - boxH) / 2;

  tft.fillRect(bx, by, boxW, boxH, ILI9341_BLACK);
  tft.drawRect(bx, by, boxW, boxH, ILI9341_WHITE);
  Btn sleepBtn = {bx + 10, by + 12, boxW - 20, 34, "Sleep"};
  Btn offBtn   = {bx + 10, by + 56, boxW - 20, 34, "Power off"};
  uiDrawMenuButton(sleepBtn);
  uiDrawMenuButton(offBtn);
  {
    const char *msg = "tap outside to cancel";
    int16_t tbx, tby; uint16_t tbw, tbh;
    tft.setTextSize(1);
    tft.getTextBounds(msg, 0, 0, &tbx, &tby, &tbw, &tbh);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(bx + (boxW - (int)tbw) / 2 - tbx, by + boxH - 14);
    tft.print(msg);
  }

  for (;;) {
    TouchPoint t = uiReadTouch();
    uiServiceChrome();
    // This modal screen blocks the main loop. It must service the auto-off timeout manually. It prevents silent timeout pile-up.
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInButton(t, sleepBtn)) { uiWaitForRelease(); sleepMode(); return; }
    if (uiTouchInButton(t, offBtn)) {
      uiWaitForRelease();
      // Deep sleep blanks the screen immediately. The code shows a confirmation dialog first. It prevents user confusion.
      uiClearBelow(0);
      tft.setTextColor(ILI9341_YELLOW);
      tft.setTextSize(2);
      tft.setCursor(8, 40);
      tft.print("Power off");
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(1);
      tft.setCursor(8, 76);
      tft.print("Lowest power mode.");
      tft.setCursor(8, 92);
      tft.print("Press RESET to power on.");
      Btn go  = {8, 140, tft.width() - 16, 34, "Power off now"};
      Btn no  = {8, 184, tft.width() - 16, 34, "cancel"};
      uiDrawMenuButton(go);
      uiDrawMenuButton(no);
      for (;;) {
        TouchPoint t2 = uiReadTouch();
        if (t2.pressed) powerNoteActivity();
        powerServiceAutoOff();   // Service the auto-off timeout manually.
        if (!t2.pressed) { delay(15); continue; }
        if (uiTouchInButton(t2, no) || uiTouchInBackButton(t2)) { uiWaitForRelease(); break; }
        if (uiTouchInButton(t2, go)) { uiWaitForRelease(); deepSleepMode(); }   // never returns
      }
      return;
    }
    bool inBox = (t.x >= bx && t.x < bx + boxW && t.y >= by && t.y < by + boxH);
    if (!inBox) { uiWaitForRelease(); return; }   // tapped outside the box -- dismiss, no action
    delay(15);
  }
}
