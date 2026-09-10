#pragma once
// Shared with flock_detect / deauth_detect so they don't each reinvent a
// WiFi scan-result struct.
struct ApInfo {
  String ssid;
  uint8_t bssid[6];
  int32_t rssi;
  int32_t channel;
  wifi_auth_mode_t enc;
};
