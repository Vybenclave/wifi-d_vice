#pragma once
#include <Arduino.h>
// Promote a hit to a tracked finding. Any detector screen calls this.
// The flow picks a tag and accepts an optional note. The device does not
// manage the findings register. Raw detection stays in the screen log.
//
// `source` names the detector. `severity` uses the UI_SEV_OK/WATCH/ALERT scale.
// Pass an empty `location` string to auto-fill from the live GPS fix.
// The function requires an armed engagement. It returns silently on cancel.
void flagDetectionShow(const char *source, uint8_t severity, const String &location = "");

// Show details for a single hit. The screen blocks until the user taps back.
// Pass nullptr for `mac` or `ssid` to show "N/A". Pass false for `hasGps` to hide coordinates.
// The `source` string passes to the flag function.
void showDetectionDetail(const char *headline, uint8_t severity, const char *timeStr,
                          const uint8_t *mac, const char *ssid,
                          bool hasGps, float lat, float lon, const char *source);
