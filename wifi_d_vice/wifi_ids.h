#pragma once
#include <stdint.h>
#include <stddef.h>

// Shared passive 802.11 promiscuous-capture core. One place owns the single
// esp_wifi_set_promiscuous_rx_cb + the channel-hop schedule; feature screens
// (deauth detector today; evil-twin / beacon-flood / auth-flood and the
// Guardian umbrella later, see DESIGN.md section 3) register a callback and
// get parsed management frames pushed to them. Rationale for one core rather
// than a callback per module: the ESP32 promiscuous RX callback slot is a
// single global, and every extra module re-registering it would clobber the
// last -- they cannot coexist. This also lets one hop schedule be shared
// (DESIGN.md section 1: "shares one channel-hop schedule across the monitors
// that need it").
//
// PASSIVE ONLY. This core calls esp_wifi_set_promiscuous / _channel and
// nothing else on the radio -- no esp_wifi_80211_tx, no injection, no
// association. Detectors receive and count; they never answer.
//
// Coexistence with the rest of the firmware's WiFi use:
//  * wifiIdsBegin() puts the radio in WIFI_MODE_NULL + promiscuous. That is
//    incompatible with a concurrent WiFi.mode(WIFI_STA) / WiFi.scanNetworks()
//    / WiFi.begin() -- the STA path and promiscuous fight over the same PHY
//    and channel. The firmware's single-screen model is what keeps them
//    apart: wifi_scan.cpp, flock_detect.cpp and netstats.cpp own the radio
//    while their screen is up; a wifi_ids consumer owns it while its screen
//    is up; only one screen is ever active. wifiIdsEnd() restores
//    WIFI_MODE_NULL so the next screen's WiFi.mode(WIFI_STA) starts clean.
//  * Do NOT call wifiIdsBegin() from a screen that also runs WiFi.scanNetworks.
//  * BLE: the WROOM-32 cannot hold the WiFi driver and the BLE stack in RAM
//    at once (see the wroom32-wifi-ble-ram note -- flock_detect/meshtastic
//    call WiFi.mode(WIFI_OFF) before BLEDevice::init for exactly this). When
//    Guardian mode picks its concurrent monitor set it must not enable a BLE
//    monitor alongside wifi_ids. This core does not and cannot fix that; it
//    only needs to not be running when BLE comes up.

// 802.11 management-frame subtypes we parse and dispatch. The value is the
// raw subtype nibble (frame control byte 0, bits 4..7) so a detector can
// compare f.subtype straight against a byte it pulled out of f.raw.
enum WifiIdsSubtype : uint8_t {
  WIDS_ASSOC_REQ    = 0x0,
  WIDS_ASSOC_RESP   = 0x1,
  WIDS_REASSOC_REQ  = 0x2,
  WIDS_REASSOC_RESP = 0x3,
  WIDS_PROBE_REQ    = 0x4,
  WIDS_PROBE_RESP   = 0x5,
  WIDS_BEACON       = 0x8,
  WIDS_DISASSOC     = 0xA,
  WIDS_AUTH         = 0xB,
  WIDS_DEAUTH       = 0xC,
};

// Subscription mask: OR of (1 << subtype) for the subtypes a detector wants.
#define WIDS_BIT(st)             (1u << (st))
#define WIDS_MASK_DEAUTH_FAMILY  (WIDS_BIT(WIDS_DEAUTH) | WIDS_BIT(WIDS_DISASSOC))
#define WIDS_MASK_ALL_MGMT       0xFFFFu

// One parsed management frame handed to a detector callback.
//
// `raw` points at a bounded SNAPSHOT copy owned by wifi_ids, valid ONLY for
// the duration of the callback -- copy out anything you need to keep. It is
// truncated to WIFI_IDS_SNAP_LEN bytes; `rawLen` is how much is actually
// there to walk, `onAirLen` is the real on-air length. For beacons /
// probe-responses the fixed params start at raw+24 and the IE list at raw+36
// (raw+24 for the other subtypes). The snapshot is long enough for the
// front-loaded IEs Tier 1 needs (SSID, DS-param, RSN); a detector that needs
// a tail IE past the snapshot has to check onAirLen and accept it may be
// cut off.
struct WifiIdsFrame {
  uint8_t  subtype;    // WifiIdsSubtype
  uint8_t  channel;    // channel it was captured on (from the driver, not the hopper's guess)
  int8_t   rssi;       // driver RSSI, dBm
  uint16_t seq;        // 802.11 sequence number (seq_ctrl >> 4)
  uint8_t  addr1[6];   // receiver / destination
  uint8_t  addr2[6];   // transmitter / source
  uint8_t  addr3[6];   // BSSID (for the subtypes we carry)
  const uint8_t *raw;  // snapshot, callback-lifetime only, <= WIFI_IDS_SNAP_LEN
  uint16_t rawLen;     // bytes available at `raw`
  uint16_t onAirLen;   // full frame length as received
};

