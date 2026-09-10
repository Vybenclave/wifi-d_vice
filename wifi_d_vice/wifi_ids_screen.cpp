// WiFi IDS -- the Tier 1 management-frame anomaly screen (DESIGN.md section 3).
//
// This is the old "Deauth Detect" screen grown into the umbrella the shared
// wifi_ids core was built for: ONE promiscuous session, ONE hop schedule,
// three passive detectors registered against it, one unified verdict area.
//
//   * Deauth v2   -- deauth + disassoc rate (the original 10-in-5s flood
//                    threshold), reason-code readout, and spoof detection:
//                    a deauth/disassoc that claims to come from a real AP
//                    but whose channel / RSSI / sequence number don't match
//                    that AP's own beacons.
//   * Beacon flood -- distinct-BSSID rate spike over ambient, random / binary
//                     SSIDs, locally-administered ("random") BSSID MACs, and
//                     near-sequential BSSID MACs (mdk4 / aireplay beacon spam).
//   * Auth/assoc flood -- auth + (re)assoc-request rate to one BSSID from
//                         many distinct (often locally-administered) source
//                         MACs in a short window (mdk3 'a').
//
// Why one screen and not three: the wifi_ids core has a single RX-callback
// slot and one hop schedule; three screens would each spin the radio up and
// down and each carry a near-identical Enter/Loop/Exit + draw + window
// harness. One screen = one wifiIdsBegin(), three wifiIdsRegister() calls,
// one throttled redraw, one alert log. Cheaper in flash and in RAM.
//
// PASSIVE ONLY. Every detector just counts frames handed to it by wifi_ids
// (receive-only). Nothing here transmits.
//
// RAM: all state is small fixed tables, sized in bytes:
//   AP baseline  12 * 16 B  = 192 B  (beacon writes it, deauth spoof reads it)
//   beacon bloom      64 B          (distinct BSSIDs this window)
//   auth src bloom    64 B          (distinct source MACs this window)
//   auth targets  8 * 8 B   =  64 B
//   beacon recent 4 * 6 B   =  24 B
//   alert log     4 * 26 B  = 104 B
//   + scalars                ~120 B
//   ------------------------------  ~ 0.7 KB total, all static.
// Nothing here is big enough to need calloc/free like the wifi_ids ring.

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ui.h"
#include "wifi_ids.h"
#include "wlog.h"
#include "rogue_ap.h"

// ---- window ------------------------------------------------------------
//
// 5 s. At this screen's 300 ms-per-channel dwell a full 1..13 hop sweep is
// ~3.9 s, so a 5 s window is ~1.3 sweeps: long enough that "distinct BSSIDs
// this window" / "distinct source MACs this window" approximate the real
// ambient population instead of one channel's slice, short enough to trip
// within a couple of seconds of a flood starting. The deauth threshold
// stays the historical 10-in-5s.
static const uint32_t WINDOW_MS = 5000;

enum { SEV_OK = 0, SEV_WATCH = 1, SEV_ALERT = 2 };

// ---- shared AP beacon baseline --------------------------------------
//
// Populated by the beacon detector for nearby APs only (RSSI > -85: the APs
// an attacker would actually target, and the only ones whose beacons we can
// trust as a spoof reference). Read by the deauth detector to sanity-check a
// deauth that claims to be from one of them. LRU eviction by last-seen.
struct Ap {
  uint8_t  bssid[6];
  uint16_t seq;      // last beacon sequence number seen
  int8_t   rssi;     // EWMA of beacon RSSI
  uint8_t  ch;       // channel its beacons arrive on
  uint32_t seen;     // millis() of last beacon
  uint32_t ssidHash; // FNV-1a of the beacon SSID (0 = hidden/none) -- for the
                     // baseline-free evil-twin check
  uint8_t  priv;     // capability Privacy bit (1 = encrypted)
};
static const int AP_N = 12;
static Ap aps[AP_N];

static Ap *apFind(const uint8_t *b) {
  for (int i = 0; i < AP_N; i++)
    if (aps[i].seen && !memcmp(aps[i].bssid, b, 6)) return &aps[i];
  return nullptr;
}
static Ap *apLruSlot() {
  int o = 0;
  for (int i = 1; i < AP_N; i++) if (aps[i].seen < aps[o].seen) o = i;
  return &aps[o];
}

// ---- tiny Bloom filter (distinct-count estimator) -------------------
//
// 512 bits, 3 hashes, memset to zero each window. We don't store the MACs;
// we popcount the filter and back out an estimated distinct count from
//   bits_set ~= m * (1 - e^(-k*n/m))     (m=512, k=3)
// which inverts to  n ~= -(m/k) * ln(1 - bits/m).
// Reference points: 40 distinct -> ~107 bits, 85 -> ~200, 150 -> ~300,
// 300 -> ~424. Ambient home/office beacons top out around 10-40 distinct
// BSSIDs; mdk4 beacon flood is hundreds/sec, so it pins this immediately.
static const int BLOOM_BYTES = 64;

