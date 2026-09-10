#pragma once
#include <Arduino.h>
// Engagement Page second factor: native BLE Secure Connections "Passkey
// Entry" pairing + bonding, instead of a custom challenge-response scheme.
// The ESP32 (Display-Only IO capability) shows a random 6-digit passkey on
// its own screen; the operator's phone/laptop types it into their OWN
// system Bluetooth pairing dialog -- no companion app or script needed.
// Real ECDH-based Secure Connections crypto, not hand-rolled.
//
// Unverified: whether a given phone OS actually surfaces its native
// pairing-passkey prompt for a bare custom GATT peripheral (vs. requiring a
// companion app) hasn't been confirmed against real hardware/phones. Try it
// against your actual target devices before relying on this for anything.

void ble2faBegin();                 // idempotent
bool ble2faBondedDeviceConnected();  // true once a peer completes authenticated pairing
bool ble2faPasskeyPending();         // true while we're showing a passkey, waiting on the peer
uint32_t ble2faCurrentPasskey();

// Bonds live in NVS and survive reboots -- a paired phone reconnects
// (silently re-encrypting from the stored keys) without another passkey.
// ble2faHasStoredBond() reports that a bond is on file even with no live
// connection right now; ble2faClearBonds() forgets every bond (called from
// the SD-wipe path so "clear the card" also clears the pairing).
bool ble2faHasStoredBond();
void ble2faClearBonds();

// Engagement-data export: a READ-only, encryption-required GATT
// characteristic (6e400003-...) a bonded peer can read to pull the current
// engagement summary. Off by default; the Engagement screen toggles it.
void ble2faSetExport(const String &text);

// Blocking UI: begins advertising, shows the passkey when offered, waits
// for a bonded connection or a tap on the skip/cancel button. Returns true
// if paired. Shared by the Engagement Page (where cancelling means "not
// armed") and the first-boot wizard (where skipping just means no time
// sync yet) so the flow isn't duplicated in both places.
bool ble2faPairAndWait(const char *skipLabel);
