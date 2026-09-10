#pragma once
#include <Arduino.h>
// Curated OUI-prefix -> vendor-name lookup (not the full IEEE registry --
// that's tens of thousands of entries / several MB, impractical to embed
// here). Covers common consumer/IoT/networking vendors likely to show up
// in a scan. Falls back to "Unknown" for anything not in the table.
//
// Returns "Vendor:XXYYZZ" (vendor short name, colon, last 3 MAC bytes as
// hex) -- short enough for a list row, still uniquely identifies the
// specific device via those last 3 bytes.
String macVendorTag(const uint8_t mac[6]);