static void bloomAdd(uint8_t *bf, const uint8_t *k, uint8_t n) {
  uint32_t h = 2166136261u;                       // FNV-1a
  for (uint8_t i = 0; i < n; i++) { h ^= k[i]; h *= 16777619u; }
  uint32_t h2 = (h ^ (h >> 13)) * 0x9E3779B1u;
  uint32_t h3 = (h2 ^ (h2 >> 15)) * 0x85EBCA77u;
  uint32_t idx[3] = { h % 512u, h2 % 512u, h3 % 512u };
  for (int i = 0; i < 3; i++) bf[idx[i] >> 3] |= (uint8_t)(1u << (idx[i] & 7));
}
static int bloomPop(const uint8_t *bf) {
  int c = 0;
  for (int i = 0; i < BLOOM_BYTES; i++) c += __builtin_popcount(bf[i]);
  return c;
}
static uint16_t bloomEst(int pop) {
  if (pop <= 0) return 0;
  if (pop >= 510) return 999;
  double n = -(512.0 / 3.0) * log(1.0 - (double)pop / 512.0);
  return (uint16_t)(n + 0.5);
}

// ---- per-window accumulators --------------------------------------

// deauth v2
static uint32_t dDeauth, dDisassoc;
static uint16_t dLastReason;
static uint16_t dSpoof;
static const uint32_t DEAUTH_WATCH = 5;    // ~1/s sustained: unusual but not yet an attack call
static const uint32_t DEAUTH_ALERT = 10;   // historical 10-in-5s (~2/s) flood threshold

// beacon flood
static uint8_t  bBloom[BLOOM_BYTES];
static uint32_t bBeacons;
static uint16_t bRandom;    // random / binary SSID
static uint16_t bLaa;       // locally-administered BSSID MAC
static uint16_t bSeqMac;    // BSSID MAC near-sequential to a recent one
static uint8_t  bRecent[4][6];
static uint8_t  bRecentN;

// auth / assoc flood
struct Tgt { uint8_t bssid[6]; uint16_t hits; };
static const int TGT_N = 8;
static Tgt      tgts[TGT_N];
static uint8_t  aSrcBloom[BLOOM_BYTES];
static uint32_t aReq;
static uint16_t aLaaSrc;

// dropped-frame delta: if the capture ring overflows, capture can't keep up
// with the air -- itself a flood signal, per wifi_ids.h.
static uint32_t dropAtWindow;
static uint32_t dropDelta;

static uint32_t gSeen;      // mgmt frames dispatched to our detectors, cumulative

// rogue-AP / evil-twin: checked against the SD baseline (rogue_ap.cpp). Off
// unless /rogueap.csv exists. A hit feeds the alert log + the banner. Each
// offending BSSID is reported once per session.
static bool     rogueOn;
static bool     vRogue;
static uint8_t  rogueSeen[8][6];
static uint8_t  rogueSeenN;

// Baseline-free extras that need no /rogueap.csv (DESIGN.md section 3, and
// what Marauder's Detect-Pwnagotchi / Wireless Wizard's scored evil-twin
// do). Both feed the alert log + the banner; there is no room for another
// stats row.
static bool     vPwn;        // a Pwnagotchi beacon (src/BSSID de:ad:be:ef:de:ad) was seen
static bool     pwnLogged;   // one alert-log line per session
static bool     vTwin;       // two BSSIDs, one SSID, divergent enough to score an evil twin

static bool rogueAlready(const uint8_t *b) {
  for (uint8_t i = 0; i < rogueSeenN; i++)
    if (!memcmp(rogueSeen[i], b, 6)) return true;
  if (rogueSeenN < 8) memcpy(rogueSeen[rogueSeenN++], b, 6);
  return false;
}

// ---- last completed window's verdict (what the screen shows) --------
struct Verdict { uint8_t sev; uint16_t a, b, c, d, e; };
static Verdict  vD, vB, vA;
static uint8_t  vATop[6];   // busiest auth/assoc target BSSID
static bool     vSpoofFlag;

// ---- alert log ------------------------------------------------------
static char    alog[4][26];
static uint8_t alogN;
static bool    alogDirty;

static void alogPush(const char *s) {
  if (alogN < 4) {
    strncpy(alog[alogN], s, 25); alog[alogN][25] = 0; alogN++;
  } else {
    memmove(alog[0], alog[1], 3 * 26);
    strncpy(alog[3], s, 25); alog[3][25] = 0;
  }
  alogDirty = true;
}

// ---- screen state -------------------------------------------------
static bool     running = false;
static bool     s_jumpRogue = false;   // widsEnter's no-baseline prompt -> Rogue AP screen
static int      hD = -1, hB = -1, hA = -1;

