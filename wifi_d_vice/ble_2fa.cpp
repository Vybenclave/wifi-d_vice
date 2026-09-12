#include "ble_2fa.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <esp_gap_ble_api.h>
#include "ui.h"
#include "theme.h"

static volatile bool bonded = false;
static volatile bool passkeyPending = false;
static volatile uint32_t shownPasskey = 0;
static BLEServer *pServer = nullptr;
static BLECharacteristic *exportCh = nullptr;

// Text lines here are deliberately short and at textSize 1, not laid out
// with hardcoded Y positions assuming a longer line at textSize 2 -- that
// was confirmed on hardware to overflow even the 320px-wide orientation
// (Adafruit_GFX auto-wraps by default, and the auto-wrapped remainder
// collided with the next manually-positioned line, corrupting the layout).
// setTextWrap(false) is a defensive backstop so a too-long string gets
// clipped instead of silently wrapping into whatever's drawn next.
static void drawPasskey(uint32_t pass_key) {
  uiClearBelow(0);
  tft.setTextWrap(false);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 60);
  tft.print("Enter this code on your");
  tft.setCursor(10, 74);
  tft.print("phone/laptop's Bluetooth");
  tft.setCursor(10, 88);
  tft.print("pairing prompt:");
  tft.setTextSize(4);
  tft.setTextColor(thLabel());
  tft.setCursor(50, 130);
  tft.printf("%06lu", (unsigned long)pass_key);
}

// Diagnostic logging -- a rapid "passkey screen <-> instructions screen"
// alternation reported on real hardware suggests pairing is repeatedly
// failing and retrying, not a simple redraw bug. These prints identify
// exactly which callback fires, how often, and (for failures) the real SMP
// failure reason code instead of guessing from photos.
class Sec2FA : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    Serial.println("[ble2fa] onPassKeyRequest (unexpected -- we're display-only)");
    return 0;
  }
  void onPassKeyNotify(uint32_t pass_key) override {
    // State only -- NOT drawing here. This callback fires from the BLE/
    // Bluedroid task, a different FreeRTOS task than the main Arduino
    // loop. ble2faPairAndWait()'s loop (main task) was ALSO drawing the
    // skip button unconditionally every ~50ms; two tasks writing to the
    // same SPI display with no synchronization can genuinely interleave
    // and corrupt the render (confirmed report: passkey text overlapping
    // the cancel button in a way the coordinates alone don't explain).
    // The main loop now owns all drawing for this screen; see
    // ble2faPairAndWait().
    Serial.printf("[ble2fa] onPassKeyNotify: %06lu\n", (unsigned long)pass_key);
    shownPasskey = pass_key;
    passkeyPending = true;
  }
  bool onSecurityRequest() override {
    Serial.println("[ble2fa] onSecurityRequest");
    return true;
  }
  bool onConfirmPIN(uint32_t pin) override {
    Serial.printf("[ble2fa] onConfirmPIN: %lu\n", (unsigned long)pin);
    return true;
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    Serial.printf("[ble2fa] onAuthenticationComplete: success=%d fail_reason=%d auth_mode=%d\n",
                  cmpl.success, cmpl.fail_reason, cmpl.auth_mode);
    passkeyPending = false;
    bonded = cmpl.success;
    if (cmpl.success) { ledSet(true); beep(120, 1800); }
  }
};

// The BLE Current Time Service was removed: phone OSes don't reliably
// auto-write CTS to a bare custom GATT peripheral (unlike the passkey
// pairing itself), so the clock stayed "unsynced" after every pair. Time
// now comes only from SNTP once WiFi is connected (see devtime.cpp).

class Srv2FA : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    (void)s;
    Serial.println("[ble2fa] onConnect");
  }
  void onDisconnect(BLEServer *s) override {
    (void)s;
    Serial.println("[ble2fa] onDisconnect -- restarting advertising");
    bonded = false;
    ledSet(false);
    BLEDevice::startAdvertising();
  }
};

