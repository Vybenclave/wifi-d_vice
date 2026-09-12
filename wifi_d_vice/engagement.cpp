#include "engagement.h"
#include <WiFi.h>
#include "ui.h"
#include "keyboard.h"
#include "crypto.h"
#include "ble_2fa.h"
#include "devtime.h"
#include "engstore.h"
#include "webdl.h"
#include "theme.h"

static Engagement eng;
static String passphrase;
static String s_status;     // one-shot line shown at the bottom on the next draw

const Engagement &engagementGet() { return eng; }
bool engagementIsArmed() { return eng.armed; }

String engagementHeaderLine() {
  return "client=" + eng.client + ";tester=" + eng.tester +
         ";armed_at=" + devTimeNowString();
}

// ---- small boot/validate helpers (also used by the in-screen flows) ----
static void bootCenter(uint16_t color, uint8_t size, const char *s, int y) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  int16_t bx, by; uint16_t w, h;
  tft.getTextBounds(s, 0, 0, &bx, &by, &w, &h);
  tft.setCursor((tft.width() - (int)w) / 2 - bx, y);
  tft.print(s);
}

// progress bar for the PBKDF2 grind, plus a green blink so "Validating..."
// is obvious on the LED too.
static void kdfBar(int pct) {
  int w = tft.width() - 60, x = 30, y = tft.height() / 2 + 12;
  tft.drawRect(x, y, w, 12, ILI9341_CYAN);
  tft.fillRect(x + 2, y + 2, (w - 4) * pct / 100, 8, ILI9341_CYAN);
  ledGreen((millis() % 75) < 50);
}

// Derive the key from `pp` against the active client's stored salt, showing a
// "Validating..." screen + progress bar. Returns with a key loaded.
static void deriveKey(const String &pp, const char *title) {
  uiClearBelow(0);
  bootCenter(thLabel(), 2, title, tft.height() / 2 - 16);
  cryptoSetPassphraseEx(pp, engStoreSalt(), (uint32_t)engStoreIters(), kdfBar);
  ledGreen(false);
}

// ---- screen ----
static Btn pairBtn;
static Btn clientBtn, clientPlusBtn;   // client selector row + its inline "+"
static Btn testerBtn;
static Btn passBtn;
static Btn exportBtn;
static Btn webBtn;
static Btn armBtn, clearBtn;
static bool exportOn = false;

static bool allFieldsFilled() {
  return eng.client.length() && eng.tester.length() && cryptoHasKey();
}

static void drawRow(const Btn &b, const char *label, const String &value) {
  tft.drawRect(b.x, b.y, b.w, b.h, ILI9341_WHITE);
  tft.setTextWrap(false);
  tft.setTextColor(thLabel());
  tft.setCursor(b.x + 2, b.y + (b.h - 8) / 2);      // vertically centred, like every other row
  tft.print(label);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(b.x + 90, b.y + (b.h - 8) / 2);
  String shown = value.length() ? value : "<tap to set>";
  int maxChars = (b.w - 90 - 4) / 6;
  if ((int)shown.length() > maxChars && maxChars > 3) shown = shown.substring(0, maxChars - 3) + "...";
  tft.print(shown);
}

static const int ROW_H = 26, ROW_GAP = 4;

static void publishExport() {
  if (!exportOn) { ble2faSetExport("(BLE export off)"); return; }
  String s = "client=" + eng.client + "\n";
  s += "tester=" + eng.tester + "\n";
  s += "armed=" + String(eng.armed ? "yes" : "no") + "\n";
  s += "armed_at=" + engStoreArmedAt() + "\n";
  s += "clock=" + devTimeNowString() + "\n";
  s += "wifi_profiles=" + String(engStoreWifiCount()) + "\n";
  ble2faSetExport(s);
}