bool widsTakeJumpToRogue() { bool j = s_jumpRogue; s_jumpRogue = false; return j; }
static uint32_t startMs, windowStart, lastStatsDraw;
static uint16_t lastBannerKey = 0xFFFF;
static bool     alerted = false;
static uint32_t lastWlogSummary = 0;   // event-level SD log: one summary row / minute

// ---- layout -----------------------------------------------------
static const int HDR_Y    = 30;
static const int D_Y      = 46;
static const int B_Y      = 76;
static const int A_Y      = 104;
static const int R_Y      = 126;                  // rogue-AP status line
static const int STATS_H  = (R_Y + 11) - HDR_Y;
static const int BANNER_Y = 140;
static const int BANNER_H  = 44;
static const int LOG_Y    = BANNER_Y + BANNER_H + 2;

// ---- SSID helpers -------------------------------------------------
//
// Beacon body: 24 B MAC header + 12 B fixed params, then the IE list; the
// SSID IE (id 0) is first. wifi_ids snapshots the front of the frame so the
// SSID is always inside f.raw for a non-runt beacon.
static bool beaconSsid(const WifiIdsFrame &f, const uint8_t **ssid, uint8_t *len) {
  if (f.rawLen < 38) return false;
  const uint8_t *ie = f.raw + 36;
  if (ie[0] != 0) return false;
  uint8_t l = ie[1];
  if (l == 0 || 38 + l > f.rawLen) return false;
  *ssid = ie + 2; *len = l;
  return true;
}

// Cheap "this SSID was generated, not typed by a human" heuristic. mdk-style
// beacon spam draws SSIDs from random bytes: either non-ASCII/control bytes
// land in them, or they come out as long vowel-less letter salad. No String,
// no entropy math.
static bool ssidLooksRandom(const uint8_t *s, uint8_t n) {
  if (n < 6) return false;
  uint8_t nonprint = 0, vowels = 0, digits = 0, alpha = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c < 0x20 || c > 0x7E) { nonprint++; continue; }
    uint8_t lc = c | 0x20;
    if (lc == 'a' || lc == 'e' || lc == 'i' || lc == 'o' || lc == 'u') vowels++;
    if (c >= '0' && c <= '9') digits++;
    if (lc >= 'a' && lc <= 'z') alpha++;
  }
  if (nonprint) return true;                         // 8-bit / control bytes
  if (alpha >= 6 && vowels == 0) return true;        // long, no vowel
  if (n >= 10 && digits * 3 >= (int)n && alpha >= 3) return true;  // digit/letter salad
  return false;
}

static uint32_t ssidHash32(const uint8_t *s, uint8_t n) {
  uint32_t h = 2166136261u;
  for (uint8_t i = 0; i < n; i++) { h ^= s[i]; h *= 16777619u; }
  return h ? h : 1;   // reserve 0 for "no SSID"
}

// Pwnagotchi beacons carry a JSON blob in the SSID; the unit's name is the
// "name":"..." field. Best-effort: the SSID snapshot can be truncated or
// the field can be past it -- then this returns "?".
static void pwnName(const uint8_t *s, uint8_t n, char *out, size_t outN) {
  strncpy(out, "?", outN); out[outN - 1] = 0;
  const char *key = "\"name\":\"";
  for (int i = 0; i + 8 < (int)n; i++) {
    if (memcmp(s + i, key, 8) != 0) continue;
    int j = i + 8, k = 0;
    while (j < (int)n && s[j] != '"' && k < (int)outN - 1) out[k++] = (char)s[j++];
    out[k] = 0;
    return;
  }
}

// ---- detector callbacks (task context, via wifiIdsLoop) -----------

static void onDeauthFamily(const WifiIdsFrame &f, void *) {
  gSeen++;
  if (f.subtype == WIDS_DEAUTH) dDeauth++;
  else                          dDisassoc++;
  if (f.rawLen >= 26) dLastReason = (uint16_t)(f.raw[24] | (f.raw[25] << 8));

  // Spoof check only when the frame claims the AP itself sent it
  // (transmitter == BSSID) -- that's the shape aireplay/mdk forge. Compare
  // against the AP's own recent beacons.
  if (!memcmp(f.addr2, f.addr3, 6)) {
    Ap *a = apFind(f.addr2);
    if (a && (millis() - a->seen) < 10000) {
      int score = 0;
      if (f.channel != a->ch) score += 2;                       // deauth off the AP's beacon channel
      if (abs((int)f.rssi - (int)a->rssi) > 18) score += 2;     // different distance / TX power
      int gap = (int)f.seq - (int)a->seq;
      if (gap < 0) gap += 4096;
      if (gap > 1500 && gap < 4090) score += 1;                 // seq far off the AP's counter
                                                               // (loose: we miss frames while hopping)
      if (score >= 2) dSpoof++;
    }
  }
}

