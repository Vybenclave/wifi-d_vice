// WiFi IDS -- the WiFi-attack-detection umbrella screen (DESIGN.md section 3).
//
// This is the old "Deauth Detect" screen grown into the umbrella the shared
// wifi_ids core was built for: ONE promiscuous session, ONE hop schedule,
// nine passive detectors registered against it, one unified verdict area.
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
//   * Rogue-AP / evil-twin (SD baseline) + a baseline-free evil-twin score,
//     and Pwnagotchi-presence -- see rogue_ap.cpp and onBeacon() below.
//   * KARMA -- one BSSID answering probe requests for several different
//     SSIDs (a rogue AP impersonating every network a nearby client has
//     ever joined). Management-frame only, no core changes needed.
//   * Channel-switch-announcement (CSA) abuse -- an off-regulatory-domain
//     target channel, or a CSA re-announced/re-targeted without a real
//     switch ever happening (the mdk4-style disruption signature).
//     Management-frame only (action frames), no core changes needed.
//   * Handshake-theft correlation -- a deauth for client X, then an EAPOL
//     4-way handshake for X on the same BSSID within seconds.
//   * PMKID-harvest -- an EAPOL message-1 with no matching recent real
//     association for that (STA,BSSID) pair (a clientless direct-association
//     PMKID grab, hcxdumptool-style).
//   * KRACK -- the same EAPOL message-3 replay counter seen twice for one
//     (STA,BSSID) pair (the 2017 Vanhoef key-reinstallation signature).
//   * WPS brute-force -- WSC/EAP-Packet attempt rate per BSSID
//     (Reaver/Bully-style PIN brute force).
// The last four need the EAPOL/WPS data-frame path wifi_ids.cpp exposes via
// wifiIdsWantEapol()/wifiIdsRegisterEapol()/wifiIdsRegisterWps() -- still
// fully passive (EAPOL/WPS frames are themselves unencrypted; they ARE the
// key exchange / provisioning handshake), but the one place this core looks
// at anything other than a management frame. See wifi_ids.h for why that's
// safe to do unconditionally cheap when no screen asks for it.
//
// Why one screen and not nine: the wifi_ids core has a single RX-callback
// slot and one hop schedule; nine screens would each spin the radio up and
// down and each carry a near-identical Enter/Loop/Exit + draw + window
// harness. One screen = one wifiIdsBegin(), N wifiIdsRegister() calls,
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
//   KARMA         8 * 34 B  = 272 B
//   CSA targets   4 * 10 B  =  40 B
//   deauth victims 6 * 16 B =  96 B
//   assoc-seen    8 * 16 B  = 128 B
//   PMKID-wait    6 * 15 B  =  90 B
//   KRACK table   6 * 16 B  =  96 B
//   WPS targets   4 * 8 B   =  32 B
//   + scalars                ~160 B
//   ------------------------------  ~ 1.4 KB total, all static.
// Nothing here is big enough to need calloc/free like the wifi_ids ring
// (the EAPOL/WPS ring itself is wifi_ids.cpp's, calloc'd there, not here).

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ui.h"
#include "wifi_ids.h"
#include "wlog.h"
#include "rogue_ap.h"
#include "devtime.h"
#include "keyboard.h"
#include "accent.h"
#include "debuglog.h"

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
  char     ssid[24]; // sanitized SSID text, display only (channel-lock SSID picker)
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
static int      hK = -1, hCsa = -1, hEapol = -1, hWps = -1;

bool widsTakeJumpToRogue() { bool j = s_jumpRogue; s_jumpRogue = false; return j; }
static uint32_t startMs, windowStart, lastStatsDraw;
static uint32_t lastBannerKey = 0xFFFFFFFFu;   // widened from uint16_t -- the CL state adds two more bits
// KARMA/PMKID/WPS severities are computed live from continuously-updated
// tables (no "raw counters reset + recompute" step like vD/vB/vA get from
// computeVerdicts()), so there's nothing to naturally compare "before" vs
// "after" within one window-boundary check -- these three cache the value
// as of the END of the last check, updated right before resetWindow().
static uint8_t lastKarmaSev = 0, lastPmkidSev = 0, lastWpsSev = 0;   // SEV_OK == 0
static bool     alerted = false;
static uint32_t lastWlogSummary = 0;   // event-level SD log: one summary row / minute

// ---- layout -----------------------------------------------------
static const int HDR_Y    = 30;
static const int D_Y      = 46;
static const int B_Y      = 76;
static const int A_Y      = 104;
static const int R_Y      = 126;                   // rogue-AP status line
static const int N_Y      = R_Y + 11;              // KARMA/CSA/HS/PMKID/KRACK/WPS one-line status
static const int STATS_H  = (N_Y + 11) - HDR_Y;
static const int BANNER_Y = N_Y + 15;
static const int BANNER_H  = 44;
static const int LOG_Y    = BANNER_Y + BANNER_H + 2;

// The channel readout on the stats header (drawStats()) is plain text --
// NOT a button. Tried making it double as the channel-lock options
// trigger three different ways (a bordered box, a tiny gear icon, a real
// button behind the readout's own severity-colored text) and none of it
// looked or worked right, so the options entry point is now its own,
// separate, unambiguous button instead -- see optionsBtnRect() below.
static const int CHAN_X = 2;

// Bottom-right "Options" button: opens the channel-lock picker
// (clOpenPicker()). Deliberately separate from the channel readout above
// (see note there). A real uiDrawButton() at this app's established
// button size -- h=20 is the minimum that lets its label hit the
// standard UI_MENU_BTN_MAXSIZE=2 text (uiDrawButton() needs bh <= h-4),
// w=92 fits "Options" at that size with room to spare, same numbers that
// already proved out comfortably on this screen's old channel-lock
// button attempt. Sits in the screen's bottom-right corner, drawn last
// in drawLog() so it always paints on top of whatever the event log
// draws underneath -- cheaper and more robust than carving a dedicated
// row out of the log's already-tight vertical space in landscape
// (~2 log lines to begin with).
//
// Gap from the status bar: confirmed via on-device touch logging that a
// tight 2px gap (hugging the bar) put the button right at the physical
// bottom edge, where this resistive touchscreen's accuracy drops off --
// every real tap landed a few px ABOVE the button, never inside it.
// Portrait has plenty of spare room below this screen's fixed,
// landscape-tuned stats/banner/log offsets, so it gets a generous 16px
// gap there; landscape keeps the original tight 2px since that
// orientation has (deliberately) almost no room to spare.
static const int OPT_BTN_W = 92, OPT_BTN_H = 20;
static Btn optionsBtnRect() {
  bool portrait = tft.height() > tft.width();
  int gap = portrait ? 16 : 2;
  return { tft.width() - OPT_BTN_W - 2, tft.height() - UI_STATUSBAR_H - gap - OPT_BTN_H,
           OPT_BTN_W, OPT_BTN_H, "Options" };
}

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

// forward decls -- defined in the handshake-theft/PMKID/KRACK section below,
// called from here and from onAuthAssoc
static void dvRecord(const uint8_t *sta, const uint8_t *bssid);
static void asRecord(const uint8_t *bssid, const uint8_t *sta);
// defined in the channel-lock section below, called from onBeacon
static void clOnBeacon(const WifiIdsFrame &f, const uint8_t *b, const uint8_t *s, uint8_t sl, bool haveSsid);