void ble2faBegin() {
  // Idempotent, and re-entrant after a teardown: webdl.cpp does
  // BLEDevice::deinit() to free RAM for its SoftAP, then calls this again.
  // getInitialized() is the real signal (not a one-shot flag). After a
  // deinit the old BLEServer / characteristic C++ objects dangle -- we
  // rebuild fresh and they leak (~hundreds of bytes; webdl is a rare flow).
  if (BLEDevice::getInitialized()) return;

  BLEDevice::init("CYD-Engagement-2FA");
  BLEDevice::setSecurityCallbacks(new Sec2FA());

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new Srv2FA());

  BLEService *svc = pServer->createService("6e400001-0000-1000-8000-00805f9b34fb");
  BLECharacteristic *ch = svc->createCharacteristic(
      "6e400002-0000-1000-8000-00805f9b34fb", BLECharacteristic::PROPERTY_READ);
  ch->setValue("armed-gate");
  // Forces the OS to complete authenticated (MITM-protected, i.e. passkey-
  // verified) pairing on first read -- this is what actually triggers the
  // native pairing prompt rather than a plain unauthenticated connection.
  ch->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);

  // Engagement-data export -- readable only over an encrypted (bonded) link.
  exportCh = svc->createCharacteristic(
      "6e400003-0000-1000-8000-00805f9b34fb", BLECharacteristic::PROPERTY_READ);
  exportCh->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  exportCh->setValue("(BLE export off)");

  svc->start();

  BLESecurity *sec = new BLESecurity();
  sec->setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  sec->setCapability(ESP_IO_CAP_OUT);   // display-only: we show it, they type it
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::regenPassKeyOnConnect(true);   // fresh random code per pairing attempt

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(svc->getUUID());
  // Our 128-bit service UUID alone eats 16 of the 31 advertising-packet
  // bytes, which can push the device name out of the primary advertisement
  // entirely -- put the name in the scan response packet instead so it's
  // still there. setMinPreferred/setMaxPreferred are the standard
  // ESP32-BLE-Arduino workaround for phones (notably iOS) that otherwise
  // don't reliably show/connect to a bare custom peripheral.
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  adv->start();
}

bool ble2faBondedDeviceConnected() { return bonded; }
bool ble2faPasskeyPending() { return passkeyPending; }
uint32_t ble2faCurrentPasskey() { return shownPasskey; }

void ble2faSetExport(const String &text) {
  if (!exportCh) return;
  String v = text;
  if (v.length() > 500) v = v.substring(0, 500) + "\n...(truncated)";
  exportCh->setValue(v.c_str());
}

bool ble2faHasStoredBond() {
  // Bonds are kept in NVS by Bluedroid; the count is readable even before
  // ble2faBegin() brings the stack up on this boot.
  return esp_ble_get_bond_device_num() > 0;
}

void ble2faClearBonds() {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return;
  esp_ble_bond_dev_t list[8];
  if (n > 8) n = 8;
  if (esp_ble_get_bond_device_list(&n, list) != ESP_OK) return;
  for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  bonded = false;
}

bool ble2faPairAndWait(const char *skipLabel) {
  ble2faBegin();
  Btn skipBtn = {tft.width() / 2 - 60, tft.height() - 40, 120, 30, skipLabel};
  // Both "instructions shown" and "passkey shown" only redraw on an actual
  // state transition, not every ~50ms tick. All drawing for this screen
  // happens HERE, in the main loop -- onPassKeyNotify() (BLE task) only
  // sets state now, it used to draw directly from that other task, racing
  // against this loop's own tft writes and corrupting the render (confirmed
  // report: passkey text overlapping the cancel button in a way the
  // coordinates alone didn't explain).
  bool instructionsShown = false;
  bool passkeyShown = false;
  uint32_t lastPasskeyDrawn = 0;
  while (true) {
    if (ble2faBondedDeviceConnected()) return true;
    if (!ble2faPasskeyPending()) {
      passkeyShown = false;
      if (!instructionsShown) {
        Serial.println("[ble2fa] showing pairing instructions");
        uiClearBelow(0);
        tft.setTextWrap(false);   // a too-long line clips instead of silently
                                  // wrapping into the next manually-positioned
                                  // line -- see drawPasskey()'s comment for why
        tft.setTextColor(ILI9341_WHITE);
        tft.setTextSize(1);
        tft.setCursor(10, 90);
        tft.print("Pair from your phone/");
        tft.setCursor(10, 104);
        tft.print("laptop's Bluetooth");
        tft.setCursor(10, 118);
        tft.print("settings:");
        tft.setCursor(10, 140);
        tft.print("\"CYD-Engagement-2FA\"");
        instructionsShown = true;
      }
    } else {
      instructionsShown = false;
      uint32_t key = ble2faCurrentPasskey();
      if (!passkeyShown || key != lastPasskeyDrawn) {
        drawPasskey(key);
        passkeyShown = true;
        lastPasskeyDrawn = key;
      }
    }
    uiDrawButton(skipBtn);   // always visible/tappable, on either screen
    TouchPoint t = uiReadTouch();
    if (t.pressed && uiTouchInButton(t, skipBtn)) { uiWaitForRelease(); return false; }
    delay(50);
  }
}