static void onBeacon(const WifiIdsFrame &f, void *) {
  gSeen++;
  bBeacons++;
  const uint8_t *b = f.addr3;                 // BSSID

  bloomAdd(bBloom, b, 6);
  if (b[0] & 0x02) bLaa++;                    // locally-administered = randomized MAC

  // near-sequential BSSID: same top 4 bytes as a recent BSSID, low byte off
  // by 1..4 (mdk4 / fakeauth walking the MAC).
  for (uint8_t i = 0; i < bRecentN; i++) {
    if (!memcmp(bRecent[i], b, 4)) {
      int d = (int)b[5] - (int)bRecent[i][5];
      if (d < 0) d = -d;
      if (d >= 1 && d <= 4) { bSeqMac++; break; }
    }
  }
  bool known = false;
  for (uint8_t i = 0; i < bRecentN; i++)
    if (!memcmp(bRecent[i], b, 6)) { known = true; break; }
  if (!known) {
    memmove(bRecent[0], bRecent[1], 3 * 6);
    memcpy(bRecent[3], b, 6);
    if (bRecentN < 4) bRecentN++;
  }

  const uint8_t *s; uint8_t sl;
  bool haveSsid = beaconSsid(f, &s, &sl);
  if (haveSsid && ssidLooksRandom(s, sl)) bRandom++;

  // Pwnagotchi presence -- a beacon whose transmitter (or BSSID) is the
  // fixed de:ad:be:ef:de:ad. Not an attack in itself, but a
  // handshake-harvesting device in range is worth one alert-log line.
  static const uint8_t PWN_MAC[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD };
  if (!memcmp(f.addr2, PWN_MAC, 6) || !memcmp(b, PWN_MAC, 6)) {
    vPwn = true;
    if (!pwnLogged) {
      pwnLogged = true;
      char nm[16];
      if (haveSsid) pwnName(s, sl, nm, sizeof nm); else strcpy(nm, "?");
      uint32_t el = (millis() - startMs) / 1000;
      char line[26];
      snprintf(line, sizeof line, "%02lu:%02lu PWNAGOTCHI %.8s", el / 60, el % 60, nm);
      alogPush(line);
      if (wlogIsOpen()) {
        char wl[96];
        snprintf(wl, sizeof wl, "%lu,%d,pwnagotchi,watch,%s %ddBm",
                 (unsigned long)millis(), f.channel, nm, f.rssi);
        wlogRow(wl); wlogFlush();
      }
    }
  }

  // rogue-AP / evil-twin check against the SD baseline
  if (rogueOn && haveSsid && sl < 33) {
    char es[33];
    memcpy(es, s, sl); es[sl] = 0;
    uint8_t priv = 0;
    if (f.rawLen >= 36) {
      uint16_t cap = (uint16_t)(f.raw[34] | (f.raw[35] << 8));
      priv = (cap & 0x0010) ? 1 : 0;      // capability info bit 4 = Privacy
    }
    RogueHit rh;
    // priv bit is all a beacon capability field tells us: 0xFE = "encrypted,
    // strength unknown" (won't read as a downgrade), 0 = OPEN (will).
    RogueKind rk = rogueApCheck(es, f.addr3, priv ? 0xFE : 0,
                                f.channel, f.rssi, &rh);
    // Only the strong signals raise an IDS alert. CHAN-MOVE / RSSI-JUMP are
    // too noisy off single promiscuous captures (adjacent-channel leak,
    // per-frame RSSI jitter) -- the standalone Rogue AP screen, which has
    // accurate scan data, is where those belong.
    if (rk != ROGUE_EVIL_TWIN && rk != ROGUE_DOWNGRADE) rk = ROGUE_NONE;
    if (rk != ROGUE_NONE && !rogueAlready(f.addr3)) {
      vRogue = true;
      uint32_t el = (millis() - startMs) / 1000;
      char line[26];
      snprintf(line, sizeof line, "%02lu:%02lu %s %.9s",
               el / 60, el % 60, rogueKindTag(rk), es);
      alogPush(line);
      if (wlogIsOpen()) {
        char wl[96];
        snprintf(wl, sizeof wl, "%lu,%d,rogue,%s,%s %02X%02X%02X ch%u %ddBm",
                 (unsigned long)millis(), f.channel, rogueKindTag(rk), es,
                 f.addr3[3], f.addr3[4], f.addr3[5], f.channel, f.rssi);
        wlogRow(wl);
        wlogFlush();
      }
    }
  }

  // baseline for the deauth spoof check -- nearby APs only. Also carries the
  // SSID hash + Privacy bit for the baseline-free evil-twin check below.
  if (f.rssi > -85) {
    uint8_t priv = 0;
    if (f.rawLen >= 36) {
      uint16_t cap = (uint16_t)(f.raw[34] | (f.raw[35] << 8));
      priv = (cap & 0x0010) ? 1 : 0;
    }
    uint32_t hh = (haveSsid && sl < 33) ? ssidHash32(s, sl) : 0;

    // Baseline-free evil twin: another strong, recent AP advertises the same
    // SSID from a different BSSID, and the pair diverges more than a legit
    // multi-AP ESS would. Score needs a randomized (locally-administered)
    // BSSID or a security-downgrade to reach the threshold -- same-SSID +
    // different-channel/RSSI alone (normal for mesh / band-steering) tops
    // out at 3 and does not fire.
    if (hh) {
      for (int i = 0; i < AP_N; i++) {
        if (!aps[i].seen || aps[i].ssidHash != hh) continue;
        if (!memcmp(aps[i].bssid, b, 6)) continue;             // same AP, different frame
        if (millis() - aps[i].seen > 60000) continue;          // stale
        int score = 1;
        if ((b[0] & 0x02) || (aps[i].bssid[0] & 0x02)) score += 2;   // randomized BSSID
        if (priv != aps[i].priv)                       score += 2;   // security downgrade
        if (abs((int)f.rssi - (int)aps[i].rssi) > 25)  score += 1;
        if (f.channel != aps[i].ch)                    score += 1;
        if (score >= 4 && !rogueAlready(b)) {
          vTwin = true;
          char es[13] = "?";
          if (haveSsid) { uint8_t n = sl < 12 ? sl : 12; memcpy(es, s, n); es[n] = 0; }
          uint32_t el = (millis() - startMs) / 1000;
          char line[26];
          snprintf(line, sizeof line, "%02lu:%02lu EVIL-TWIN? %.9s", el / 60, el % 60, es);
          alogPush(line);
          if (wlogIsOpen()) {
            char wl[96];
            snprintf(wl, sizeof wl,
                     "%lu,%d,eviltwin,alert,%s %02X%02X%02X vs %02X%02X%02X sc%d",
                     (unsigned long)millis(), f.channel, es,
                     b[3], b[4], b[5], aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5], score);
            wlogRow(wl); wlogFlush();
          }
          break;
        }
      }
    }

    Ap *a = apFind(b);
    if (!a) { a = apLruSlot(); memset(a, 0, sizeof(*a)); memcpy(a->bssid, b, 6); a->rssi = f.rssi; }
    a->rssi     = (int8_t)((a->rssi * 3 + f.rssi) / 4);
    a->seq      = f.seq;
    a->ch       = f.channel;
    a->seen     = millis();
    if (hh) a->ssidHash = hh;
    a->priv     = priv;
  }
}

