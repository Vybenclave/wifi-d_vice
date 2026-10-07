#pragma once
#include <Arduino.h>
// Engagement Page second factor. The ESP32 shows a random passkey.
// The operator types it into the host pairing dialog. The system uses native BLE crypto.

void ble2faBegin();                 // idempotent
// Tear down the BLE server. The function frees memory. NVS bonds survive this call.
void ble2faEnd();
bool ble2faBondedDeviceConnected();
bool ble2faPasskeyPending();
uint32_t ble2faCurrentPasskey();

// Bonds live in NVS. The device reconnects without a new passkey.
// The SD wipe path clears all bonds.
bool ble2faHasStoredBond();
void ble2faClearBonds();

// Export engagement data over GATT. The characteristic requires encryption.
// The Engagement screen toggles this feature.
void ble2faSetExport(const String &text);

// Begin advertising and wait for pairing. The function blocks until paired.
// The Engagement Page and first-boot wizard share this flow.
bool ble2faPairAndWait(const char *skipLabel);
