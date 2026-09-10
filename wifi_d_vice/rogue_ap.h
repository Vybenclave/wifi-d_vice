#pragma once
#include <Arduino.h>

// Rogue-AP / evil-twin baseline + check. A "known good" table of the APs you
// expect (ESSID / BSSID / security / channel / typical RSSI) is learned once
// and kept on the SD card as /rogueap.csv (plaintext -- it is not secret and
// has to be readable with no engagement key). Two consumers:
//
//   * the standalone "Rogue AP" screen (WiFi menu) -- learn / clear the
//     baseline and watch a live WiFi.scanNetworks() pass against it.
//   * the WiFi IDS screen -- registers a beacon check against the same
//     baseline so an evil twin raises an alert in the unified IDS view.
//
// PASSIVE: this only reads scan results / received beacons and compares them
// to a stored table. Nothing transmits.

static const int ROGUE_MAX = 20;   // baseline entries kept in RAM / on card

enum RogueKind {
  ROGUE_NONE = 0,
  ROGUE_EVIL_TWIN,   // a KNOWN ESSID advertised by a BSSID not in the baseline
  ROGUE_DOWNGRADE,   // known ESSID+BSSID, but weaker security than the baseline
  ROGUE_CHAN_MOVE,   // known ESSID+BSSID, but a different channel
  ROGUE_RSSI_JUMP,   // known ESSID+BSSID, RSSI far from the learned level
};

struct RogueHit {
  RogueKind kind;
  char      essid[33];
  uint8_t   bssid[6];
  uint8_t   channel;
  int8_t    rssi;
  uint8_t   baseAuth;   // what the baseline expected (for a DOWNGRADE message)
};

// ---- baseline store ----
void rogueApLoad();        // read /rogueap.csv into RAM (call on screen enter)
int  rogueApCount();
bool rogueApLoaded();      // a baseline file was found and parsed
void rogueApClear();       // wipe RAM + delete the file

// ---- learn ----
// rogueApLearnReset(), then rogueApLearnObserve() for every AP of several
// scan passes (deduped on BSSID here), then rogueApLearnCommit() to replace
// the baseline and write the file. Returns the entry count written.
void rogueApLearnReset();
void rogueApLearnObserve(const char *essid, const uint8_t *bssid,
                         uint8_t auth, uint8_t channel, int rssi);
int  rogueApLearnCommit();
int  rogueApLearnPending();   // distinct BSSIDs accumulated so far

// ---- check one observed AP against the baseline ----
// `auth` is a wifi_auth_mode_t value (0 = OPEN). Returns ROGUE_NONE when the
// ESSID isn't one we have a baseline for, or it matches cleanly. Fills `out`
// on a non-NONE result.
RogueKind rogueApCheck(const char *essid, const uint8_t *bssid,
                       uint8_t auth, uint8_t channel, int rssi, RogueHit *out);

const char *rogueKindTag(RogueKind k);   // short label for the UI / logs