static void onAuthAssoc(const WifiIdsFrame &f, void *) {
  gSeen++;
  aReq++;
  const uint8_t *src = f.addr2;
  const uint8_t *bss = f.addr3;

  bloomAdd(aSrcBloom, src, 6);
  if (src[0] & 0x02) aLaaSrc++;

  Tgt *t = nullptr;
  int minSlot = 0;
  for (int i = 0; i < TGT_N; i++) {
    if (tgts[i].hits && !memcmp(tgts[i].bssid, bss, 6)) { t = &tgts[i]; break; }
    if (tgts[i].hits < tgts[minSlot].hits) minSlot = i;
  }
  if (!t) { t = &tgts[minSlot]; memcpy(t->bssid, bss, 6); t->hits = 0; }
  t->hits++;
}

// ---- verdict --------------------------------------------------------

static uint8_t sevDeauth() {
  uint32_t tot = dDeauth + dDisassoc;
  if (dSpoof) return SEV_ALERT;                     // an evidenced spoofed deauth: call it now
  if (tot >= DEAUTH_ALERT) return SEV_ALERT;
  if (tot >= DEAUTH_WATCH) return SEV_WATCH;
  return SEV_OK;
}

static uint8_t sevBeacon(uint16_t est) {
  int pop = bloomPop(bBloom);
  // ALERT: unmistakable flood -- ~85+ distinct BSSIDs in 5 s, OR a pile of
  // random SSIDs (ambient is ~zero), OR a moderate distinct-count rise that
  // is ALSO backed by randomized MACs / random SSIDs / a capture that can't
  // keep up.
  if (bRandom >= 12 || pop >= 200 ||
      (pop >= 110 && (bLaa >= 20 || bRandom >= 4 || bSeqMac >= 6 || dropDelta >= 8)))
    return SEV_ALERT;
  // WATCH: ~40+ distinct BSSIDs (top of a busy office) or a high raw beacon
  // rate without the corroborating anomalies.
  if (pop >= 110 || bBeacons >= 500) return SEV_WATCH;
  (void)est;
  return SEV_OK;
}

static uint8_t sevAuth(uint16_t srcEst) {
  int spop = bloomPop(aSrcBloom);
  // Normal auth/assoc is a trickle. ALERT: >=16/s, OR >=6/s that is ALSO
  // spread across ~50+ distinct source MACs (the mdk3 'a' signature).
  if (aReq >= 80 || (aReq >= 30 && spop >= 130)) return SEV_ALERT;
  if (aReq >= 30) return SEV_WATCH;
  (void)srcEst;
  return SEV_OK;
}