static void drawClientRow(int y) {
  clientBtn = {4, y, tft.width() - 8, ROW_H, "Client"};
  tft.drawRect(clientBtn.x, clientBtn.y, clientBtn.w, clientBtn.h, ILI9341_WHITE);
  tft.setTextWrap(false);
  tft.setTextColor(thLabel());
  tft.setCursor(clientBtn.x + 2, clientBtn.y + (ROW_H - 8) / 2);
  tft.print("Client");

  // inline "+" (far right) and a down-caret just left of it
  clientPlusBtn = {clientBtn.x + clientBtn.w - 22, clientBtn.y, 22, ROW_H, "+"};
  tft.drawRect(clientPlusBtn.x, clientPlusBtn.y, clientPlusBtn.w, clientPlusBtn.h, ILI9341_WHITE);
  tft.setTextColor(ILI9341_GREEN);
  tft.setCursor(clientPlusBtn.x + 7, clientPlusBtn.y + (ROW_H - 8) / 2);
  tft.print("+");
  int cx = clientPlusBtn.x - 12, cy = clientBtn.y + ROW_H / 2;
  tft.fillTriangle(cx - 5, cy - 3, cx + 5, cy - 3, cx, cy + 4, ILI9341_CYAN);

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(clientBtn.x + 90, clientBtn.y + (ROW_H - 8) / 2);
  String shown = eng.client.length() ? eng.client : String("<tap to pick>");
  int maxChars = (clientBtn.w - 90 - 26) / 6;
  if ((int)shown.length() > maxChars && maxChars > 3) shown = shown.substring(0, maxChars - 3) + "...";
  tft.print(shown);
  if (eng.client.length() && cryptoHasKey()) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(clientBtn.x + 90 + (int)shown.length() * 6 + 6, clientBtn.y + (ROW_H - 8) / 2);
    tft.print(engStoreHasVerifier() ? "unlocked" : "new");
  }
}

static void drawFields() {
  uiClearBelow(29);
  tft.setTextSize(1);
  int y = 34;

  pairBtn = {4, y, tft.width() - 8, ROW_H, "BLE Pair"};
  tft.drawRect(pairBtn.x, pairBtn.y, pairBtn.w, pairBtn.h, ILI9341_WHITE);
  tft.setTextColor(thLabel());
  tft.setCursor(pairBtn.x + 2, pairBtn.y + (ROW_H - 8) / 2);
  tft.print("BLE Pair");
  bool live = ble2faBondedDeviceConnected();
  bool stored = ble2faHasStoredBond();
  tft.setTextColor((live || stored) ? ILI9341_GREEN : ILI9341_YELLOW);
  tft.setCursor(pairBtn.x + 90, pairBtn.y + (ROW_H - 8) / 2);
  tft.print(live ? "paired" : (stored ? "bond stored" : "not paired"));
  y += ROW_H + ROW_GAP;

  drawClientRow(y);
  y += ROW_H + ROW_GAP;

  testerBtn = {4, y, tft.width() - 8, ROW_H, "Tester"};
  drawRow(testerBtn, "Tester", eng.tester);
  y += ROW_H + ROW_GAP;

  passBtn = {4, y, tft.width() - 8, ROW_H, "Passphrase"};
  {
    const char *pv = !eng.client.length() ? "" :
                     cryptoHasKey()        ? "(set)" :
                     engStoreHasVerifier() ? "(locked - tap)" : "";
    drawRow(passBtn, "Passphrase", String(pv));
  }
  y += ROW_H + ROW_GAP;

  exportBtn = {4, y, tft.width() - 8, ROW_H, "BLE export"};
  tft.drawRect(exportBtn.x, exportBtn.y, exportBtn.w, exportBtn.h, ILI9341_WHITE);
  tft.setTextColor(thLabel());
  tft.setCursor(exportBtn.x + 2, exportBtn.y + (ROW_H - 8) / 2);
  tft.print("Export BLE");
  tft.setTextColor(exportOn ? ILI9341_GREEN : ILI9341_YELLOW);
  tft.setCursor(exportBtn.x + 90, exportBtn.y + (ROW_H - 8) / 2);
  tft.print(exportOn ? "ON (bonded read)" : "OFF");
  y += ROW_H + ROW_GAP;

  webBtn = {4, y, tft.width() - 8, ROW_H, "WiFi download"};
  tft.drawRect(webBtn.x, webBtn.y, webBtn.w, webBtn.h, ILI9341_WHITE);
  tft.setTextColor(thLabel());
  tft.setCursor(webBtn.x + 2, webBtn.y + (ROW_H - 8) / 2);
  tft.print("WiFi download");
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(webBtn.x + 90, webBtn.y + (ROW_H - 8) / 2);
  tft.print("SoftAP + HTTP");
  y += ROW_H + ROW_GAP;

  const int armGap = 8;
  int rowY = y + armGap;
  int totalW = tft.width() - 8;
  int armW = (totalW * 62) / 100;
  armBtn  = {4, rowY, armW, ROW_H, eng.armed ? "DISARM" : "ARM"};
  clearBtn = {4 + armW + 6, rowY, totalW - armW - 6, ROW_H, "Clear data"};
  uiDrawMenuButton(armBtn);
  uiDrawMenuButton(clearBtn);
  tft.setTextColor(eng.armed ? ILI9341_GREEN : ILI9341_WHITE);
  tft.setCursor(4, rowY + ROW_H + 8);
  tft.printf("%s  clock: %s", eng.armed ? "ARMED " : "", devTimeNowString().c_str());

  if (s_status.length()) { uiToast(s_status.c_str()); s_status = ""; }
  else uiToast("passphrase doubles as the SD encryption key");
}

