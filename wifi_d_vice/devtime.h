#pragma once
#include <Arduino.h>
// Wall-clock time, synced from SNTP once WiFi is connected (see
// devTimeBeginNet()). Backed by the ESP32's libc time() (settimeofday()),
// so it keeps ticking off the internal timer between syncs.
// (BLE Current Time Service sync was removed -- phone OSes didn't reliably
// push it to a bare GATT peripheral.)
bool devTimeSynced();
// Once WiFi is associated, kick off an SNTP sync -- tries an NTP server
// handed out by DHCP (option 42) first, then the gateway, then time.nist.gov,
// then pool.ntp.org. No-op if already synced (e.g. from BLE) or already
// started. devTimePoll() promotes to "synced" once a reply lands; call it
// from loop().
void devTimeBeginNet();
void devTimePoll();
// Feed an absolute epoch (UTC seconds) from a WiFi-independent source --
// e.g. the rx_time carried in a Meshtastic mesh packet. A lower-priority
// bootstrap: it only sets the clock when nothing else has (!devTimeSynced())
// and the value passes a sanity window (2024-01-01 .. 2100); otherwise it is
// ignored. The SNTP path is untouched -- if WiFi later associates before any
// source has synced, SNTP still runs. 'source' is a short static tag kept for
// diagnostics (e.g. "meshtastic").
void devTimeSetEpoch(uint32_t epoch, const char *source);
uint32_t devTimeNow();          // epoch seconds, 0 if never synced
String devTimeNowString();      // "YYYY-MM-DD HH:MM:SS", "unsynced" if never synced
String devDateString();         // "YYYY-MM-DD", "unsynced" if never synced -- for folder names