static void computeVerdicts() {
  dropDelta = wifiIdsDropped() - dropAtWindow;

  uint16_t bEst = bloomEst(bloomPop(bBloom));
  uint16_t aEst = bloomEst(bloomPop(aSrcBloom));

  uint8_t sd = sevDeauth();
  uint8_t sb = sevBeacon(bEst);
  uint8_t sa = sevAuth(aEst);

  vD = { sd, (uint16_t)dDeauth, (uint16_t)dDisassoc, dLastReason, dSpoof, 0 };
  vB = { sb, bEst, bRandom, bLaa, bSeqMac,
         (uint16_t)(bBeacons > 65535 ? 65535 : bBeacons) };
  // busiest auth/assoc target
  int top = 0;
  for (int i = 1; i < TGT_N; i++) if (tgts[i].hits > tgts[top].hits) top = i;
  memcpy(vATop, tgts[top].bssid, 6);
  vA = { sa, (uint16_t)aReq, aEst, aLaaSrc, tgts[top].hits, 0 };

  vSpoofFlag = dSpoof > 0;
}

// rising edge OK/WATCH -> ALERT: log it (used for the scrolling list).
static void logEdges(uint8_t pd, uint8_t pb, uint8_t pa) {
  uint32_t el = (millis() - startMs) / 1000;
  char line[26];
  if (vD.sev == SEV_ALERT && pd != SEV_ALERT) {
    if (vSpoofFlag) snprintf(line, sizeof line, "%02lu:%02lu DEAUTH SPOOF x%u",
                             el / 60, el % 60, vD.d);
    else            snprintf(line, sizeof line, "%02lu:%02lu DEAUTH FLOOD %u/5s",
                             el / 60, el % 60, vD.a + vD.b);
    alogPush(line);
  }
  if (vB.sev == SEV_ALERT && pb != SEV_ALERT) {
    snprintf(line, sizeof line, "%02lu:%02lu BEACON ~%u rnd%u", el / 60, el % 60, vB.a, vB.b);
    alogPush(line);
  }
  if (vA.sev == SEV_ALERT && pa != SEV_ALERT) {
    snprintf(line, sizeof line, "%02lu:%02lu AUTH %u/5s src~%u", el / 60, el % 60, vA.a, vA.b);
    alogPush(line);
  }
}

static void resetWindow() {
  dDeauth = dDisassoc = dSpoof = 0;
  memset(bBloom, 0, sizeof bBloom);
  bBeacons = 0; bRandom = bLaa = bSeqMac = 0;
  memset(aSrcBloom, 0, sizeof aSrcBloom);
  aReq = 0; aLaaSrc = 0;
  for (int i = 0; i < TGT_N; i++) tgts[i].hits = 0;
  dropAtWindow = wifiIdsDropped();
}

// ---- drawing ------------------------------------------------------

static uint16_t sevColor(uint8_t s) {
  return s == SEV_ALERT ? ILI9341_RED : (s == SEV_WATCH ? ILI9341_YELLOW : ILI9341_GREEN);
}
static const char *sevTag(uint8_t s) {
  return s == SEV_ALERT ? "ALERT" : (s == SEV_WATCH ? "watch" : "ok");
}

// Event-level SD log (never per frame): a row on ANY detector's severity
// change -- OK->WATCH/ALERT and back -- plus one summary row per minute
// from widsLoop(). Columns: millis,channel,detector,severity,detail.
static void wlogVerdictEdges(uint8_t pd, uint8_t pb, uint8_t pa) {
  if (!wlogIsOpen()) return;
  struct { const char *name; uint8_t prev, now; uint16_t a, b; } d[3] = {
    { "deauth", pd, vD.sev, vD.a, vD.b },   // a=deauth  b=disassoc
    { "beacon", pb, vB.sev, vB.a, vB.b },   // a=uniq~   b=random-ssid
    { "auth",   pa, vA.sev, vA.a, vA.b },   // a=req     b=src~
  };
  bool wrote = false;
  for (int i = 0; i < 3; i++) {
    if (d[i].now == d[i].prev) continue;
    char line[80];
    snprintf(line, sizeof(line), "%lu,%d,%s,%s,%u/%u",
             (unsigned long)millis(), wifiIdsChannel(), d[i].name,
             sevTag(d[i].now), d[i].a, d[i].b);
    wlogRow(line);
    wrote = true;
  }
  if (wrote) wlogFlush();
}