// ---- client selection flows ----
// Switch to `name` (existing folder), drop any current key, prompt once and
// validate against the stored verifier.
static void selectExistingClient(const String &name) {
  engStoreSelectClient(name);
  eng.client = engStoreCurrentClient();
  eng.tester = engStoreTester();
  cryptoClearKey();
  passphrase = "";
  eng.armed = false;

  String pp = uiTextInput(("Passphrase - " + eng.client).c_str(), "", true);
  if (pp.length() == 0) { s_status = eng.client + ": locked (no passphrase)"; return; }

  deriveKey(pp, "Validating...");
  if (engStoreHasVerifier()) {
    if (engStoreUnlockCheck()) { passphrase = pp; s_status = eng.client + ": unlocked"; }
    else { cryptoClearKey(); s_status = "wrong passphrase for " + eng.client; }
  } else {
    passphrase = pp;   // folder exists but was never armed -> verifier written on ARM
    s_status = eng.client + ": passphrase set (not yet armed)";
  }
}

// "+ new client": name it, create the folder, set a fresh passphrase (twice).
static void newClient() {
  String nm = uiTextInput("New client name", "", false);
  nm.trim();
  if (nm.length() == 0) return;
  if (engStoreClientExists(nm)) { selectExistingClient(nm); return; }

  engStoreSelectClient(nm);          // creates /<nm>/
  eng.client = engStoreCurrentClient();
  eng.tester = "";
  cryptoClearKey();
  passphrase = "";
  eng.armed = false;

  String p1 = uiTextInput("Set passphrase", "", true);
  String p2 = uiTextInput("Confirm passphrase", "", true);
  if (p1.length() && p1 == p2) {
    cryptoSetPassphrase(p1, engStoreSalt());
    passphrase = p1;
    s_status = eng.client + ": new - set Tester, then ARM";
  } else {
    s_status = eng.client + ": passphrase not set";
  }
}

static void clientPicker() {
  String names[16];
  int n = engStoreListClients(names, 16);

  uiDrawTopBar("Select client");
  uiClearBelow(29);
  tft.setTextSize(1);
  tft.setTextColor(thLabel());
  tft.setCursor(6, 33);
  tft.print(n ? "Client folders on the card:" : "No client folders yet.");

  Btn rows[16];
  int shown = 0, y = 48;
  for (int i = 0; i < n && y + 26 <= tft.height() - 34; i++) {
    rows[i] = {6, y, tft.width() - 12, 24, names[i].c_str()};
    uiDrawMenuButton(rows[i]);
    y += 28;
    shown++;
  }
  Btn newBtn = {6, tft.height() - 30, tft.width() - 12, 26, "+ new client"};
  uiDrawMenuButton(newBtn);

  while (true) {
    TouchPoint t = uiReadTouch();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    if (uiTouchInButton(t, newBtn)) { uiWaitForRelease(); newClient(); return; }
    for (int i = 0; i < shown; i++) {
      if (uiTouchInButton(t, rows[i])) { uiWaitForRelease(); selectExistingClient(names[i]); return; }
    }
  }
}

