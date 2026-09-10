#include "mac_vendor.h"

struct OuiEntry { uint32_t oui; const char *name; };   // oui = 0x00AABBCC style, top 3 bytes

// Not sorted -- linear search below. Table's small enough (called only on
// scan-row display, not a hot loop) that sorting for binary search isn't
// worth the risk of a hand-sorted list silently being wrong somewhere.
static const OuiEntry kOui[] = {
  {0x000C29, "VMware"},
  {0x000D3A, "Microsoft"},
  {0x0016CB, "Apple"},
  {0x001B63, "Apple"},
  {0x0021B7, "TexasInstr"},
  {0x001CB3, "Apple"},
  {0x0024D7, "Intel"},
  {0x00256F, "Apple"},
  {0x0026BB, "Apple"},
  {0x00507F, "Cisco"},
  {0x00904C, "Epigram"},
  {0x0C4796, "Wistron"},
  {0x108CCF, "Ubiquiti"},
  {0x149130, "AzureWave"},
  {0x18B430, "Nest"},
  {0x1C9E46, "NetgearMob"},
  {0x24A2E1, "AmazonTech"},
  {0x2C5089, "Sony"},
  {0x2CF0EE, "EspressifIn"},
  {0x30AEA4, "Espressif"},
  {0x3417EB, "SamsungElec"},
  {0x3C5AB4, "Google"},
  {0x3C71BF, "AmazonTech"},
  {0x3C8375, "GigaDevice"},
  {0x40A36B, "Xiaomi"},
  {0x442F9D, "SonosInc"},
  {0x480FCF, "TP-Link"},
  {0x4CE676, "GProXmt"},
  {0x50EC50, "TP-Link"},
  {0x54AF97, "AmazonTech"},
  {0x581F28, "PhilipsHue"},
  {0x5C5F67, "NestLabs"},
  {0x647002, "Sonos"},
  {0x6C56F7, "Roku"},
  {0x70886B, "SamsungElec"},
  {0x74C63B, "SonosInc"},
  {0x7CDD90, "Netgear"},
  {0x84F3EB, "Apple"},
  {0x8863DF, "Apple"},
  {0x8CAAB5, "SamsungElec"},
  {0x94103E, "Netgear"},
  {0x94944A, "SamsungElec"},
  {0x9803D8, "Apple"},
  {0x9C93E4, "IEEE reg"},
  {0xA02195, "Nintendo"},
  {0xA4C138, "IkeaTradfri"},
  {0xA4E31B, "IkeaTradfri"},
  {0xACBC32, "Apple"},
  {0xB0BE76, "D-Link"},
  {0xB827EB, "RaspberryPi"},
  {0xB8AEED, "Netgear"},
  {0xC82158, "Xiaomi"},
  {0xC8D083, "Netgear"},
  {0xCC50E3, "Espressif"},
  {0xD83134, "Espressif"},
  {0xDCA632, "RaspberryPi"},
  {0xDCA633, "RaspberryPi"},
  {0xE0757D, "IntelCorp"},
  {0xE8DB84, "Espressif"},
  {0xEC1A59, "Belkin"},
  {0xF018FA, "Amazon"},
  {0xF4F26D, "GoogleInc"},
  {0xF81654, "AppleInc"},
  {0xFCA47A, "GoogleInc"},
};
static const int kOuiCount = sizeof(kOui) / sizeof(kOui[0]);

static const char *lookupOui(uint32_t oui) {
  for (int i = 0; i < kOuiCount; i++) if (kOui[i].oui == oui) return kOui[i].name;
  return nullptr;
}

String macVendorTag(const uint8_t mac[6]) {
  uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
  const char *name = lookupOui(oui);
  char buf[24];
  snprintf(buf, sizeof(buf), "%s:%02X%02X%02X", name ? name : "Unknown", mac[3], mac[4], mac[5]);
  return String(buf);
}
