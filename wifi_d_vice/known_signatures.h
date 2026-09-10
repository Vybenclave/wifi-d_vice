#pragma once
// Detection signatures used by flock_detect.cpp and skimmer_detect.cpp.
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
static const char *const kSkimmerNamePatterns[] = {
  "hc-05", "hc-06", "hc-08", "free2move",
};
static const int kSkimmerNamePatternCount = sizeof(kSkimmerNamePatterns) / sizeof(kSkimmerNamePatterns[0]);

static bool matchesAnyPattern(const String &nameLower, const char *const *patterns, int count) {
  for (int i = 0; i < count; i++) {
    if (nameLower.indexOf(patterns[i]) >= 0) return true;
  }
  return false;
}
