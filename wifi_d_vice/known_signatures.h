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

// WiFi surveillance-camera OUIs, for camera_detect.cpp (Wireless Wizard's
// "Camera Detector"). These ARE real IEEE MA-L assignments to the named
// vendors -- taken from the public registry / Wireshark manuf, not
// invented (contrast the Flock note above -- no fabricated OUIs). Match is
// on the top 3 bytes of a beacon BSSID. A hit is "a device from a camera
// vendor", not proof of a camera: these vendors also ship NVRs, doorbells
// and the odd router, and a camera behind a router NATs behind that
// router's OUI. Kept to vendors that are overwhelmingly camera/NVR makers
// so the false-positive rate stays low; deliberately NOT including Amazon
// (Ring shares OUIs with Echo/Fire) or Netgear (mostly routers).
struct CameraOui { uint32_t oui; const char *vendor; };   // oui = 0x00RRGGBB (top 3 MAC bytes)
static const CameraOui kCameraOuis[] = {
  {0x001C27, "Hikvision"}, {0x4419B6, "Hikvision"}, {0x4CBD8F, "Hikvision"},
  {0xBCAD28, "Hikvision"}, {0xC056E3, "Hikvision"}, {0x2857BE, "Hikvision"},
  {0xE0CA3C, "Hikvision"}, {0xACB927, "Hikvision"},
  {0x3CEF8C, "Dahua"},     {0x9002A9, "Dahua"},     {0xE0508B, "Dahua"},
  {0x08EDED, "Dahua"},     {0x24526A, "Dahua"},     {0x14A78B, "Dahua"},
  {0x4C11BF, "Dahua"},     {0x38AF29, "Dahua"},
  {0x00408C, "Axis"},      {0xACCC8E, "Axis"},      {0xB8A44F, "Axis"},      {0xE82725, "Axis"},
  {0xEC71DB, "Reolink"},
  {0x9C8ECD, "Amcrest"},
  {0x2CAA8E, "Wyze"},
  {0x7483C2, "Ubiquiti"},  {0xFCECDA, "Ubiquiti"},  {0x788A20, "Ubiquiti"},
  {0xE063DA, "Ubiquiti"},  {0x245A4C, "Ubiquiti"},  {0xF492BF, "Ubiquiti"},
  {0x001344, "Vivotek"},   {0x0002D1, "Vivotek"},
  {0x000B94, "Uniview"},   {0x48EA63, "Uniview"},
};
static const int kCameraOuiCount = sizeof(kCameraOuis) / sizeof(kCameraOuis[0]);

static const char *cameraVendorForBssid(const uint8_t b[6]) {
  uint32_t o = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
  for (int i = 0; i < kCameraOuiCount; i++)
    if (kCameraOuis[i].oui == o) return kCameraOuis[i].vendor;
  return nullptr;
}

static bool matchesAnyPattern(const String &nameLower, const char *const *patterns, int count) {
  for (int i = 0; i < count; i++) {
    if (nameLower.indexOf(patterns[i]) >= 0) return true;
  }
  return false;
}