// Stats block only (header + 3 detector rows). Redrawn once/second on a
// bounded fillRect, like the old deauth screen -- never uiClearBelow the
// whole content area, that flickers. The banner and the log below are
// redrawn only when they actually change.
static void drawStats() {
  tft.fillRect(0, HDR_Y, tft.width(), STATS_H, ILI9341_BLACK);
  tft.setTextSize(1);

  uint32_t el = (millis() - startMs) / 1000;
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(2, HDR_Y);
  tft.printf("ch%2d  %02lu:%02lu  seen %lu  drop %lu",
             wifiIdsChannel(), (unsigned long)(el / 60), (unsigned long)(el % 60),
             (unsigned long)gSeen, (unsigned long)wifiIdsDropped());

  tft.setTextColor(sevColor(vD.sev));
  tft.setCursor(2, D_Y);
  tft.printf("DEAUTH  %s", sevTag(vD.sev));
  tft.setCursor(2, D_Y + 11);
  tft.printf(" d%u dis%u  rc%u  spoof%u", vD.a, vD.b, vD.c, vD.d);

  tft.setTextColor(sevColor(vB.sev));
  tft.setCursor(2, B_Y);
  tft.printf("BEACON  %s", sevTag(vB.sev));
  tft.setCursor(2, B_Y + 11);
  tft.printf(" uniq~%u rnd%u laa%u seq%u b%u", vB.a, vB.b, vB.c, vB.d, vB.e);

  tft.setTextColor(sevColor(vA.sev));
  tft.setCursor(2, A_Y);
  tft.printf("AUTH/ASSOC  %s", sevTag(vA.sev));
  tft.setCursor(2, A_Y + 11);
  tft.printf(" req%u src~%u laa%u ->%02X%02X%02X",
             vA.a, vA.b, vA.c, vATop[3], vATop[4], vATop[5]);

  tft.setCursor(2, R_Y);
  if (!rogueOn) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.print("ROGUE-AP  no baseline, run Rogue AP");
  } else {
    tft.setTextColor(vRogue ? ILI9341_RED : ILI9341_GREEN);
    tft.printf("ROGUE-AP  %s  (baseline %d)", vRogue ? "HIT" : "watching", rogueApCount());
  }
}

static uint16_t bannerKey() {
  return (uint16_t)(vD.sev | (vB.sev << 2) | (vA.sev << 4) |
                    (vSpoofFlag ? 0x40 : 0) | (vRogue ? 0x80 : 0) |
                    (vPwn ? 0x100 : 0) | (vTwin ? 0x200 : 0));
}

static void drawBanner() {
  uint16_t key = bannerKey();
  if (key == lastBannerKey) return;
  lastBannerKey = key;

  uint8_t worst = vD.sev;
  if (vB.sev > worst) worst = vB.sev;
  if (vA.sev > worst) worst = vA.sev;
  if (vRogue && worst < SEV_ALERT) worst = SEV_ALERT;   // a baseline mismatch is an alert
  if (vTwin  && worst < SEV_ALERT) worst = SEV_ALERT;   // scored evil twin, no baseline needed
  if (vPwn   && worst < SEV_WATCH) worst = SEV_WATCH;   // harvester in range -- note, not an attack

  tft.fillRect(4, BANNER_Y, tft.width() - 8, BANNER_H,
               worst == SEV_ALERT ? ILI9341_RED : ILI9341_BLACK);

  if (worst == SEV_ALERT) {
    char what[48]; what[0] = 0;
    if (vSpoofFlag)               strncat(what, "DEAUTH-SPOOF ", 20);
    else if (vD.sev == SEV_ALERT) strncat(what, "DEAUTH-FLOOD ", 20);
    if (vB.sev == SEV_ALERT)      strncat(what, "BEACON-FLOOD ", 20);
    if (vA.sev == SEV_ALERT)      strncat(what, "AUTH-FLOOD ", 20);
    if (vRogue)                   strncat(what, "ROGUE-AP ", 20);
    if (vTwin)                    strncat(what, "EVIL-TWIN? ", 20);
    if (vPwn)                     strncat(what, "PWN ", 20);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, BANNER_Y + 4);
    tft.print("ATTACK LIKELY");
    tft.setTextSize(1);
    tft.setCursor(10, BANNER_Y + 26);
    tft.print(what);
    if (!alerted) { alerted = true; ledSet(true); beep(400, 1200); }
  } else {
    tft.setTextSize(1);
    tft.setTextColor(worst == SEV_WATCH ? ILI9341_YELLOW : ILI9341_GREEN);
    tft.setCursor(10, BANNER_Y + 16);
    tft.print(worst == SEV_WATCH ? (vPwn ? "Pwnagotchi in range" : "elevated -- watching")
                                 : "no attack indicators");
    if (alerted) { alerted = false; ledSet(false); }
  }
}

static void drawLog() {
  if (!alogDirty) return;
  alogDirty = false;
  tft.fillRect(0, LOG_Y, tft.width(), tft.height() - LOG_Y, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_RED);
  for (uint8_t i = 0; i < alogN; i++) {
    tft.setCursor(2, LOG_Y + i * 12);
    tft.print(alog[i]);
  }
}

// ---- lifecycle ----------------------------------------------------