static void onDeauthFamily(const WifiIdsFrame &f, void *) {
  gSeen++;
  if (f.subtype == WIDS_DEAUTH) dDeauth++;
  else                          dDisassoc++;
  if (f.rawLen >= 26) dLastReason = (uint16_t)(f.raw[24] | (f.raw[25] << 8));

  // handshake-theft: remember who got deauthed by whom, so a follow-up
  // EAPOL-M2 for the same (STA,BSSID) pair within a few seconds can be
  // recognized as "attacker forced a reconnect to capture the handshake".
  // addr1 = the deauth target (STA); addr3 is the BSSID on every shape this
  // detector cares about (addr2==addr3 is the forged-from-the-AP-itself
  // case already checked below; a third-party deauth still names the real
  // BSSID in addr3).
  dvRecord(f.addr1, f.addr3);

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

static void checkCsaIeInBeacon(const WifiIdsFrame &f);   // defined in the CSA-abuse section below

static void onBeacon(const WifiIdsFrame &f, void *) {
  gSeen++;
  bBeacons++;
  const uint8_t *b = f.addr3;                 // BSSID

  checkCsaIeInBeacon(f);
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

  clOnBeacon(f, b, s, sl, haveSsid);

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
        char wl[112];
        snprintf(wl, sizeof wl, "%s,%d,pwnagotchi,watch,%s %ddBm",
                 devTimeNowString().c_str(), f.channel, nm, f.rssi);
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
        char wl[112];
        snprintf(wl, sizeof wl, "%s,%d,rogue,%s,%s %02X%02X%02X ch%u %ddBm",
                 devTimeNowString().c_str(), f.channel, rogueKindTag(rk), es,
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
            char wl[112];
            snprintf(wl, sizeof wl,
                     "%s,%d,eviltwin,alert,%s %02X%02X%02X vs %02X%02X%02X sc%d",
                     devTimeNowString().c_str(), f.channel, es,
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
    if (haveSsid) {
      uint8_t n = sl < sizeof(a->ssid) - 1 ? sl : sizeof(a->ssid) - 1;
      memcpy(a->ssid, s, n); a->ssid[n] = 0;
      for (uint8_t i = 0; i < n; i++) if (a->ssid[i] < 0x20 || a->ssid[i] > 0x7E) a->ssid[i] = '.';
    } else {
      a->ssid[0] = 0;
    }
  }
}

static void onAuthAssoc(const WifiIdsFrame &f, void *) {
  gSeen++;
  aReq++;
  const uint8_t *src = f.addr2;
  const uint8_t *bss = f.addr3;

  bloomAdd(aSrcBloom, src, 6);
  if (src[0] & 0x02) aLaaSrc++;

  // PMKID-harvest baseline: an assoc/reassoc-request is the normal client
  // connect path. Record it so a later EAPOL-M1 for this (STA,BSSID) pair
  // with NO recent entry here looks like a clientless direct-association
  // PMKID grab instead of an ordinary connect. WIDS_AUTH frames also land
  // here (this detector's mask includes it) but only assoc/reassoc mark a
  // real connection attempt -- an auth-only exchange isn't one yet.
  if (f.subtype == WIDS_ASSOC_REQ || f.subtype == WIDS_REASSOC_REQ) asRecord(bss, src);

  Tgt *t = nullptr;
  int minSlot = 0;
  for (int i = 0; i < TGT_N; i++) {
    if (tgts[i].hits && !memcmp(tgts[i].bssid, bss, 6)) { t = &tgts[i]; break; }
    if (tgts[i].hits < tgts[minSlot].hits) minSlot = i;
  }
  if (!t) { t = &tgts[minSlot]; memcpy(t->bssid, bss, 6); t->hits = 0; }
  t->hits++;
}

// ---- KARMA: one BSSID answering probe requests for many different SSIDs --
//
// Session-scoped (not the 5 s flood window -- KARMA is a slow-forming
// pattern across many probe/response pairs, not a rate spike). Exact SSID
// hashes per BSSID, not a bloom filter: too few BSSIDs/SSIDs at this scale
// for a bloom filter to pay off over just storing up to 4 hashes directly.
struct KarmaBssid {
  uint8_t  bssid[6];
  uint32_t ssidHash[4];
  uint8_t  ssidN, overflow;   // overflow = answered a 5th+ distinct SSID
  uint16_t respCount;
  uint32_t lastSeen;
};
static const int KARMA_N = 8;
static KarmaBssid karma[KARMA_N];

static KarmaBssid *karmaFind(const uint8_t *b) {
  for (int i = 0; i < KARMA_N; i++)
    if (karma[i].lastSeen && !memcmp(karma[i].bssid, b, 6)) return &karma[i];
  return nullptr;
}
static KarmaBssid *karmaLruSlot() {
  int o = 0;
  for (int i = 1; i < KARMA_N; i++) if (karma[i].lastSeen < karma[o].lastSeen) o = i;
  return &karma[o];
}

static void onKarmaProbeResp(const WifiIdsFrame &f, void *) {
  if (f.rawLen < 38) return;
  const uint8_t *ie = f.raw + 36;      // probe-resp fixed params are 12 B, same offset as beacons
  if (ie[0] != 0) return;
  uint8_t l = ie[1];
  if (l == 0 || 38 + l > f.rawLen) return;   // blank-SSID response -- nothing to attribute
  uint32_t h = ssidHash32(ie + 2, l);

  KarmaBssid *k = karmaFind(f.addr3);
  if (!k) { k = karmaLruSlot(); memset(k, 0, sizeof(*k)); memcpy(k->bssid, f.addr3, 6); }
  k->respCount++;
  k->lastSeen = millis();
  bool known = false;
  for (uint8_t i = 0; i < k->ssidN; i++) if (k->ssidHash[i] == h) { known = true; break; }
  if (!known) {
    if (k->ssidN < 4) k->ssidHash[k->ssidN++] = h;
    else k->overflow++;
  }
}

// WATCH: one BSSID answered 3+ distinct SSIDs (a legit dual-SSID consumer
// mesh AP tops out at 2). ALERT: 5+ distinct (or any 5th-plus "overflow"),
// corroborated by respCount>=6 so one truncated-IE misparse can't trip it.
static uint8_t sevKarma() {
  uint8_t worst = SEV_OK;
  for (int i = 0; i < KARMA_N; i++) {
    KarmaBssid &k = karma[i];
    if (!k.lastSeen) continue;
    uint8_t distinct = (uint8_t)(k.ssidN + k.overflow);
    uint8_t s = SEV_OK;
    if ((k.overflow || distinct >= 5) && k.respCount >= 6) s = SEV_ALERT;
    else if (distinct >= 3) s = SEV_WATCH;
    if (s > worst) worst = s;
  }
  return worst;
}

// ---- Channel-switch-announcement (CSA) abuse --------------------------
//
// CSA IE (id 37: mode, newChannel, count) can appear in an ordinary beacon
// (checked from onBeacon above) or a dedicated Spectrum-Management action
// frame (category 0, action 4 -- action frames, subtype 0xD, already flow
// through the core once a detector subscribes to them, same precedent
// drone_detect.cpp uses for Remote ID).
struct CsaTgt { uint8_t bssid[6]; uint16_t count; uint8_t lastCh; bool varied; uint32_t lastSeen; };
static const int CSA_N = 4;
static CsaTgt  csaTgts[CSA_N];
static uint16_t csaOffRegdomain;   // hard protocol violations this window

static CsaTgt *csaFind(const uint8_t *b) {
  for (int i = 0; i < CSA_N; i++)
    if (csaTgts[i].lastSeen && !memcmp(csaTgts[i].bssid, b, 6)) return &csaTgts[i];
  return nullptr;
}
static CsaTgt *csaLruSlot() {
  int o = 0;
  for (int i = 1; i < CSA_N; i++) if (csaTgts[i].lastSeen < csaTgts[o].lastSeen) o = i;
  return &csaTgts[o];
}

static bool csaOffLogged;   // one alert-log line per session for the hard-violation case

static void csaLog(const char *line, const uint8_t *bssid, const char *detail) {
  alogPush(line);
  if (wlogIsOpen()) {
    char wl[112];
    snprintf(wl, sizeof wl, "%s,%d,csa_abuse,alert,%02X%02X%02X %s",
             devTimeNowString().c_str(), wifiIdsChannel(), bssid[3], bssid[4], bssid[5], detail);
    wlogRow(wl); wlogFlush();
  }
}

static void handleCsa(const uint8_t *bssid, uint8_t newCh) {
  if (newCh == 0 || newCh > 13) {   // no legit AP can switch to this channel
    csaOffRegdomain++;
    if (!csaOffLogged) {
      csaOffLogged = true;
      uint32_t el = (millis() - startMs) / 1000;
      char line[26];
      snprintf(line, sizeof line, "%02lu:%02lu CSA BAD-CH%u %02X%02X%02X",
               el / 60, el % 60, newCh, bssid[3], bssid[4], bssid[5]);
      csaLog(line, bssid, "off-regdomain");
    }
    return;
  }

  CsaTgt *c = csaFind(bssid);
  if (!c) { c = csaLruSlot(); memset(c, 0, sizeof(*c)); memcpy(c->bssid, bssid, 6); }
  if (c->count && c->lastCh != newCh) c->varied = true;
  c->lastCh = newCh;
  c->count++;
  c->lastSeen = millis();
  if (c->count == 3 && c->varied) {   // crossed the ALERT threshold -- log once, not every CSA
    uint32_t el = (millis() - startMs) / 1000;
    char line[26];
    snprintf(line, sizeof line, "%02lu:%02lu CSA-ABUSE %02X%02X%02X ->ch%u",
             el / 60, el % 60, bssid[3], bssid[4], bssid[5], newCh);
    csaLog(line, bssid, "re-announced/varied");
  }
}

static void checkCsaIeInBeacon(const WifiIdsFrame &f) {
  if (f.rawLen < 38) return;
  const uint8_t *ie = f.raw + 36;
  int n = f.rawLen - 36, i = 0;
  while (i + 2 <= n) {
    uint8_t id = ie[i], l = ie[i + 1];
    if (i + 2 + l > n) break;
    if (id == 37 && l >= 3) { handleCsa(f.addr3, ie[i + 3]); return; }
    i += 2 + l;
  }
}

// Action-frame body starts right after the 24-byte header (no fixed params,
// unlike beacons) -- same offset drone_detect.cpp's onAction uses.
static void onCsaAction(const WifiIdsFrame &f, void *) {
  if (f.rawLen < 24 + 2 + 5) return;
  if (f.raw[24] != 0 || f.raw[25] != 4) return;    // category 0 / action 4 = Spectrum-Mgmt CSA
  const uint8_t *ie = f.raw + 26;
  if (ie[0] != 37 || ie[1] < 3) return;
  handleCsa(f.addr3, ie[3]);
}

// ALERT: a hard off-regdomain target channel (no corroboration needed -- a
// real AP never announces a switch to a channel it can't use), or 3+ CSAs
// from one BSSID that re-announce without the channel ever settling (the
// mdk4-style disruption signature: re-announced without a real switch, or
// re-targeted each time). WATCH: a CSA from a BSSID with no fresh beacon
// baseline (apFind, same 10s freshness the deauth-spoof check uses) -- a
// newly-arrived legit AP could trigger this honestly.
static uint8_t sevCsa() {
  if (csaOffRegdomain) return SEV_ALERT;
  uint8_t worst = SEV_OK;
  for (int i = 0; i < CSA_N; i++) {
    CsaTgt &c = csaTgts[i];
    if (!c.lastSeen) continue;
    uint8_t s = SEV_OK;
    if (c.count >= 3 && c.varied) s = SEV_ALERT;
    else {
      Ap *a = apFind(c.bssid);
      if (!a || millis() - a->seen > 10000) s = SEV_WATCH;
    }
    if (s > worst) worst = s;
  }
  return worst;
}

// ---- Channel lock / SSID-protect mode: state (declared early -- onEapol
// below needs clMode to guard the transient PMKID pin). The implementation
// (clOnBeacon, serviceClMode, the header picker UI) lives in its own section
// further down, after the detectors it draws on (Ap, ssidHash32, alogPush,
// wifiIdsHopPin/Set/Resume) are all in scope. See the plan's "Context"
// section for why: watching one SSID/channel gives every detector here
// ~100% coverage of the protected network instead of the ~8% a free 13-
// channel hop leaves it, and the operator wants to know about a protected
// AP going silent as its own, genuinely useful alert.
enum ClMode : uint8_t {
  CL_AUTO = 0,        // default: free 1-13 hopping (today's behavior)
  CL_MANUAL,          // pinned/cycling a user-chosen 1-3 channels, no SSID tracking
  CL_SEARCHING,       // "lock to SSID" requested -- running a discovery sweep
  CL_LOCKED,          // found on 1-3 channels + hop-set to just those, actively monitored
  CL_REACQUIRING,     // one or more tracked nodes lost -- sweeping to find them again
  CL_DOWN,            // 3 sweep attempts found nothing at all -- down, slow periodic re-checks
};
static ClMode   clMode = CL_AUTO;
static char     clSsid[24];         // target SSID text
static uint32_t clSsidHash;         // ssidHash32(clSsid), computed once when a lock starts

// One tracked node, identified by BSSID (not just channel) -- that's what
// lets a reacquire sweep tell "this node relocated to a new channel" apart
// from "this is a different node", since channel alone is ambiguous once a
// node can move. Up to 3: 1 for manual-single/a just-starting SSID lock,
// more once mesh nodes are discovered during a sweep.
struct ClNode { uint8_t bssid[6]; uint8_t channel; uint32_t lastSeen; bool down; };
static ClNode   clNodes[3];
static uint8_t  clNodeN;
static uint8_t  clManualN;          // CL_MANUAL: channel count, for the header "+N" suffix

static uint8_t  clAttempts;         // consecutive sweeps that found NONE of the tracked nodes at all
static uint32_t clSweepStart;       // when the current discovery/reacquire sweep began
static uint32_t clDownRecheckAt;    // next slow re-check time while CL_DOWN
static bool     clDownRecheck;      // true: the CURRENT CL_REACQUIRING sweep is CL_DOWN's periodic
                                     // recheck, not a fresh node-loss reacquire out of CL_LOCKED --
                                     // decides whether a success logs "NETWORK-UP" and whether a
                                     // failure re-logs NETWORK-DOWN or just quietly reschedules.

// ---- handshake-theft correlation + PMKID-harvest + KRACK --------------
//
// All three ride the EAPOL-Key classification from wifi_ids.h
// (WifiIdsEapol); one registered callback (onEapol, below), dispatched
// internally by message number, so this uses one of the two available
// EAPOL detector slots.

// handshake-theft: onDeauthFamily (above) calls dvRecord() on every
// deauth/disassoc; onEapol consumes a matching entry on that pair's M2.
struct DeauthVictim { uint8_t sta[6], bssid[6]; uint32_t deauthAt; };
static const int DV_N = 6;
static DeauthVictim dv[DV_N];
static bool vHsTheft, hsTheftLogged;

static void dvRecord(const uint8_t *sta, const uint8_t *bssid) {
  int o = 0;
  for (int i = 0; i < DV_N; i++) {
    if (!dv[i].deauthAt) { o = i; break; }
    if (dv[i].deauthAt < dv[o].deauthAt) o = i;
  }
  memcpy(dv[o].sta, sta, 6);
  memcpy(dv[o].bssid, bssid, 6);
  dv[o].deauthAt = millis();
}

// PMKID-harvest: onAuthAssoc (above) calls asRecord() on every real
// assoc/reassoc-request, so an EAPOL-M1 with no recent entry for that
// (STA,BSSID) pair looks like a clientless direct-association PMKID grab.
struct AssocSeen { uint8_t bssid[6], sta[6]; uint32_t assocAt; };
static const int AS_N = 8;
static AssocSeen asTbl[AS_N];

static void asRecord(const uint8_t *bssid, const uint8_t *sta) {
  int o = 0;
  for (int i = 0; i < AS_N; i++) {
    if (!asTbl[i].assocAt) { o = i; break; }
    if (asTbl[i].assocAt < asTbl[o].assocAt) o = i;
  }
  memcpy(asTbl[o].bssid, bssid, 6);
  memcpy(asTbl[o].sta, sta, 6);
  asTbl[o].assocAt = millis();
}
static AssocSeen *asFind(const uint8_t *bssid, const uint8_t *sta) {
  for (int i = 0; i < AS_N; i++)
    if (asTbl[i].assocAt && !memcmp(asTbl[i].bssid, bssid, 6) && !memcmp(asTbl[i].sta, sta, 6))
      return &asTbl[i];
  return nullptr;
}

// Per-pair "waiting to see if M2 follows this M1" state -- a real client
// that fails retries with a NEW M1; a PMKID-grab tool sends exactly one M1
// and disconnects, so "M1 with no M2 within 5s" is itself corroborating
// evidence, on top of the distinct-STA-count signal below.
struct PmkidWait { uint8_t sta[6], bssid[6]; uint32_t m1At; bool armed; };
static const int PW_N = 6;
static PmkidWait pmkidWait[PW_N];
static uint16_t  pmkidSusp;         // this window's "M1, no prior assoc" count
static uint8_t   pmkidStas[8][6];   // distinct STA MACs behind pmkidSusp, this window
static uint8_t   pmkidStaN;
// A genuine M2 reply follows M1 within single-digit ms, on the SAME channel
// -- but the shared hopper moves every 300ms regardless, so catching M1
// right as we're about to leave that channel means missing an entirely
// normal M2 purely on timing, not because anything's wrong. Briefly pin the
// hopper to the M1's channel for a first-ever (unvetted) sighting, long
// enough to catch an immediate legitimate reply. 0 = not pinned for this.
static uint32_t  pmkidPinUntil;

static PmkidWait *pwFind(const uint8_t *bssid, const uint8_t *sta) {
  for (int i = 0; i < PW_N; i++)
    if (pmkidWait[i].armed && !memcmp(pmkidWait[i].bssid, bssid, 6) && !memcmp(pmkidWait[i].sta, sta, 6))
      return &pmkidWait[i];
  return nullptr;
}
static PmkidWait *pwSlot() {
  int o = 0;
  for (int i = 1; i < PW_N; i++) if (pmkidWait[i].m1At < pmkidWait[o].m1At) o = i;
  return &pmkidWait[o];
}

// KRACK: per-(STA,BSSID) last-seen EAPOL-M3 replay-counter high bits -- a
// repeat of the same counter for the same pair is a message-3 replay (the
// 2017 Vanhoef key-reinstallation signature).
struct M3Seen { uint8_t sta[6], bssid[6]; uint16_t replayHi; uint32_t seen; };
static const int M3_N = 6;
static M3Seen m3tbl[M3_N];
static bool   vKrack, krackLogged;

static M3Seen *m3Find(const uint8_t *bssid, const uint8_t *sta) {
  for (int i = 0; i < M3_N; i++)
    if (m3tbl[i].seen && !memcmp(m3tbl[i].bssid, bssid, 6) && !memcmp(m3tbl[i].sta, sta, 6))
      return &m3tbl[i];
  return nullptr;
}
static M3Seen *m3Slot() {
  int o = 0;
  for (int i = 1; i < M3_N; i++) if (m3tbl[i].seen < m3tbl[o].seen) o = i;
  return &m3tbl[o];
}

// Pairs that completed at least one full handshake (M1 then M2) this
// session. A later M1 for the same pair with no fresh assoc is then a
// routine PTK rekey of an already-vetted client, not fresh suspicion --
// without this, every already-connected device's periodic rekey (which
// never produces a new assoc frame at all, ever) re-triggers PMKID
// suspicion on every rekey for as long as the screen stays open, not just
// at startup. Session-persistent (reset only at widsEnter(), not
// widsTouch() -- dismissing an alert shouldn't make the detector forget
// which clients it already vetted), same as asTbl/dv/m3tbl above.
struct KnownGoodPair { uint8_t sta[6], bssid[6]; };
static const int KG_N = 8;
static KnownGoodPair kgTbl[KG_N];
static uint8_t kgNext;   // round-robin slot -- no "last seen" field to rank by

static bool kgFind(const uint8_t *bssid, const uint8_t *sta) {
  for (int i = 0; i < KG_N; i++)
    if (!memcmp(kgTbl[i].bssid, bssid, 6) && !memcmp(kgTbl[i].sta, sta, 6)) return true;
  return false;
}
static void kgRecord(const uint8_t *bssid, const uint8_t *sta) {
  if (kgFind(bssid, sta)) return;
  memcpy(kgTbl[kgNext].bssid, bssid, 6);
  memcpy(kgTbl[kgNext].sta, sta, 6);
  kgNext = (uint8_t)((kgNext + 1) % KG_N);
}

static void onEapol(const WifiIdsEapol &e, void *) {
  if (e.msg == WIDS_EAPOL_M2) {
    // handshake-theft: a real client answered -- stronger evidence than M1
    // alone (an AP can send M1 into dead air). Match against a recent
    // deauth for this exact (STA,BSSID) pair.
    for (int i = 0; i < DV_N; i++) {
      if (!dv[i].deauthAt) continue;
      if (memcmp(dv[i].sta, e.sta, 6) || memcmp(dv[i].bssid, e.bssid, 6)) continue;
      uint32_t gap = millis() - dv[i].deauthAt;
      dv[i].deauthAt = 0;   // consume -- don't re-fire on this handshake's later frames
      if (gap <= 10000) {
        vHsTheft = true;
        if (!hsTheftLogged) {
          hsTheftLogged = true;
          uint32_t el = (millis() - startMs) / 1000;
          char line[26];
          snprintf(line, sizeof line, "%02lu:%02lu HS-THEFT %02X%02X%02X %lums",
                   el / 60, el % 60, e.sta[3], e.sta[4], e.sta[5], (unsigned long)gap);
          alogPush(line);
          if (wlogIsOpen()) {
            char wl[128];
            snprintf(wl, sizeof wl,
                     "%s,%d,handshake_theft,alert,sta=%02X%02X%02X bssid=%02X%02X%02X gap=%lums",
                     devTimeNowString().c_str(), e.channel, e.sta[3], e.sta[4], e.sta[5],
                     e.bssid[3], e.bssid[4], e.bssid[5], (unsigned long)gap);
            wlogRow(wl); wlogFlush();
          }
        }
      }
      break;
    }
    // a real M2 means this pair's handshake is proceeding normally -- clear
    // any pending "M1 with no M2" PMKID suspicion for it, and remember the
    // pair as vetted so a later rekey (no new assoc, possibly an hour from
    // now) doesn't re-raise suspicion from scratch.
    PmkidWait *pw = pwFind(e.bssid, e.sta);
    if (pw) pw->armed = false;
    kgRecord(e.bssid, e.sta);

  } else if (e.msg == WIDS_EAPOL_M1) {
    AssocSeen *a = asFind(e.bssid, e.sta);
    bool hadAssoc = (a && (millis() - a->assocAt) < 15000) || kgFind(e.bssid, e.sta);
    if (!hadAssoc) {
      pmkidSusp++;
      bool known = false;
      for (uint8_t i = 0; i < pmkidStaN; i++) if (!memcmp(pmkidStas[i], e.sta, 6)) { known = true; break; }
      if (!known && pmkidStaN < 8) memcpy(pmkidStas[pmkidStaN++], e.sta, 6);
      PmkidWait *pw = pwSlot();
      memcpy(pw->sta, e.sta, 6); memcpy(pw->bssid, e.bssid, 6);
      pw->m1At = millis(); pw->armed = true;
      // Maximize the chance of catching this pair's M2 reply, which happens
      // on this same channel within milliseconds -- don't just trust
      // whatever channel the hopper lands on next. Only if nothing's
      // already pinned for this reason (don't fight a second concurrent
      // unvetted M1 for a different pair; wifiIdsHopPin() doesn't nest), and
      // only in CL_AUTO -- a manual/SSID lock already owns the channel/hop-
      // set and this transient pin would otherwise fight it.
      if (!pmkidPinUntil && clMode == CL_AUTO) {
        wifiIdsHopPin(e.channel);
        pmkidPinUntil = millis() + 400;
      }
    }

  } else if (e.msg == WIDS_EAPOL_M3) {
    M3Seen *m = m3Find(e.bssid, e.sta);
    if (m && m->replayHi == e.replayCounterHi && (millis() - m->seen) < 15000) {
      vKrack = true;
      if (!krackLogged) {
        krackLogged = true;
        uint32_t el = (millis() - startMs) / 1000;
        char line[26];
        snprintf(line, sizeof line, "%02lu:%02lu KRACK %02X%02X%02X replay",
                 el / 60, el % 60, e.bssid[3], e.bssid[4], e.bssid[5]);
        alogPush(line);
        if (wlogIsOpen()) {
          char wl[128];
          snprintf(wl, sizeof wl, "%s,%d,krack,alert,bssid=%02X%02X%02X sta=%02X%02X%02X replay=%u",
                   devTimeNowString().c_str(), e.channel, e.bssid[3], e.bssid[4], e.bssid[5],
                   e.sta[3], e.sta[4], e.sta[5], e.replayCounterHi);
          wlogRow(wl); wlogFlush();
        }
      }
    }
    if (!m) m = m3Slot();
    memcpy(m->bssid, e.bssid, 6); memcpy(m->sta, e.sta, 6);
    m->replayHi = e.replayCounterHi; m->seen = millis();
  }
}

// ALERT: 2+ distinct STA MACs behind no-prior-assoc M1s against one target
// this window (one tool run against multiple targets), or any M1 that never
// saw a follow-up M2 within 5 s (see PmkidWait above). WATCH: a single
// no-prior-assoc M1 alone -- weak evidence (a missed assoc during a
// channel-hop gap, or a session-restart rekey, can produce this too).
static uint8_t sevPmkid() {
  bool m2NeverFollowed = false;
  for (int i = 0; i < PW_N; i++)
    if (pmkidWait[i].armed && (millis() - pmkidWait[i].m1At) > 5000) m2NeverFollowed = true;
  if (pmkidStaN >= 2 || m2NeverFollowed) return SEV_ALERT;
  if (pmkidSusp >= 1) return SEV_WATCH;
  return SEV_OK;
}

// ---- WPS brute-force ---------------------------------------------------
//
// Rate of WSC (WiFi Simple Config) EAP-Packet attempts per BSSID, in the
// same 5 s window the flood detectors use. The core (wifi_ids.cpp) already
// narrows this to Expanded-EAP-type frames carrying the WFA vendor id --
// this detector only has to count them.
struct WpsTgt { uint8_t bssid[6]; uint16_t count; uint32_t lastSeen; };
static const int WPS_N = 4;
static WpsTgt wpsTgts[WPS_N];

static WpsTgt *wpsFind(const uint8_t *b) {
  for (int i = 0; i < WPS_N; i++)
    if (wpsTgts[i].lastSeen && !memcmp(wpsTgts[i].bssid, b, 6)) return &wpsTgts[i];
  return nullptr;
}
static WpsTgt *wpsLruSlot() {
  int o = 0;
  for (int i = 1; i < WPS_N; i++) if (wpsTgts[i].lastSeen < wpsTgts[o].lastSeen) o = i;
  return &wpsTgts[o];
}
static void onWps(const WifiIdsWps &w, void *) {
  WpsTgt *t = wpsFind(w.bssid);
  if (!t) { t = wpsLruSlot(); memset(t, 0, sizeof(*t)); memcpy(t->bssid, w.bssid, 6); }
  t->count++;
  t->lastSeen = millis();
}
// Thresholds are a starting point, not yet tuned against real Reaver/Bully
// traffic -- see DESIGN.md / the plan's hardware-validation notes.
static uint8_t sevWps() {
  uint8_t worst = SEV_OK;
  for (int i = 0; i < WPS_N; i++) {
    if (!wpsTgts[i].lastSeen) continue;
    uint8_t s = SEV_OK;
    if (wpsTgts[i].count >= 10) s = SEV_ALERT;       // Reaver/Bully cadence, approximate
    else if (wpsTgts[i].count >= 3) s = SEV_WATCH;   // more than one manual provisioning attempt
    if (s > worst) worst = s;
  }
  return worst;
}

// ---- Channel lock / SSID-protect mode: implementation ------------------
//
// State (ClMode, clNodes[], etc.) is declared earlier, above onEapol, which
// needs clMode to guard the transient PMKID pin. Everything else -- the
// beacon hook, the state machine, and the header picker UI -- lives here.

static const uint32_t CL_NODE_LOST_MS    = 5000;   // no beacon from a tracked node's BSSID this long -> down
static const uint8_t  CL_MAX_ATTEMPTS    = 3;       // consecutive all-nodes-missing sweeps before CL_DOWN
static const uint32_t CL_DOWN_RECHECK_MS = 30000;   // periodic re-check cadence once CL_DOWN
static const uint32_t CL_SWEEP_MS        = 3900;    // one full 1..13 hop cycle at this screen's 300ms dwell

static bool clAnyNodeDown() {
  for (uint8_t i = 0; i < clNodeN; i++) if (clNodes[i].down) return true;
  return false;
}
static uint8_t clNotDownCount() {
  uint8_t c = 0;
  for (uint8_t i = 0; i < clNodeN; i++) if (!clNodes[i].down) c++;
  return c;
}
static void clRebuildHopSetFromNotDown() {
  uint8_t chs[3], n = 0;
  for (uint8_t i = 0; i < clNodeN && n < 3; i++) if (!clNodes[i].down) chs[n++] = clNodes[i].channel;
  if (n == 1)      wifiIdsHopPin(chs[0]);
  else if (n > 1)  wifiIdsHopSet(chs, n);
}

// Called from onBeacon() for every beacon, management-frame only -- a
// SSID-hash match against the active lock target is checked here, cheap for
// every other beacon in range. Tracks node found/lost/moved by BSSID so a
// channel move can be told apart from a different node entirely (see the
// ClNode comment above).
static void clOnBeacon(const WifiIdsFrame &f, const uint8_t *b, const uint8_t *s, uint8_t sl, bool haveSsid) {
  if (clMode != CL_SEARCHING && clMode != CL_LOCKED && clMode != CL_REACQUIRING && clMode != CL_DOWN) return;
  if (!haveSsid) return;
  if (ssidHash32(s, sl) != clSsidHash) return;

  bool sweeping = (clMode == CL_SEARCHING || clMode == CL_REACQUIRING);

  for (uint8_t i = 0; i < clNodeN; i++) {
    if (memcmp(clNodes[i].bssid, b, 6) != 0) continue;
    ClNode &n = clNodes[i];
    bool    wasDown = n.down;
    uint8_t oldCh   = n.channel;
    bool    moved   = (oldCh != f.channel);
    n.lastSeen = millis();
    n.channel  = f.channel;
    n.down     = false;
    if (moved) {
      uint32_t el = (millis() - startMs) / 1000;
      char line[26];
      snprintf(line, sizeof line, "%02lu:%02lu NODE-MOVED ch%d->ch%d", el / 60, el % 60, oldCh, f.channel);
      alogPush(line);
      if (wlogIsOpen()) {
        char wl[112];
        snprintf(wl, sizeof wl, "%s,%d,chanlock,watch,NODE-MOVED %02X%02X%02X ch%d->ch%d",
                 devTimeNowString().c_str(), f.channel, b[3], b[4], b[5], oldCh, f.channel);
        wlogRow(wl); wlogFlush();
      }
    } else if (wasDown) {
      uint32_t el = (millis() - startMs) / 1000;
      char line[26];
      snprintf(line, sizeof line, "%02lu:%02lu NODE-UP ch%d", el / 60, el % 60, f.channel);
      alogPush(line);
      if (wlogIsOpen()) {
        char wl[112];
        snprintf(wl, sizeof wl, "%s,%d,chanlock,watch,NODE-UP %02X%02X%02X ch%d",
                 devTimeNowString().c_str(), f.channel, b[3], b[4], b[5], f.channel);
        wlogRow(wl); wlogFlush();
      }
    }
    return;
  }

  // Genuinely new BSSID for this SSID -- only adopted during a deliberate
  // sweep (initial discovery or reacquire), never while just CL_LOCKED, so
  // a plain roaming client's probe-elicited response can't silently grow
  // the tracked set.
  if (sweeping && clNodeN < 3) {
    ClNode &n = clNodes[clNodeN++];
    memcpy(n.bssid, b, 6);
    n.channel  = f.channel;
    n.lastSeen = millis();
    n.down     = false;
  }
}

static void serviceClMode() {
  uint32_t now = millis();
  switch (clMode) {
    case CL_AUTO:
    case CL_MANUAL:
      break;   // nothing to drive -- no SSID target in either mode

    case CL_SEARCHING:
      if (now - clSweepStart > CL_SWEEP_MS) {
        if (clNodeN > 0) {
          uint8_t chs[3];
          for (uint8_t i = 0; i < clNodeN; i++) chs[i] = clNodes[i].channel;
          if (clNodeN == 1) wifiIdsHopPin(chs[0]); else wifiIdsHopSet(chs, clNodeN);
          clMode = CL_LOCKED;
          clAttempts = 0;
          uint32_t el = (now - startMs) / 1000;
          char line[26];
          if (clNodeN == 1)      snprintf(line, sizeof line, "%02lu:%02lu SSID found ->ch%d", el / 60, el % 60, chs[0]);
          else if (clNodeN == 2) snprintf(line, sizeof line, "%02lu:%02lu SSID found ->ch%d,%d", el / 60, el % 60, chs[0], chs[1]);
          else                   snprintf(line, sizeof line, "%02lu:%02lu SSID found ->ch%d,%d,%d", el / 60, el % 60, chs[0], chs[1], chs[2]);
          alogPush(line);
        } else {
          wifiIdsHopResume();   // keep sweeping -- no attempt cap on the initial search
          clSweepStart = now;
        }
      }
      break;

    case CL_LOCKED:
      for (uint8_t i = 0; i < clNodeN; i++) {
        if (clNodes[i].down) continue;
        if (now - clNodes[i].lastSeen <= CL_NODE_LOST_MS) continue;
        clNodes[i].down = true;
        uint32_t el = (now - startMs) / 1000;
        char line[26];
        snprintf(line, sizeof line, "%02lu:%02lu NODE-DOWN ch%d", el / 60, el % 60, clNodes[i].channel);
        alogPush(line);
        if (wlogIsOpen()) {
          char wl[112];
          snprintf(wl, sizeof wl, "%s,%d,chanlock,watch,NODE-DOWN %02X%02X%02X ch%d",
                   devTimeNowString().c_str(), wifiIdsChannel(),
                   clNodes[i].bssid[3], clNodes[i].bssid[4], clNodes[i].bssid[5], clNodes[i].channel);
          wlogRow(wl); wlogFlush();
        }
        // Losing even one node (not just the whole tracked set) kicks off a
        // reacquire sweep -- it may just have moved channels. The other,
        // still-good nodes keep getting their lastSeen refreshed too as the
        // sweep passes their channels, so this costs one bounded ~3.9s dip
        // in focus, not a false "network down".
        clDownRecheck = false;
        wifiIdsHopResume();
        clSweepStart = now;
        clMode = CL_REACQUIRING;
        break;   // one sweep covers every node; don't start a second
      }
      break;

    case CL_REACQUIRING:
      if (now - clSweepStart > CL_SWEEP_MS) {
        if (clNotDownCount() > 0) {
          clRebuildHopSetFromNotDown();
          clMode = CL_LOCKED;
          clAttempts = 0;
          if (clDownRecheck) {
            uint32_t el = (now - startMs) / 1000;
            char line[26];
            snprintf(line, sizeof line, "%02lu:%02lu NETWORK-UP", el / 60, el % 60);
            alogPush(line);
            if (wlogIsOpen()) {
              char wl[112];
              snprintf(wl, sizeof wl, "%s,%d,chanlock,ok,NETWORK-UP %s", devTimeNowString().c_str(), wifiIdsChannel(), clSsid);
              wlogRow(wl); wlogFlush();
            }
          }
        } else if (clDownRecheck) {
          // still gone -- stay down, reschedule, no repeat alert-log spam
          clMode = CL_DOWN;
          clDownRecheckAt = now + CL_DOWN_RECHECK_MS;
        } else {
          clAttempts++;
          if (clAttempts < CL_MAX_ATTEMPTS) {
            wifiIdsHopResume();
            clSweepStart = now;
          } else {
            clMode = CL_DOWN;
            clDownRecheckAt = now + CL_DOWN_RECHECK_MS;
            uint32_t el = (now - startMs) / 1000;
            char line[26];
            snprintf(line, sizeof line, "%02lu:%02lu NETWORK-DOWN", el / 60, el % 60);
            alogPush(line);
            if (wlogIsOpen()) {
              char wl[112];
              snprintf(wl, sizeof wl, "%s,%d,chanlock,alert,NETWORK-DOWN %s", devTimeNowString().c_str(), wifiIdsChannel(), clSsid);
              wlogRow(wl); wlogFlush();
            }
          }
        }
      }
      break;

    case CL_DOWN:
      if (now > clDownRecheckAt) {
        clDownRecheck = true;
        wifiIdsHopResume();
        clSweepStart = now;
        clMode = CL_REACQUIRING;
      }
      break;
  }
}

// ---- header picker UI: tap the channel number to change mode -----------

static const char *clModeItemLabel(int i) {
  static const char *items[3] = { "Auto-hop", "Lock channel...", "Lock to SSID..." };
  return items[i];
}
static const char *clChanItemLabel(int i) {
  static char buf[13][4];
  snprintf(buf[i], sizeof buf[i], "%d", i + 1);
  return buf[i];
}
static char     clPickLabel[AP_N + 1][24];
static uint32_t clPickHash[AP_N + 1];
static uint8_t  clPickN;
static const char *clSsidItemLabel(int i) { return clPickLabel[i]; }

static void clBuildSsidList() {
  clPickN = 0;
  for (int i = 0; i < AP_N && clPickN < AP_N; i++) {
    if (!aps[i].seen || !aps[i].ssidHash || !aps[i].ssid[0]) continue;
    bool dup = false;
    for (uint8_t j = 0; j < clPickN; j++) if (clPickHash[j] == aps[i].ssidHash) { dup = true; break; }
    if (dup) continue;
    strncpy(clPickLabel[clPickN], aps[i].ssid, 23); clPickLabel[clPickN][23] = 0;
    clPickHash[clPickN] = aps[i].ssidHash;
    clPickN++;
  }
  strncpy(clPickLabel[clPickN], "+ Type manually...", 23); clPickLabel[clPickN][23] = 0;
  clPickHash[clPickN] = 0;
  clPickN++;
}

static void clStartSsidLock(const char *ssid, uint32_t hash) {
  strncpy(clSsid, ssid, sizeof(clSsid) - 1); clSsid[sizeof(clSsid) - 1] = 0;
  clSsidHash = hash ? hash : ssidHash32((const uint8_t *)ssid, (uint8_t)strlen(ssid));
  clMode = CL_SEARCHING;
  clNodeN = 0; memset(clNodes, 0, sizeof clNodes);
  clManualN = 0;
  clAttempts = 0;
  clDownRecheck = false;
  clSweepStart = millis();
  wifiIdsHopResume();
}

static void clPickChannels() {
  uint8_t picked[3]; uint8_t n = 0;
  for (;;) {
    int ch = uiDropdownPick("Lock channel", 13, clChanItemLabel, -1);
    if (ch < 0 || ch > 12) break;          // back/cancel this round
    uint8_t c = (uint8_t)(ch + 1);
    bool dup = false;
    for (uint8_t i = 0; i < n; i++) if (picked[i] == c) dup = true;
    if (!dup && n < 3) picked[n++] = c;
    if (n >= 3) break;

    uiClearBelow(0);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(6, 40); tft.print("Locked so far:");
    tft.setCursor(6, 54);
    for (uint8_t i = 0; i < n; i++) tft.printf("ch%d  ", picked[i]);
    Btn more = {6, 92,  tft.width() - 12, 38, "Add another channel"};
    Btn done = {6, 138, tft.width() - 12, 38, "Done"};
    uiDrawMenuButton(more);
    uiDrawMenuButton(done);
    bool addAnother = false;
    for (;;) {
      TouchPoint t = uiReadTouch();
      uiServiceChrome();
      if (!t.isNewPress) { delay(15); continue; }
      if (uiTouchInButton(t, more))                          { addAnother = true;  uiWaitForRelease(); break; }
      if (uiTouchInButton(t, done) || uiTouchInBackArea(t))  { addAnother = false; uiWaitForRelease(); break; }
    }
    if (!addAnother) break;
  }
  if (n == 0) return;   // cancelled before picking anything
  if (n == 1) wifiIdsHopPin(picked[0]); else wifiIdsHopSet(picked, n);
  clMode = CL_MANUAL;
  clManualN = n;
  clNodeN = 0;   // CL_MANUAL tracks no per-node state -- no SSID to match against
}

static void clPickSsid() {
  clBuildSsidList();
  if (clPickN == 1) {   // only "+ Type manually..." -- aps[] has nothing yet
    uiToast("No SSIDs seen yet -- type one");
    String typed = uiTextInput("SSID to protect", clSsid);
    if (typed.length() == 0) return;
    clStartSsidLock(typed.c_str(), 0);
    return;
  }
  int pick = uiDropdownPick("Lock to SSID", clPickN, clSsidItemLabel, -1);
  if (pick < 0 || pick >= (int)clPickN) return;   // cancelled
  if (clPickHash[pick] == 0) {
    String typed = uiTextInput("SSID to protect", clSsid);
    if (typed.length() == 0) return;
    clStartSsidLock(typed.c_str(), 0);
  } else {
    clStartSsidLock(clPickLabel[pick], clPickHash[pick]);
  }
}

// forward decls -- defined in the "drawing" section further down; needed
// here so picking a lock mode can force an immediate full repaint.
static void drawStats();
static void drawBanner();
static void drawLog();
static void resetStatsFields();

static void clRestoreScreen() {
  uiDrawTopBar("WiFi IDS");
  uiClearBelow(29);
  lastStatsDraw = 0;
  lastBannerKey = 0xFFFFFFFFu;
  alogDirty = true;
  // uiClearBelow() just wiped the whole stats band, but drawStats()'s
  // DEAUTH/BEACON/AUTH/rogue/channel/elapsed lines are each gated by
  // uiFieldChanged() against their own prevXxx buffer (see the comment
  // above resetStatsFields()) -- any line whose underlying value hasn't
  // actually changed since before the picker opened would otherwise be
  // skipped and stay blank instead of being reprinted onto the now-empty
  // background. Must reset those buffers here too, not just at
  // widsEnter(), since clRestoreScreen() also runs after every pick in
  // the channel-lock picker (clOpenPicker()).
  resetStatsFields();
  drawStats();
  drawBanner();
  drawLog();
}

static void clOpenPicker() {
  // Highlight whichever mode is actually active right now (green outline,
  // see uiDropdownPick()) instead of always opening blank -- Auto-hop is
  // naturally the default/first-listed option already (index 0), and now
  // it's also shown as the current one whenever that's genuinely the case,
  // rather than the picker implying nothing is selected.
  int current = (clMode == CL_AUTO) ? 0 : (clMode == CL_MANUAL) ? 1 : 2;
  int pick = uiDropdownPick("Channel lock", 3, clModeItemLabel, current);
  if (pick == 0) {
    wifiIdsHopResume();
    clMode = CL_AUTO;
    clNodeN = 0; clManualN = 0;
  } else if (pick == 1) {
    clPickChannels();
  } else if (pick == 2) {
    clPickSsid();
  }
  clRestoreScreen();
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
static KarmaBssid *karmaWorst() {
  KarmaBssid *w = nullptr;
  for (int i = 0; i < KARMA_N; i++) {
    if (!karma[i].lastSeen) continue;
    if (!w || (karma[i].ssidN + karma[i].overflow) > (w->ssidN + w->overflow)) w = &karma[i];
  }
  return w;
}
static WpsTgt *wpsWorst() {
  WpsTgt *w = nullptr;
  for (int i = 0; i < WPS_N; i++) {
    if (!wpsTgts[i].lastSeen) continue;
    if (!w || wpsTgts[i].count > w->count) w = &wpsTgts[i];
  }
  return w;
}

// rising edge OK/WATCH -> ALERT: log it (used for the scrolling list).
// pk/pp/pw are the previous window's KARMA/PMKID/WPS severities -- CSA's
// hard-violation and re-announce cases, and handshake-theft/KRACK, log
// directly from their own detector functions instead (one-shot "this IS
// the attack" events, not windowed rate trends).
static void logEdges(uint8_t pd, uint8_t pb, uint8_t pa, uint8_t pk, uint8_t pp, uint8_t pw) {
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
  uint8_t sk = sevKarma();
  if (sk == SEV_ALERT && pk != SEV_ALERT) {
    KarmaBssid *w = karmaWorst();
    if (w) snprintf(line, sizeof line, "%02lu:%02lu KARMA %02X%02X%02X x%u", el / 60, el % 60,
                     w->bssid[3], w->bssid[4], w->bssid[5], (unsigned)(w->ssidN + w->overflow));
    else   snprintf(line, sizeof line, "%02lu:%02lu KARMA detected", el / 60, el % 60);
    alogPush(line);
  }
  uint8_t sp = sevPmkid();
  if (sp == SEV_ALERT && pp != SEV_ALERT) {
    snprintf(line, sizeof line, "%02lu:%02lu PMKID-HARVEST x%u STAs", el / 60, el % 60, pmkidStaN);
    alogPush(line);
  } else if (sp == SEV_WATCH && pp == SEV_OK) {
    snprintf(line, sizeof line, "%02lu:%02lu PMKID?", el / 60, el % 60);
    alogPush(line);
  }
  uint8_t sw = sevWps();
  if (sw == SEV_ALERT && pw != SEV_ALERT) {
    WpsTgt *t = wpsWorst();
    if (t) snprintf(line, sizeof line, "%02lu:%02lu WPS-BRUTE %02X%02X%02X x%u", el / 60, el % 60,
                     t->bssid[3], t->bssid[4], t->bssid[5], t->count);
    else   snprintf(line, sizeof line, "%02lu:%02lu WPS-BRUTE detected", el / 60, el % 60);
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
  // PMKID and WPS are windowed (5s), like the flood detectors above.
  // KARMA/CSA/handshake-theft/PMKID-wait/KRACK tables are session-scoped
  // (slow-forming or one-shot patterns) and are NOT cleared here -- only at
  // widsEnter()/widsTouch(), same as the Ap/Tgt baseline tables.
  pmkidSusp = 0; pmkidStaN = 0;
  for (int i = 0; i < WPS_N; i++) wpsTgts[i].count = 0;
  dropAtWindow = wifiIdsDropped();
}

// ---- drawing ------------------------------------------------------

// Thin wrappers over the shared ui.h convention -- kept so the rest of this
// file's `sevColor(x.sev)` / `sevTag(x.sev)` call sites don't need a rename.
static uint16_t sevColor(uint8_t s) { return uiSevColor(s); }
static const char *sevTag(uint8_t s) { return uiSevTag(s); }

// Event-level SD log (never per frame): a row on ANY detector's severity
// change -- OK->WATCH/ALERT and back -- plus one summary row per minute
// from widsLoop(). Columns: millis,channel,detector,severity,detail.
// Handshake-theft and KRACK are excluded here -- they log immediately from
// their own detector (onEapol), not on a 5s-window edge, since the
// correlation itself IS the attack rather than a rate trend.
static void wlogVerdictEdges(uint8_t pd, uint8_t pb, uint8_t pa, uint8_t pk, uint8_t pp, uint8_t pw) {
  if (!wlogIsOpen()) return;
  struct { const char *name; uint8_t prev, now; uint16_t a, b; } d[6] = {
    { "deauth", pd, vD.sev, vD.a, vD.b },   // a=deauth  b=disassoc
    { "beacon", pb, vB.sev, vB.a, vB.b },   // a=uniq~   b=random-ssid
    { "auth",   pa, vA.sev, vA.a, vA.b },   // a=req     b=src~
    { "karma",  pk, sevKarma(), 0, 0 },
    { "pmkid",  pp, sevPmkid(), pmkidSusp, pmkidStaN },
    { "wps",    pw, sevWps(), 0, 0 },
  };
  bool wrote = false;
  for (int i = 0; i < 6; i++) {
    if (d[i].now == d[i].prev) continue;
    char line[96];
    snprintf(line, sizeof(line), "%s,%d,%s,%s,%u/%u",
             devTimeNowString().c_str(), wifiIdsChannel(), d[i].name,
             sevTag(d[i].now), d[i].a, d[i].b);
    wlogRow(line);
    wrote = true;
  }
  if (wrote) wlogFlush();
}

// "What's currently drawn" per line in the stats band -- see
// uiDrawFieldIfChanged()/uiFieldChanged() in ui.h. This band used to be
// erased and fully reprinted every second (the old comment here called
// that out as already bounded, not uiClearBelow -- but still a full
// reprint every tick); isolating each line means an unchanged DEAUTH /
// BEACON / AUTH / ROGUE-AP / six-detector block no longer flashes just
// because the elapsed-time field ticked over. widsEnter() resets all of
// these alongside lastBannerKey (see resetStatsFields() below).
static char prevChanLine[32] = "", prevElapsed[48] = "";
static char prevDeauth1[32] = "", prevDeauth2[40] = "";
static char prevBeacon1[32] = "", prevBeacon2[48] = "";
static char prevAuth1[32] = "",  prevAuth2[48] = "";
static char prevRogueLine[64] = "";
static char prevDetLine[16] = "";

static void resetStatsFields() {
  prevChanLine[0] = prevElapsed[0] = '\0';
  prevDeauth1[0] = prevDeauth2[0] = '\0';
  prevBeacon1[0] = prevBeacon2[0] = '\0';
  prevAuth1[0]  = prevAuth2[0]  = '\0';
  prevRogueLine[0] = '\0';
  prevDetLine[0] = '\0';
}

static void drawStats() {
  tft.setTextSize(1);

  uint32_t el = (millis() - startMs) / 1000;

  // Channel + lock-status readout: plain text, not a button -- see
  // optionsBtnRect()'s comment above for why the lock picker now has its
  // own separate button instead of living here.
  char chanSig[32];
  snprintf(chanSig, sizeof(chanSig), "%d|%d|%d|%d|%d", wifiIdsChannel(), clMode, clManualN, clNodeN, clAnyNodeDown());
  if (uiFieldChanged(prevChanLine, sizeof(prevChanLine), chanSig)) {
    uiClearRect(CHAN_X, HDR_Y, 94, 11);
    tft.setTextColor(accentLabel());
    tft.setCursor(CHAN_X, HDR_Y);
    tft.printf("ch%d", wifiIdsChannel());
    // "+N" when more than one channel is being watched (a manual multi-
    // channel lock, or a mesh SSID lock with more than one node discovered).
    if (clMode == CL_MANUAL && clManualN > 1) {
      tft.printf("+%d", clManualN - 1);
    } else if ((clMode == CL_SEARCHING || clMode == CL_LOCKED || clMode == CL_REACQUIRING || clMode == CL_DOWN)
               && clNodeN > 1) {
      tft.printf("+%d", clNodeN - 1);
    }
    const char *clSfx = "";
    uint16_t    clCol = accentLabel();
    switch (clMode) {
      case CL_AUTO:                                           break;
      case CL_MANUAL:      clSfx = " LOCK";   clCol = accentLabel();  break;
      case CL_SEARCHING:   clSfx = " SEARCH"; clCol = ILI9341_YELLOW; break;
      case CL_LOCKED:      clSfx = " OK";     clCol = ILI9341_GREEN;  break;
      case CL_REACQUIRING: clSfx = " LOST";   clCol = ILI9341_YELLOW; break;
      case CL_DOWN:        clSfx = " DOWN";   clCol = ILI9341_RED;    break;
    }
    tft.setTextColor(clCol);
    tft.print(clSfx);
    if (clMode != CL_DOWN && clAnyNodeDown()) { tft.setTextColor(ILI9341_YELLOW); tft.print(" !"); }
  }

  int elapsedX = CHAN_X + 94;
  uiDrawFieldIfChanged(elapsedX, HDR_Y, tft.width() - elapsedX, 11,
                        ILI9341_WHITE, 1, prevElapsed, sizeof(prevElapsed),
                        "%02lu:%02lu  seen %lu  drop %lu",
                        (unsigned long)(el / 60), (unsigned long)(el % 60),
                        (unsigned long)gSeen, (unsigned long)wifiIdsDropped());

  uiDrawFieldIfChanged(2, D_Y, tft.width() - 4, 11, sevColor(vD.sev), 1, prevDeauth1, sizeof(prevDeauth1),
                        "DEAUTH  %s", sevTag(vD.sev));
  uiDrawFieldIfChanged(2, D_Y + 11, tft.width() - 4, 11, sevColor(vD.sev), 1, prevDeauth2, sizeof(prevDeauth2),
                        " d%u dis%u  rc%u  spoof%u", vD.a, vD.b, vD.c, vD.d);

  uiDrawFieldIfChanged(2, B_Y, tft.width() - 4, 11, sevColor(vB.sev), 1, prevBeacon1, sizeof(prevBeacon1),
                        "BEACON  %s", sevTag(vB.sev));
  uiDrawFieldIfChanged(2, B_Y + 11, tft.width() - 4, 11, sevColor(vB.sev), 1, prevBeacon2, sizeof(prevBeacon2),
                        " uniq~%u rnd%u laa%u seq%u b%u", vB.a, vB.b, vB.c, vB.d, vB.e);

  uiDrawFieldIfChanged(2, A_Y, tft.width() - 4, 11, sevColor(vA.sev), 1, prevAuth1, sizeof(prevAuth1),
                        "AUTH/ASSOC  %s", sevTag(vA.sev));
  uiDrawFieldIfChanged(2, A_Y + 11, tft.width() - 4, 11, sevColor(vA.sev), 1, prevAuth2, sizeof(prevAuth2),
                        " req%u src~%u laa%u ->%02X%02X%02X",
                        vA.a, vA.b, vA.c, vATop[3], vATop[4], vATop[5]);

  if (!rogueOn) {
    uiDrawFieldIfChanged(2, R_Y, tft.width() - 4, 11, ILI9341_YELLOW, 1, prevRogueLine, sizeof(prevRogueLine),
                          "ROGUE-AP  no baseline, run Rogue AP");
  } else {
    uiDrawFieldIfChanged(2, R_Y, tft.width() - 4, 11, vRogue ? ILI9341_RED : ILI9341_GREEN, 1,
                          prevRogueLine, sizeof(prevRogueLine),
                          "ROGUE-AP  %s  (baseline %d)", vRogue ? "HIT" : "watching", rogueApCount());
  }

  // One compact line for the six newer detectors -- color conveys severity
  // (no room here for a text tag per item the way the three blocks above
  // get one); same technique ble_scan.cpp's "Flipper:N  Glasses:N" summary
  // line uses (sequential setTextColor+print, not one printf). Six colors
  // on one line, so like the channel readout above this is a gate-only
  // field.
  char detSig[16];
  snprintf(detSig, sizeof(detSig), "%d%d%d%d%d%d", sevKarma(), sevCsa(), vHsTheft, sevPmkid(), vKrack, sevWps());
  if (uiFieldChanged(prevDetLine, sizeof(prevDetLine), detSig)) {
    uiClearRect(0, N_Y, tft.width(), 11);
    tft.setCursor(2, N_Y);
    tft.setTextColor(sevColor(sevKarma()));                     tft.print("KARMA ");
    tft.setTextColor(sevColor(sevCsa()));                       tft.print("CSA ");
    tft.setTextColor(sevColor(vHsTheft ? SEV_ALERT : SEV_OK));  tft.print("HS ");
    tft.setTextColor(sevColor(sevPmkid()));                     tft.print("PMKID ");
    tft.setTextColor(sevColor(vKrack ? SEV_ALERT : SEV_OK));    tft.print("KRACK ");
    tft.setTextColor(sevColor(sevWps()));                       tft.print("WPS");
  }
}

static uint32_t bannerKey() {
  // Bits 10-15: one each for the six newer detectors, "elevated at all"
  // (WATCH or ALERT) rather than full 2-bit precision -- the original
  // uint16_t was full at that point. This means a WATCH->ALERT escalation
  // within one of these six, with nothing else changing, can go unrepainted
  // for up to one more 5s window (the underlying detection/logging doesn't
  // depend on this key at all, only the on-screen repaint timing does) --
  // an acceptable cosmetic tradeoff against widening this key's type
  // further than the uint32_t below. Bits 16-19: channel-lock state (3 bits
  // for the 6 ClMode values) + "any tracked node down" -- widened to
  // uint32_t to fit these once the original 16 bits filled up.
  return (uint32_t)vD.sev | ((uint32_t)vB.sev << 2) | ((uint32_t)vA.sev << 4) |
                    (vSpoofFlag ? 0x40u : 0) | (vRogue ? 0x80u : 0) |
                    (vPwn ? 0x100u : 0) | (vTwin ? 0x200u : 0) |
                    (sevKarma()   ? 0x400u  : 0) |
                    (sevCsa()     ? 0x800u  : 0) |
                    (vHsTheft     ? 0x1000u : 0) |
                    (sevPmkid()   ? 0x2000u : 0) |
                    (vKrack       ? 0x4000u : 0) |
                    (sevWps()     ? 0x8000u : 0) |
                    ((uint32_t)clMode << 16) |
                    (clAnyNodeDown() ? 0x80000u : 0);
}

static void drawBanner() {
  uint32_t key = bannerKey();
  if (key == lastBannerKey) return;
  lastBannerKey = key;

  uint8_t worst = vD.sev;
  if (vB.sev > worst) worst = vB.sev;
  if (vA.sev > worst) worst = vA.sev;
  if (vRogue && worst < SEV_ALERT) worst = SEV_ALERT;   // a baseline mismatch is an alert
  if (vTwin  && worst < SEV_ALERT) worst = SEV_ALERT;   // scored evil twin, no baseline needed
  if (vPwn   && worst < SEV_WATCH) worst = SEV_WATCH;   // harvester in range -- note, not an attack
  if (sevKarma()   > worst) worst = sevKarma();
  if (sevCsa()     > worst) worst = sevCsa();
  if (vHsTheft && worst < SEV_ALERT) worst = SEV_ALERT;  // the correlation IS the attack
  if (sevPmkid()   > worst) worst = sevPmkid();
  if (vKrack   && worst < SEV_ALERT) worst = SEV_ALERT;  // a replayed message 3 IS the attack
  if (sevWps()     > worst) worst = sevWps();
  // Channel lock: the protected network being fully gone (3 failed sweeps)
  // IS the event, same as HS-THEFT/KRACK. A single tracked node down while
  // the lock is otherwise LOCKED/REACQUIRING is only ever a WATCH -- it
  // never forces ALERT on its own, only CL_DOWN does.
  if (clMode == CL_DOWN && worst < SEV_ALERT) worst = SEV_ALERT;
  if ((clMode == CL_LOCKED || clMode == CL_REACQUIRING) && clAnyNodeDown() && worst < SEV_WATCH)
    worst = SEV_WATCH;

  if (worst == SEV_ALERT) tft.fillRect(4, BANNER_Y, tft.width() - 8, BANNER_H, ILI9341_RED);
  else                    uiClearRect(4, BANNER_Y, tft.width() - 8, BANNER_H);   // theme-aware, not flat black

  if (worst == SEV_ALERT) {
    // 128 B: comfortably over the worst case of every tag firing at once
    // (~108 B with all 12 below) -- appendTag() bounds by the destination's
    // REMAINING space (sizeof-strlen-1), not a fixed source-length cap, so
    // this can't overflow even if more tags are added later.
    char what[128]; what[0] = 0;
    auto appendTag = [&](const char *tag) { strncat(what, tag, sizeof(what) - strlen(what) - 1); };
    if (vSpoofFlag)               appendTag("DEAUTH-SPOOF ");
    else if (vD.sev == SEV_ALERT) appendTag("DEAUTH-FLOOD ");
    if (vB.sev == SEV_ALERT)      appendTag("BEACON-FLOOD ");
    if (vA.sev == SEV_ALERT)      appendTag("AUTH-FLOOD ");
    if (vRogue)                   appendTag("ROGUE-AP ");
    if (vTwin)                    appendTag("EVIL-TWIN? ");
    if (vPwn)                     appendTag("PWN ");
    if (sevKarma() == SEV_ALERT)  appendTag("KARMA ");
    if (sevCsa() == SEV_ALERT)    appendTag("CSA-ABUSE ");
    if (vHsTheft)                 appendTag("HS-THEFT ");
    if (sevPmkid() == SEV_ALERT)  appendTag("PMKID ");
    if (vKrack)                   appendTag("KRACK ");
    if (sevWps() == SEV_ALERT)    appendTag("WPS-BRUTE ");
    if (clMode == CL_DOWN)        appendTag("NETWORK-DOWN ");
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
  // Stop clear above UI_STATUSBAR_H -- clearing all the way to tft.height()
  // wiped the persistent status-bar strip (background + white separator
  // line) without restoring it, since that's drawn once by uiDrawStatusBar()
  // and otherwise only repainted piecemeal (clock/battery glyphs) by
  // uiServiceChrome(), not as a whole strip -- this is why the bar used to
  // vanish on every log update, leaving only the clock/battery floating
  // with no strip under them.
  uiClearRect(0, LOG_Y, tft.width(), tft.height() - UI_STATUSBAR_H - LOG_Y);   // theme-aware, not flat black
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_RED);
  // Clamp to however many lines actually fit above the status bar, rather
  // than assuming all 4 stored lines always do -- on this board's landscape
  // orientation (240px tall) the stats block above has grown enough (the
  // new N_Y status line) that 4 no longer reliably fits; portrait still
  // shows all 4.
  int maxLines = (tft.height() - UI_STATUSBAR_H - LOG_Y) / 12;
  if (maxLines < 0) maxLines = 0;
  uint8_t show = alogN < (uint8_t)maxLines ? alogN : (uint8_t)maxLines;
  for (uint8_t i = 0; i < show; i++) {
    tft.setCursor(2, LOG_Y + i * 12);
    tft.print(alog[i]);
  }
  // Drawn last so it always paints on top of any log text underneath --
  // see optionsBtnRect()'s comment for why this isn't given its own
  // reserved row instead.
  uiDrawButton(optionsBtnRect());
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
  resetStatsFields();
  alerted = false;

  // KARMA / CSA-abuse / handshake-theft / PMKID-harvest / KRACK / WPS --
  // session-scoped tables and one-shot alert flags, all cleared here same
  // as the Ap/Tgt baselines above.
  memset(karma, 0, sizeof karma);
  memset(csaTgts, 0, sizeof csaTgts);
  csaOffRegdomain = 0; csaOffLogged = false;
  memset(dv, 0, sizeof dv);
  vHsTheft = hsTheftLogged = false;
  memset(asTbl, 0, sizeof asTbl);
  memset(pmkidWait, 0, sizeof pmkidWait);
  pmkidSusp = 0; pmkidStaN = 0;
  pmkidPinUntil = 0;   // the hopper itself starts unpinned via wifiIdsBegin(), nothing to resume
  memset(kgTbl, 0, sizeof kgTbl); kgNext = 0;
  memset(m3tbl, 0, sizeof m3tbl);
  vKrack = krackLogged = false;
  memset(wpsTgts, 0, sizeof wpsTgts);
  lastKarmaSev = lastPmkidSev = lastWpsSev = SEV_OK;

  // Channel lock / SSID-protect: session-scoped, not NVS-persisted -- every
  // screen entry starts back at free auto-hop.
  clMode = CL_AUTO;
  clSsid[0] = 0; clSsidHash = 0;
  memset(clNodes, 0, sizeof clNodes);
  clNodeN = clManualN = clAttempts = 0;
  clSweepStart = clDownRecheckAt = 0;
  clDownRecheck = false;

  resetWindow();
  dropAtWindow = 0;

  wifiIdsWantEapol(true);   // widen the filter before Begin() so the EAPOL/WPS ring gets allocated
  wifiIdsBegin();
  wifiIdsSetDwell(300);    // keep the screen's historical 300 ms hop cadence
  hD = wifiIdsRegister(&onDeauthFamily, nullptr, WIDS_MASK_DEAUTH_FAMILY);
  hB = wifiIdsRegister(&onBeacon,       nullptr, WIDS_BIT(WIDS_BEACON));
  hA = wifiIdsRegister(&onAuthAssoc,    nullptr,
                       WIDS_BIT(WIDS_AUTH) | WIDS_BIT(WIDS_ASSOC_REQ) | WIDS_BIT(WIDS_REASSOC_REQ));
  hK    = wifiIdsRegister(&onKarmaProbeResp, nullptr, WIDS_BIT(WIDS_PROBE_RESP));
  hCsa  = wifiIdsRegister(&onCsaAction,      nullptr, WIDS_BIT(0xD));   // action frames (unnamed subtype, see drone_detect.cpp precedent)
  hEapol = wifiIdsRegisterEapol(&onEapol, nullptr);
  hWps   = wifiIdsRegisterWps(&onWps, nullptr);

  startMs = windowStart = millis();
  lastStatsDraw = 0;
  lastWlogSummary = millis();
  running = true;

  wlogOpen("wifiids", "utc,channel,detector,severity,detail");   // event rows only

  drawStats();
  drawBanner();
  // drawLog() is normally only entered when alogDirty is set by a new
  // event -- force one pass now so the Options button (drawn at the end
  // of drawLog(), see its comment) actually appears on screen entry
  // instead of staying invisible until the first log event or dismiss tap.
  alogDirty = true;
  drawLog();
}

void widsLoop() {
  if (!running) return;
  wifiIdsLoop();            // pump hopper + drain captures into the 3 callbacks

  uint32_t now = millis();
  if (pmkidPinUntil && now > pmkidPinUntil && clMode == CL_AUTO) { wifiIdsHopResume(); pmkidPinUntil = 0; }
  serviceClMode();
  if (now - lastStatsDraw > 1000) { drawStats(); lastStatsDraw = now; }

  if (now - windowStart >= WINDOW_MS) {
    uint8_t pd = vD.sev, pb = vB.sev, pa = vA.sev;
    computeVerdicts();
    logEdges(pd, pb, pa, lastKarmaSev, lastPmkidSev, lastWpsSev);
    wlogVerdictEdges(pd, pb, pa, lastKarmaSev, lastPmkidSev, lastWpsSev);   // SD: any transition
    drawBanner();
    drawLog();
    drawStats();           // repaint immediately with the fresh window's numbers
    lastStatsDraw = now;
    lastKarmaSev = sevKarma();
    lastPmkidSev = sevPmkid();
    lastWpsSev   = sevWps();
    resetWindow();
    windowStart = now;
  }

  // SD: one summary row per minute regardless of transitions.
  if (wlogIsOpen() && now - lastWlogSummary >= 60000) {
    lastWlogSummary = now;
    uint8_t worst = vD.sev;
    if (vB.sev > worst) worst = vB.sev;
    if (vA.sev > worst) worst = vA.sev;
    char line[112];
    snprintf(line, sizeof(line), "%s,%d,summary,%s,d%u/b%u/a%u",
             devTimeNowString().c_str(), wifiIdsChannel(), sevTag(worst),
             (unsigned)(vD.a + vD.b), vB.a, vA.a);
    wlogRow(line);
    wlogFlush();
  }
}

void widsTouch(const TouchPoint &t) {
  if (!t.isNewPress) return;
  // "Options" button (drawn bottom-right, see optionsBtnRect()): opens
  // the channel-lock picker. Checked first since it sits inside the
  // banner/log tap area below and would otherwise be swallowed by the
  // "dismiss alert" fallthrough right after this. A few px of slop on
  // every side for a fat-finger tap on the resistive panel, same idea as
  // this screen's old channel-lock button hit test.
  Btn ob = optionsBtnRect();
  bool hitOpt = (t.x >= ob.x - 4 && t.x < ob.x + ob.w + 4 && t.y >= ob.y - 6 && t.y < ob.y + ob.h + 6);
  DLOG("wids", "tap x=%d y=%d optBtn x=%d y=%d w=%d h=%d hit=%d", t.x, t.y, ob.x, ob.y, ob.w, ob.h, (int)hitOpt);
  if (hitOpt) {
    uiWaitForRelease();
    clOpenPicker();
    return;
  }
  if (t.y < BANNER_Y) return;                 // taps land on the banner / log area
  alogN = 0;
  memset(alog, 0, sizeof alog);
  alogDirty = true;
  alerted = false;
  vRogue = false;                             // re-arm rogue-AP alerting
  vPwn = vTwin = pwnLogged = false;
  rogueSeenN = 0;
  // KARMA/CSA are live-computed from their tables each call (not sticky
  // booleans like vRogue/vTwin/vPwn above), so dismissing has to actually
  // clear the tables or the alert would just reappear next redraw.
  memset(karma, 0, sizeof karma);
  memset(csaTgts, 0, sizeof csaTgts);
  csaOffRegdomain = 0; csaOffLogged = false;
  vHsTheft = hsTheftLogged = false;
  vKrack = krackLogged = false;
  ledSet(false);
  lastBannerKey = 0xFFFF;                     // force a banner repaint
  drawBanner();
  drawLog();
}

void widsExit() {
  if (!running) return;   // widsEnter bailed to the Rogue AP screen -- nothing came up
  running = false;
  if (pmkidPinUntil) { wifiIdsHopResume(); pmkidPinUntil = 0; }   // leave the shared core unpinned
  if (clMode != CL_AUTO) { wifiIdsHopResume(); clMode = CL_AUTO; }   // same hygiene for a channel/SSID lock
  wlogClose();
  wifiIdsUnregister(hD); wifiIdsUnregister(hB); wifiIdsUnregister(hA);
  wifiIdsUnregister(hK); wifiIdsUnregister(hCsa);
  wifiIdsUnregisterEapol(hEapol);
  wifiIdsUnregisterWps(hWps);
  hD = hB = hA = hK = hCsa = hEapol = hWps = -1;
  wifiIdsWantEapol(false);
  wifiIdsEnd();            // last ref: drops promiscuous, restores WIFI_MODE_NULL
  beepHold(false);
  ledSet(false);
}
