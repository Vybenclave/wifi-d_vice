#pragma once
// Detection signatures used by flock_detect.cpp, skimmer_detect.cpp,
// tracker_detect.cpp and ble_scan.cpp.
//
// Flock Safety patterns: name/SSID substring matching only. Sourced from
// public write-ups (the flock-you and FlipDeFlock projects, referencing
// deflock.me) which independently corroborate name-based detection
// ("flock*", "Penguin*", "Pigvision*", "FS_*" SSID/BLE-name prefixes) as a
// real technique multiple projects use. Deliberately NOT including any
// MAC-OUI list here -- the one example pulled from a public repo during
// research turned out to be placeholder values (AA:BB:CC etc.), not real
// vendor OUIs, and shipping fabricated OUIs would be worse than not
// detecting by MAC at all. If you have a verified OUI list, add it here.
static const char *const kFlockNamePatterns[] = {
  "flock", "penguin", "pigvision", "fs_",
};
static const int kFlockNamePatternCount = sizeof(kFlockNamePatterns) / sizeof(kFlockNamePatterns[0]);

// Bluetooth card-skimmer module names. Cheap HC-05/HC-06/HC-08 serial
// modules (and clones sold as "FREE2MOVE") are the commodity parts found in
// pump/ATM overlay skimmers and are the standard detection signature cited
// across multiple sources (BlueSleuth, ESP32Marauder's skimmer detector,
// UFL's "Kiss from a Rogue" paper) -- case-insensitive substring match.
// The list past the first four is the broader commodity-module family that
// Marauder ("HC-03") and Wireless Wizard ("JDY-31", "HM-10") also match:
// the same TTL-serial-over-Bluetooth parts, just other vendors' silk-screen
// names, sold pre-flashed with these defaults on the same skimmer boards.
static const char *const kSkimmerNamePatterns[] = {
  "hc-05", "hc-06", "hc-08", "free2move",
  "hc-03", "hc-02", "jdy-31", "jdy-33", "jdy-08", "hm-10", "at-09",
  "spp-c", "bt05", "mlt-bt05", "zs-040",
};
static const int kSkimmerNamePatternCount = sizeof(kSkimmerNamePatterns) / sizeof(kSkimmerNamePatterns[0]);

// Serial-over-BLE (UART bridge) GATT service UUIDs. A skimmer operator who
// renames the module defeats the name match above, but the transparent-UART
// service the HM-10 / JDY / HC-08-BLE family exposes is still there. Matched
// as a lowercase substring of BLEAdvertisedDevice::getServiceUUID(i) (which
// reports either the 16-bit "ffd0" short form or the 128-bit
// "0000ffd0-0000-1000-8000-00805f9b34fb" long form). Deliberately NOT
// including the far more common 0xFFE0 (Nordic UART clones, countless BLE
// dev kits) -- 0xFFD0 is specific enough to the JDY-08 / HC-08 bridge that
// Wireless Wizard singles it out. Still a corroborating signal, not proof.
static const char *const kSkimmerServiceUuids[] = {
  "ffd0",
};
static const int kSkimmerServiceUuidCount = sizeof(kSkimmerServiceUuids) / sizeof(kSkimmerServiceUuids[0]);

// Flipper Zero: the BLE advertised name is "Flipper <name>" out of the box.
// Marauder's Flipper Sniff and Wireless Wizard's Flipper Detect both key on
// this prefix; the manufacturer-data / service-UUID case-colour trick is
// extra and not needed just to say "a Flipper is nearby".
static const char *const kFlipperNamePatterns[] = {
  "flipper ",
};
static const int kFlipperNamePatternCount = sizeof(kFlipperNamePatterns) / sizeof(kFlipperNamePatterns[0]);

// Ray-Ban Meta / Meta smart glasses. Name match plus the BLE company
// identifiers seen in their manufacturer data: 0x01AB (Facebook/Meta),
// 0x058E (Meta Platforms), 0x0D53 (Luxottica). ble_scan.cpp checks the
// company id directly; these strings cover the name path.
static const char *const kMetaGlassesNamePatterns[] = {
  "ray-ban meta", "ray ban meta", "meta glasses", "rbm ",
};
static const int kMetaGlassesNamePatternCount = sizeof(kMetaGlassesNamePatterns) / sizeof(kMetaGlassesNamePatterns[0]);
static const uint16_t kMetaCompanyIds[] = { 0x01AB, 0x058E, 0x0D53 };
static const int kMetaCompanyIdCount = sizeof(kMetaCompanyIds) / sizeof(kMetaCompanyIds[0]);

static bool matchesAnyPattern(const String &nameLower, const char *const *patterns, int count) {
  for (int i = 0; i < count; i++) {
    if (nameLower.indexOf(patterns[i]) >= 0) return true;
  }
  return false;
}