void widsEnter() {
  uiDrawTopBar("WiFi IDS");
  uiClearBelow(29);
  s_jumpRogue = false;

  rogueApLoad();
  rogueOn = rogueApLoaded() && rogueApCount() > 0;

  // No baseline -> offer to go learn one, or run IDS without evil-twin detection.
  if (!rogueOn) {
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(6, 40);  tft.print("No rogue-AP baseline learned.");
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(6, 56);  tft.print("Evil-twin detection here needs one --");
    tft.setCursor(6, 68);  tft.print("learn it in the Rogue AP screen.");
    Btn go   = {6, 92,  tft.width() - 12, 38, "Go to Rogue AP"};
    Btn cont = {6, 138, tft.width() - 12, 38, "Continue anyway"};
    uiDrawMenuButton(go);
    uiDrawMenuButton(cont);
    for (;;) {
      TouchPoint t = uiReadTouch();
      if (!t.pressed) { delay(15); continue; }
      if (uiTouchInButton(t, go))   { s_jumpRogue = true; uiWaitForRelease(); return; }
      if (uiTouchInButton(t, cont) || uiTouchInBackArea(t)) { uiWaitForRelease(); break; }
    }
    uiDrawTopBar("WiFi IDS");
    uiClearBelow(29);
  }

  beepHold(true);          // the banner chirps on the OK->ALERT edge
  vRogue = false;
  vPwn = vTwin = pwnLogged = false;
  rogueSeenN = 0;

  memset(aps, 0, sizeof aps);
  memset(bRecent, 0, sizeof bRecent); bRecentN = 0;
  memset(alog, 0, sizeof alog); alogN = 0; alogDirty = false;
  vD = vB = vA = { SEV_OK, 0, 0, 0, 0, 0 };
  memset(vATop, 0, sizeof vATop);
  vSpoofFlag = false;
  gSeen = 0;
  dLastReason = 0;
  lastBannerKey = 0xFFFF;
  alerted = false;
  resetWindow();
  dropAtWindow = 0;

  wifiIdsBegin();
  wifiIdsSetDwell(300);    // keep the screen's historical 300 ms hop cadence
  hD = wifiIdsRegister(&onDeauthFamily, nullptr, WIDS_MASK_DEAUTH_FAMILY);
  hB = wifiIdsRegister(&onBeacon,       nullptr, WIDS_BIT(WIDS_BEACON));
  hA = wifiIdsRegister(&onAuthAssoc,    nullptr,
                       WIDS_BIT(WIDS_AUTH) | WIDS_BIT(WIDS_ASSOC_REQ) | WIDS_BIT(WIDS_REASSOC_REQ));

  startMs = windowStart = millis();
  lastStatsDraw = 0;
  lastWlogSummary = millis();
  running = true;

  wlogOpen("wifiids", "millis,channel,detector,severity,detail");   // event rows only

  drawStats();
  drawBanner();
}

void widsLoop() {
  if (!running) return;
  wifiIdsLoop();            // pump hopper + drain captures into the 3 callbacks

  uint32_t now = millis();
  if (now - lastStatsDraw > 1000) { drawStats(); lastStatsDraw = now; }

  if (now - windowStart >= WINDOW_MS) {
    uint8_t pd = vD.sev, pb = vB.sev, pa = vA.sev;
    computeVerdicts();
    logEdges(pd, pb, pa);
    wlogVerdictEdges(pd, pb, pa);   // SD: any severity transition, either direction
    drawBanner();
    drawLog();
    drawStats();           // repaint immediately with the fresh window's numbers
    lastStatsDraw = now;
    resetWindow();
    windowStart = now;
  }

  // SD: one summary row per minute regardless of transitions.
  if (wlogIsOpen() && now - lastWlogSummary >= 60000) {
    lastWlogSummary = now;
    uint8_t worst = vD.sev;
    if (vB.sev > worst) worst = vB.sev;
    if (vA.sev > worst) worst = vA.sev;
    char line[96];
    snprintf(line, sizeof(line), "%lu,%d,summary,%s,d%u/b%u/a%u",
             (unsigned long)millis(), wifiIdsChannel(), sevTag(worst),
             (unsigned)(vD.a + vD.b), vB.a, vA.a);
    wlogRow(line);
    wlogFlush();
  }
}

void widsTouch(const TouchPoint &t) {
  if (!t.isNewPress) return;
  if (t.y < BANNER_Y) return;                 // taps land on the banner / log area
  alogN = 0;
  memset(alog, 0, sizeof alog);
  alogDirty = true;
  alerted = false;
  vRogue = false;                             // re-arm rogue-AP alerting
  vPwn = vTwin = pwnLogged = false;
  rogueSeenN = 0;
  ledSet(false);
  lastBannerKey = 0xFFFF;                     // force a banner repaint
  drawBanner();
  drawLog();
}

void widsExit() {
  if (!running) return;   // widsEnter bailed to the Rogue AP screen -- nothing came up
  running = false;
  wlogClose();
  wifiIdsUnregister(hD); wifiIdsUnregister(hB); wifiIdsUnregister(hA);
  hD = hB = hA = -1;
  wifiIdsEnd();            // last ref: drops promiscuous, restores WIFI_MODE_NULL
  beepHold(false);
  ledSet(false);
}