// Snapshot length. Deauth/disassoc/auth/assoc frames are well under this;
// beacons/probe-resps are clipped but keep their leading IEs.
static const int WIFI_IDS_SNAP_LEN = 200;

typedef void (*WifiIdsDetectorFn)(const WifiIdsFrame &f, void *ctx);

// Register / unregister a detector. `subtypeMask` selects which subtypes
// fire `fn` (WIDS_BIT / WIDS_MASK_*). `ctx` is handed back untouched. The
// callback runs from wifiIdsLoop() (task context, NOT the RX callback), so
// it may touch tft / beep / WiFi state, but it is called once per captured
// frame and should still return quickly. Returns a handle >= 0, or -1 if the
// table is full. Safe to call before wifiIdsBegin().
int  wifiIdsRegister(WifiIdsDetectorFn fn, void *ctx, uint16_t subtypeMask);
void wifiIdsUnregister(int handle);

// Ref-counted lifecycle. The first wifiIdsBegin() powers the radio to
// promiscuous and starts the hopper; the matching last wifiIdsEnd() tears it
// down and restores WIFI_MODE_NULL. A screen and (later) Guardian can each
// hold a reference without stepping on each other.
void wifiIdsBegin();
void wifiIdsEnd();
bool wifiIdsActive();

// Pump the hop scheduler and drain captured frames into detector callbacks.
// Call every main-loop iteration while any consumer is active (a consumer's
// own Loop() is the natural place -- deauthLoop() does this).
void wifiIdsLoop();

// Channel-hop control. Hops WIFI_IDS_CHAN_MIN..MAX, `dwellMs` per channel
// (default 250). Pin locks the hopper on one channel until wifiIdsHopResume()
// -- a detector or Guardian camps on a channel to watch one BSSID's beacons.
// Nesting is not tracked: last caller wins (documented, fine for now -- the
// only two callers will be a detector and Guardian, arbitrated by Guardian's
// own menu).
static const uint8_t WIFI_IDS_CHAN_MIN = 1;
static const uint8_t WIFI_IDS_CHAN_MAX = 13;   // 12/13 are set-channel no-ops on an 11-channel regdomain; harmless
void    wifiIdsSetDwell(uint32_t dwellMs);
void    wifiIdsHopPin(uint8_t channel);

// Cycle ONLY these channels (1-3), dwelling s_dwellMs on each before moving
// to the next -- for locking onto a specific SSID that spans more than one
// channel (mesh/dual-band). count is clamped to 3. Clears any pin.
// wifiIdsHopResume() reverts to the full default 1..13 behavior.
void    wifiIdsHopSet(const uint8_t *channels, uint8_t count);
void    wifiIdsHopResume();
uint8_t wifiIdsChannel();     // channel the capture is currently parked on

// Frames dropped because the capture ring was full (a flood outruns the
// drain). Cumulative since the last wifiIdsBegin() from zero. Detectors may
// read this as extra "we are being flooded" signal.
uint32_t wifiIdsDropped();

