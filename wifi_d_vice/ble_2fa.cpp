#include "ble_2fa.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <esp_gap_ble_api.h>
#include "ui.h"
#include "accent.h"
#include "debuglog.h"
#include "power.h"

static volatile bool bonded = false;
static volatile bool passkeyPending = false;
static volatile uint32_t shownPasskey = 0;
static BLEServer *pServer = nullptr;
static BLECharacteristic *exportCh = nullptr;

// Text lines stay short. They use textSize 1. Hardcoded Y positions assume
// a longer line at textSize 2. That layout overflows the 320px screen.
// Adafruit_GFX auto-wraps by default. The wrapped text collides with the
// next line. setTextWrap(false) clips long strings. It prevents silent
// wrapping.
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
  tft.setTextColor(accentLabel());
  tft.setCursor(50, 130);
  tft.printf("%06lu", (unsigned long)pass_key);
}

// Rapid screen alternation suggests repeated pairing failures. These prints
// identify each callback. They show the exact failure reason code. This
// avoids guessing from photos.
class Sec2FA : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    DLOG("ble2fa", "onPassKeyRequest (unexpected -- we're display-only)");
    return 0;
  }
  void onPassKeyNotify(uint32_t pass_key) override {
    // This callback sets state only. It never draws. The BLE task runs
    // separately from the main loop. Two tasks writing to the SPI display can
    // corrupt the render. The main loop owns all drawing for this screen.
    DLOG("ble2fa", "onPassKeyNotify: %06lu", (unsigned long)pass_key);
    shownPasskey = pass_key;
    passkeyPending = true;
  }
  bool onSecurityRequest() override {
    DLOG("ble2fa", "onSecurityRequest");
    return true;
  }
  bool onConfirmPIN(uint32_t pin) override {
    DLOG("ble2fa", "onConfirmPIN: %lu", (unsigned long)pin);
    return true;
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    DLOG("ble2fa", "onAuthenticationComplete: success=%d fail_reason=%d auth_mode=%d",
         cmpl.success, cmpl.fail_reason, cmpl.auth_mode);
    passkeyPending = false;
    bonded = cmpl.success;
    if (cmpl.success) { ledConnected(true); beep(120, 1800); }
  }
};

class Srv2FA : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    (void)s;
    DLOG("ble2fa", "onConnect");
  }
  void onDisconnect(BLEServer *s) override {
    (void)s;
    DLOG("ble2fa", "onDisconnect -- restarting advertising");
    bonded = false;
    ledConnected(false);
    BLEDevice::startAdvertising();
  }
};

void ble2faBegin() {
  // This function is idempotent. It handles teardowns from webdl.cpp. The
  // deinit call frees RAM for a SoftAP. The old BLE objects dangle after
  // deinit. We rebuild them fresh. They leak hundreds of bytes. Webdl uses
  // this flow rarely.
  if (BLEDevice::getInitialized()) return;

  BLEDevice::init("CYD-Engagement-2FA");
  BLEDevice::setSecurityCallbacks(new Sec2FA());

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new Srv2FA());

  BLEService *svc = pServer->createService("6e400001-0000-1000-8000-00805f9b34fb");
  BLECharacteristic *ch = svc->createCharacteristic(
      "6e400002-0000-1000-8000-00805f9b34fb", BLECharacteristic::PROPERTY_READ);
  ch->setValue("armed-gate");
  // This setting forces authenticated pairing. It requires a passkey on first
  // read. It triggers the native pairing prompt. It prevents plain
  // unauthenticated connections.
  ch->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);

  // This characteristic exports engagement data. It reads only over an
  // encrypted link.
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
  // The 128-bit UUID uses 16 of 31 advertising bytes. It pushes the device
  // name out of the primary packet. We place the name in the scan response
  // packet. The preferred interval settings fix iOS connection issues. They
  // ensure reliable pairing with custom peripherals.
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  adv->start();
}

void ble2faEnd() {
  if (!BLEDevice::getInitialized()) return;
  BLEDevice::deinit(false);
  // These pointers dangle after teardown. ble2faBegin() rebuilds fresh
  // objects. It avoids leaking memory.
  pServer = nullptr;
  exportCh = nullptr;
  bonded = false;
  passkeyPending = false;
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
  // Bluedroid stores bonds in NVS. The bond count reads before the stack
  // starts. It works on every boot.
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
  // The UI redraws only on state transitions. It skips every 50ms tick. The
  // main loop handles all drawing. The BLE task sets state only. It no
  // longer draws directly. Direct drawing races with the main loop. It
  // corrupts the render.
  bool instructionsShown = false;
  bool passkeyShown = false;
  uint32_t lastPasskeyDrawn = 0;
  while (true) {
    if (ble2faBondedDeviceConnected()) return true;
    if (!ble2faPasskeyPending()) {
      passkeyShown = false;
      if (!instructionsShown) {
        DLOG("ble2fa", "showing pairing instructions");
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
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && uiTouchInButton(t, skipBtn)) { uiWaitForRelease(); return false; }
    delay(50);
  }
}
