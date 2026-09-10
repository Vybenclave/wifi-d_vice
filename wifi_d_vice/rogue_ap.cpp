#include "rogue_ap.h"
#include <SD.h>
#include <string.h>
#include "sd_bus.h"
#include "pins.h"

static const char *PATH = "/rogueap.csv";

// ---- baseline in RAM ----
struct Entry {
  char    essid[33];
  uint8_t bssid[6];
  uint8_t auth;
  uint8_t channel;
  int8_t  rssi;
};
static Entry s_base[ROGUE_MAX];
static int   s_n = 0;
static bool  s_loaded = false;

// learn accumulator (deduped on BSSID; RSSI kept as a running mean)
static Entry s_learn[ROGUE_MAX];
static int   s_learnN = 0;
static int   s_learnCnt[ROGUE_MAX];   // samples folded into s_learn[i].rssi

// ---- security ranking ----
// `a` is a wifi_auth_mode_t, except 0xFE which the WiFi IDS beacon path
// passes for "encrypted, strength unknown" (all it can tell from the
// capability Privacy bit) -- ranked at the top so it never reads as a
// downgrade against any baseline.
static int authRank(uint8_t a) {
  switch (a) {
    case 0:    return 0;   // OPEN
    case 9:    return 0;   // OWE (encrypted but unauthenticated)
    case 1:    return 1;   // WEP
    case 2:    return 2;   // WPA
    case 6: case 7: return 4;      // WPA3 / WPA2+WPA3
    case 0xFE:     return 4;       // IDS: "encrypted, unknown strength"
    default:       return 3;       // WPA2 / WPA+WPA2 / enterprise / WAPI
  }
}

// ---- hex/mac helpers ----
static void macToStr(const uint8_t *m, char *out) {   // out[18]
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static bool strToMac(const char *s, uint8_t *m) {
  int v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) m[i] = (uint8_t)v[i];
  return true;
}

// ---- file IO ----
static bool sdReady() {
  sdBusBegin();
  return SD.begin(SD_CS, sdSPI);
}

void rogueApLoad() {
  s_n = 0;
  s_loaded = false;
  if (!sdReady() || !SD.exists(PATH)) return;
  File f = SD.open(PATH, FILE_READ);
  if (!f) return;
  while (f.available() && s_n < ROGUE_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    // essid,bssid,auth,channel,rssi   -- ESSID assumed comma-free
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);
    int c4 = line.indexOf(',', c3 + 1);
    if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0) continue;
    Entry &e = s_base[s_n];
    memset(&e, 0, sizeof(e));
    String es = line.substring(0, c1);
    strncpy(e.essid, es.c_str(), sizeof(e.essid) - 1);
    if (!strToMac(line.substring(c1 + 1, c2).c_str(), e.bssid)) continue;
    e.auth    = (uint8_t)line.substring(c2 + 1, c3).toInt();
    e.channel = (uint8_t)line.substring(c3 + 1, c4).toInt();
    e.rssi    = (int8_t)line.substring(c4 + 1).toInt();
    s_n++;
  }
  f.close();
  s_loaded = true;
}

int  rogueApCount()  { return s_n; }
bool rogueApLoaded() { return s_loaded; }

void rogueApClear() {
  s_n = 0;
  s_loaded = false;
  if (sdReady() && SD.exists(PATH)) SD.remove(PATH);
}

// ---- learn ----
void rogueApLearnReset() {
  s_learnN = 0;
  memset(s_learnCnt, 0, sizeof(s_learnCnt));
}

void rogueApLearnObserve(const char *essid, const uint8_t *bssid,
                         uint8_t auth, uint8_t channel, int rssi) {
  if (!essid || essid[0] == 0) return;   // skip hidden SSIDs -- nothing to match on
  for (int i = 0; i < s_learnN; i++) {
    if (!memcmp(s_learn[i].bssid, bssid, 6)) {
      int c = ++s_learnCnt[i];
      s_learn[i].rssi = (int8_t)((s_learn[i].rssi * (c - 1) + rssi) / c);
      if (authRank(auth) > authRank(s_learn[i].auth)) s_learn[i].auth = auth;  // keep the strongest seen
      s_learn[i].channel = channel;
      return;
    }
  }
  if (s_learnN >= ROGUE_MAX) return;
  Entry &e = s_learn[s_learnN];
  memset(&e, 0, sizeof(e));
  strncpy(e.essid, essid, sizeof(e.essid) - 1);
  memcpy(e.bssid, bssid, 6);
  e.auth = auth;
  e.channel = channel;
  e.rssi = (int8_t)rssi;
  s_learnCnt[s_learnN] = 1;
  s_learnN++;
}

int rogueApLearnPending() { return s_learnN; }

int rogueApLearnCommit() {
  memcpy(s_base, s_learn, sizeof(s_base));
  s_n = s_learnN;
  s_loaded = true;

  if (sdReady()) {
    File f = SD.open(PATH, FILE_WRITE);   // truncates
    if (f) {
      f.println("# WIFI D_VICE rogue-AP baseline: essid,bssid,auth,channel,rssi");
      char mac[18];
      for (int i = 0; i < s_n; i++) {
        macToStr(s_base[i].bssid, mac);
        f.printf("%s,%s,%u,%u,%d\n", s_base[i].essid, mac,
                 s_base[i].auth, s_base[i].channel, s_base[i].rssi);
      }
      f.close();
    }
  }
  return s_n;
}

// ---- check ----
const char *rogueKindTag(RogueKind k) {
  switch (k) {
    case ROGUE_EVIL_TWIN: return "EVIL-TWIN";
    case ROGUE_DOWNGRADE: return "DOWNGRADE";
    case ROGUE_CHAN_MOVE: return "CHAN-MOVE";
    case ROGUE_RSSI_JUMP: return "RSSI-JUMP";
    default:              return "ok";
  }
}

RogueKind rogueApCheck(const char *essid, const uint8_t *bssid,
                       uint8_t auth, uint8_t channel, int rssi, RogueHit *out) {
  if (s_n == 0 || !essid || essid[0] == 0) return ROGUE_NONE;

  bool essidKnown = false;
  const Entry *match = nullptr;   // same ESSID AND same BSSID
  for (int i = 0; i < s_n; i++) {
    if (strcmp(s_base[i].essid, essid) != 0) continue;
    essidKnown = true;
    if (!memcmp(s_base[i].bssid, bssid, 6)) { match = &s_base[i]; break; }
  }
  if (!essidKnown) return ROGUE_NONE;   // not a network we have a baseline for

  RogueKind k = ROGUE_NONE;
  uint8_t   baseAuth = 0;

  if (!match) {
    k = ROGUE_EVIL_TWIN;                          // known name, unknown radio
  } else {
    baseAuth = match->auth;
    if (authRank(auth) < authRank(match->auth) && authRank(match->auth) >= 2)
      k = ROGUE_DOWNGRADE;
    else if (channel && match->channel && channel != match->channel)
      k = ROGUE_CHAN_MOVE;
    else if (abs(rssi - (int)match->rssi) > 30)
      k = ROGUE_RSSI_JUMP;
  }

  if (k != ROGUE_NONE && out) {
    memset(out, 0, sizeof(*out));
    out->kind = k;
    strncpy(out->essid, essid, sizeof(out->essid) - 1);
    memcpy(out->bssid, bssid, 6);
    out->channel  = channel;
    out->rssi     = (int8_t)rssi;
    out->baseAuth = baseAuth;
  }
  return k;
}