// ---- EAPOL / WPS: the one data-frame exception -----------------------
//
// Everything above is management frames only. This section widens the
// capture to a narrow, specific slice of DATA frames: 802.1X (EAPOL)
// traffic -- the WPA 4-way handshake (EAPOL-Key, 802.1X packet type 3) and
// WPS/WiFi-Simple-Config negotiation (EAP-Packet, type 0, carrying the WFA
// vendor-specific Expanded EAP type). Both are themselves UNENCRYPTED (they
// ARE the key exchange / provisioning handshake), so this is still fully
// passive -- no PSK needed, nothing transmitted.
//
// This is NOT general data-frame access: widsRxCb() admits a data frame
// only as far as it takes to confirm LLC/SNAP + ethertype 0x888E (EAPOL),
// then classifies it as EAPOL-Key or WPS/EAP-Packet or discards it. A
// detector never sees raw data-frame bytes, only the two small structs
// below. See wifi_ids.cpp for the exact admission-filter arithmetic (it is
// deliberately front-loaded with cheap integer checks, since on a busy
// channel data frames vastly outnumber management frames).
//
// wifiIdsWantEapol(true) widens esp_wifi_set_promiscuous_filter() to admit
// WIFI_PROMIS_FILTER_MASK_DATA alongside the existing MGMT mask. It is
// opt-in and ref-counted independently of wifiIdsRegister()/Unregister() --
// a screen that never calls it pays zero extra per-packet cost; the filter
// itself keeps non-EAPOL data frames from ever reaching widsRxCb.

// EAPOL-Key message classification (Key Information field bits, IEEE
// 802.11-2020 Figure 12-35 / the conventional WPA 4-way-handshake numbering):
//   M1: ACK=1 MIC=0                  (AP -> STA, ANonce)
//   M2: ACK=0 MIC=1 Install=0 Secure=0  (STA -> AP, SNonce)
//   M3: ACK=1 MIC=1 Install=1        (AP -> STA)
//   M4: ACK=0 MIC=1 Secure=1         (STA -> AP)
enum WifiIdsEapolMsg : uint8_t {
  WIDS_EAPOL_OTHER = 0,
  WIDS_EAPOL_M1    = 1,
  WIDS_EAPOL_M2    = 2,
  WIDS_EAPOL_M3    = 3,
  WIDS_EAPOL_M4    = 4,
};

// One classified EAPOL-Key sighting. Deliberately NOT a WifiIdsFrame: the
// data-frame header's address-field ROLES (not just values) depend on
// ToDS/FromDS, and the body-start offset varies with QoS/Order bits -- a
// management-frame-shaped struct (raw+24/raw+36 IE-walk convention) would
// be actively misleading here. bssid/sta are already resolved from
// ToDS/FromDS by the capture core; a detector never does that arithmetic.
struct WifiIdsEapol {
  uint8_t  bssid[6];          // the AP side of the pair
  uint8_t  sta[6];            // the non-AP side of the pair
  uint8_t  channel;
  int8_t   rssi;
  uint16_t seq;
  WifiIdsEapolMsg msg;
  uint16_t replayCounterHi;   // top 16 bits of the 64-bit EAPOL replay counter --
                               // enough to notice "same handshake continuing" /
                               // a KRACK-style message-3 replay, without carrying
                               // all 8 bytes through the ring
};
typedef void (*WifiIdsEapolFn)(const WifiIdsEapol &e, void *ctx);
int  wifiIdsRegisterEapol(WifiIdsEapolFn fn, void *ctx);
void wifiIdsUnregisterEapol(int handle);

// One WPS/WiFi-Simple-Config EAP-Packet sighting -- identified via the
// Expanded EAP type (254) carrying the WFA vendor id (00:37:2A) and
// vendor-type 1 (SimpleConfig). Only enough to RATE-track registration
// attempts (Reaver/Bully-style PIN brute force); the WSC TLV payload itself
// is not parsed.
struct WifiIdsWps {
  uint8_t bssid[6];
  uint8_t sta[6];
  uint8_t channel;
  int8_t  rssi;
  uint8_t eapCode;   // RFC 3748: 1 = Request, 2 = Response
};
typedef void (*WifiIdsWpsFn)(const WifiIdsWps &w, void *ctx);
int  wifiIdsRegisterWps(WifiIdsWpsFn fn, void *ctx);
void wifiIdsUnregisterWps(int handle);

// Ref-counted independently of wifiIdsBegin()/End(): call before Begin() (or
// any time after) to widen the filter for the EAPOL/WPS paths above; call
// with false (or just stop registering EAPOL/WPS detectors) when no longer
// needed. Safe to call whether or not the core is currently running -- if it
// is, the filter is re-applied immediately.
void wifiIdsWantEapol(bool on);