// ---- "Clear data" (delete the whole client folder) ----
static void clearDataConfirm() {
  if (eng.client.length() == 0) { s_status = "no client selected"; return; }
  String c = eng.client;

  uiClearBelow(0);
  tft.setTextColor(ILI9341_RED);
  tft.setTextSize(2);
  tft.setCursor(8, 34);
  tft.print("Clear data");
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 72);
  tft.printf("Delete /%s and every file", c.c_str());
  tft.setCursor(8, 88);
  tft.print("in it: logs, saved Wi-Fi, the");
  tft.setCursor(8, 104);
  tft.print("passphrase verifier. No undo.");
  Btn yes = {8, 132, tft.width() - 16, 34, "type WIPE to confirm"};
  Btn no  = {8, 176, tft.width() - 16, 34, "cancel"};
  uiDrawMenuButton(yes);
  uiDrawMenuButton(no);

  while (true) {
    TouchPoint t = uiReadTouch();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInButton(t, no) || uiTouchInBackButton(t)) { uiWaitForRelease(); s_status = "clear cancelled"; return; }
    if (uiTouchInButton(t, yes)) {
      uiWaitForRelease();
      String typed = uiTextInput("Type WIPE to confirm", "", false);
      if (typed == "WIPE") {
        engStoreDeleteClient(c);
        engagementResetAll();
        s_status = c + ": data cleared";
      } else {
        s_status = "not cleared (WIPE not typed)";
      }
      return;
    }
  }
}

void engagementEnter() {
  WiFi.disconnect(true, false);   // radio coexistence -- see README
  WiFi.mode(WIFI_OFF);
  delay(50);
  ble2faBegin();
  if (eng.client.length() == 0) {           // first visit this boot
    eng.client = engStoreCurrentClient();
    eng.tester = engStoreTester();
  }
  publishExport();
  uiDrawTopBar("Engagement");
  drawFields();
}

void engagementLoop() {}

static bool waitForSecondFactor() { return ble2faPairAndWait("cancel"); }

