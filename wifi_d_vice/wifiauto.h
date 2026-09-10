#pragma once
#include <Arduino.h>

// "Connect on boot" for one Wi-Fi network. Credentials live in NVS
// (namespace "wifiauto"), plaintext -- this is internal flash, not the
// removable SD card, and boot happens before any engagement key exists so
// an encrypted copy would be unusable here anyway. Independent of the
// per-client engStore Wi-Fi list (that one is for pre-filling the passphrase
// prompt on a later manual connect).

// Store creds + arm boot-connect. pass "" for an open network.
void   wifiAutoStore(const String &ssid, const String &pass);
// Disarm boot-connect and wipe the stored creds.
void   wifiAutoClear();
bool   wifiAutoEnabled();
String wifiAutoSSID();
// true iff boot-connect is armed AND its SSID matches `ssid`.
bool   wifiAutoIsFor(const String &ssid);

// setup(): if armed, kick a non-blocking WiFi.begin() so the link comes up
// during the splash hold. Returns true if it started a connect.
bool   wifiAutoConnectOnBoot();
