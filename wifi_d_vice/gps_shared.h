#pragma once
// Shared GPS reader. GPS is receive-only NMEA on GPIO35 (see pins.h) -- one
// UART, one TinyGPSPlus parser, owned here, so every consumer (the Wardrive
// screen, the time-sync scheduler below, System > Hardware > Test GPS) reads
// the same live fix instead of racing each other for bytes off the wire.
//
// gpsSharedBegin() opens the UART once at boot (see setup() in the .ino).
// gpsSharedLoop() must be called every main loop() iteration -- it drains
// whatever NMEA bytes have arrived and, on its own timer, tries to set the
// device clock from a GPS fix (devtime.h): every 15s until the first
// success, then once an hour to correct for local-oscillator drift. Logs
// always use devTimeNowString() (UTC), never a GPS-specific format.
#include <TinyGPSPlus.h>

void gpsSharedBegin();
void gpsSharedLoop();

// The live parser, for screens/tests that want the full TinyGPSPlus API
// (location, satellites, hdop, altitude, charsProcessed() for a "getting
// bytes at all" check, etc.) without re-opening the UART themselves.
TinyGPSPlus &gpsShared();

// Last-known fix, for engagement.cpp's soft time-window check. Backed by
// the shared parser above, so it reflects the live background reader now --
// no longer tied to whether the Wardrive screen has ever been opened.
bool gpsGetLastFix(int &year, int &month, int &day, int &hour, int &minute);