void engagementTouch(const TouchPoint &t) {
  if (uiTouchInButton(t, pairBtn)) {
    waitForSecondFactor();
    uiDrawTopBar("Engagement");
    drawFields();
    uiWaitForRelease();
    return;
  }

  if (uiTouchInButton(t, clientPlusBtn)) {
    uiWaitForRelease();
    newClient();
    uiDrawTopBar("Engagement");
    drawFields();
    return;
  }
  if (uiTouchInButton(t, clientBtn)) {
    uiWaitForRelease();
    clientPicker();
    uiDrawTopBar("Engagement");
    drawFields();
    return;
  }

  if (uiTouchInButton(t, exportBtn)) {
    exportOn = !exportOn;
    publishExport();
    drawFields();
    uiWaitForRelease();
    return;
  }

  if (uiTouchInButton(t, webBtn)) {
    uiWaitForRelease();
    webDownloadRun();
    ble2faBegin();
    uiDrawTopBar("Engagement");
    drawFields();
    return;
  }

  if (uiTouchInButton(t, clearBtn)) {
    uiWaitForRelease();
    clearDataConfirm();
    uiDrawTopBar("Engagement");
    drawFields();
    return;
  }

  if (uiTouchInButton(t, armBtn)) {
    if (eng.armed) {
      // Disarm: wipe the RAM key and clear the boot-unlock flag so the next
      // boot goes straight in. The folder + verifier + logs all stay.
      eng.armed = false;
      engStoreMarkSuspended();
      cryptoClearKey();
      passphrase = "";
      s_status = "disarmed (no prompt at next boot)";
      drawFields();
      uiWaitForRelease();
      return;
    }
    if (!ble2faBondedDeviceConnected() && !ble2faHasStoredBond()) {
      s_status = "pair a BLE second factor first";
      drawFields();
      uiWaitForRelease();
      return;
    }
    if (!allFieldsFilled()) {
      s_status = "need client + tester + passphrase";
      drawFields();
      uiWaitForRelease();
      return;
    }
    eng.armed = true;
    engStoreMarkStarted(eng.client, eng.tester, devTimeNowString());
    publishExport();
    s_status = "ARMED";
    drawFields();
    uiWaitForRelease();
    return;
  }

  if (uiTouchInButton(t, passBtn)) {
    if (eng.client.length() == 0) {
      s_status = "pick a client first";
    } else if (engStoreHasVerifier()) {
      String pp = uiTextInput(("Passphrase - " + eng.client).c_str(), "", true);
      if (pp.length()) {
        deriveKey(pp, "Validating...");
        if (engStoreUnlockCheck()) { passphrase = pp; s_status = "unlocked"; }
        else { cryptoClearKey(); passphrase = ""; s_status = "wrong passphrase"; }
      }
    } else {
      String p1 = uiTextInput("Set passphrase", "", true);
      String p2 = uiTextInput("Confirm passphrase", "", true);
      if (p1.length() && p1 == p2) {
        cryptoSetPassphrase(p1, engStoreSalt());
        passphrase = p1;
        s_status = "passphrase set";
      }
    }
    uiDrawTopBar("Engagement");
    drawFields();
    uiWaitForRelease();
    return;
  }

  if (uiTouchInButton(t, testerBtn)) {
    eng.tester = uiTextInput("Tester", eng.tester, false);
    uiDrawTopBar("Engagement");
    drawFields();
    uiWaitForRelease();
    return;
  }
}

void engagementExit() { /* arming persists across screens until explicitly
                           disarmed or the device reboots */ }

// Terminal state after too many wrong passphrases.
static void deviceHalt() {
  cryptoClearKey();
  ledSet(false);
  WiFi.mode(WIFI_OFF);
  btStop();
  uiClearBelow(0);
  bootCenter(ILI9341_RED, 3, "Device Halted", tft.height() / 2 - 24);
  bootCenter(ILI9341_WHITE, 1, "Reset the device to continue.", tft.height() / 2 + 16);
  for (;;) delay(1000);
}

static void blinkRedFor(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    ledSet((millis() % 75) < 50);
    delay(5);
  }
  ledSet(false);
}

void engagementBootUnlock() {
  if (!engStoreStarted()) return;   // disarmed or no engagement -> boot straight in

  eng.client = engStoreClient();
  eng.tester = engStoreTester();

  const int MAX_TRIES = 5;
  int used = 0;
  while (true) {
    String pp = uiTextInput("Engagement locked - passphrase", "", true);
    if (pp.length() == 0) continue;

    deriveKey(pp, "Validating...");
    if (engStoreUnlockCheck()) {
      passphrase = pp;
      eng.armed = true;
      uiClearBelow(0);
      bootCenter(ILI9341_GREEN, 2, "Decrypting...", tft.height() / 2 - 8);
      blinkRedFor(900);
      return;
    }

    cryptoClearKey();
    if (++used >= MAX_TRIES) deviceHalt();

    uiClearBelow(0);
    bootCenter(ILI9341_RED, 2, "Incorrect Passphrase", 52);
    Btn again = {tft.width() / 2 - 70, 100, 140, 36, "Try Again"};
    uiDrawMenuButton(again);
    char rem[28];
    snprintf(rem, sizeof(rem), "tries remaining: %d", MAX_TRIES - used);
    bootCenter(ILI9341_WHITE, 1, rem, 152);
    for (;;) {
      TouchPoint t = uiReadTouch();
      if (t.pressed && uiTouchInButton(t, again)) { uiWaitForRelease(); break; }
      delay(15);
    }
  }
}

void engagementResetAll() {
  eng.client = "";
  eng.tester = "";
  eng.armed = false;
  passphrase = "";
  s_status = "";
  cryptoClearKey();
}
