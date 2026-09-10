// Shared passive 802.11 promiscuous-capture core -- see wifi_ids.h for the
// API, the "why one core" rationale, and the coexistence / BLE notes.
//
// Shape of this file:
//   RX callback (widsRxCb)  -- runs in the WiFi driver's task context, NOT a
//                              hard ISR, but kept ISR-cheap anyway: parse the
//                              fixed header, bounded memcpy of a snapshot into
//                              a lock-free ring, return. No malloc, no
//                              detector calls, no draw.
//   Drain (wifiIdsLoop)     -- task context: advance the hopper, pop the ring,
//                              call each subscribed detector once per frame.
//
// Ring buffer vs. direct dispatch from the callback: ring buffer. Direct
// dispatch would force every detector callback to be RX-context-safe (no
// tft, careful about blocking) and would run detector work -- IE walking,
// screen counters -- inside the capture path during exactly the floods we
// care about. The ring decouples them; the cost is that a flood which
// outruns wifiIdsLoop() drops frames once the ring fills. That is fine for
// flood/anomaly detection (you still see the flood; the drop count is itself
// signal, exposed via wifiIdsDropped()) and is the documented tradeoff.

#include <WiFi.h>
#include "esp_wifi.h"
#include "wifi_ids.h"

// ---- capture ring ---------------------------------------------------------

// 24 * (~30 + 200) ~= 5.5 KB static. Deep enough to ride out a short burst
// between two wifiIdsLoop() calls at the firmware's loop rate.
static const int WIDS_RING_LEN = 24;

struct RingEntry {
  uint8_t  subtype;
  uint8_t  channel;
  int8_t   rssi;
  uint16_t seq;
  uint16_t onAirLen;
  uint16_t snapLen;
  uint8_t  a1[6], a2[6], a3[6];
  uint8_t  buf[WIFI_IDS_SNAP_LEN];
};

// Heap, not static: ~5.5 KB. Allocated in wifiIdsBegin(), freed in
// wifiIdsEnd(), so screens that never open the IDS (and the WiFi+BLE-hungry
// Flock scan) keep that DRAM.
static RingEntry        *s_ring = nullptr;
static volatile uint16_t s_head = 0;      // producer: widsRxCb
static volatile uint16_t s_tail = 0;      // consumer: wifiIdsLoop
static volatile uint32_t s_dropped = 0;

// ---- detector table -----------------------------------------------------

// Tier 1 (DESIGN.md 3) is ~4 detectors; +1 for the Guardian aggregate, +1
// headroom.
static const int WIDS_MAX_DETECTORS = 6;

struct Detector {
  WifiIdsDetectorFn fn;
  void             *ctx;
  uint16_t          mask;
};
static Detector          s_det[WIDS_MAX_DETECTORS];
// Union of every registered detector's mask. The RX callback checks this and
// drops (without copying) any subtype nobody asked for -- keeps the capture
// path off frames no one will look at, e.g. beacons while only the deauth
// detector is up.
static volatile uint16_t s_wantMask = 0;

// ---- hopper + lifecycle state -----------------------------------------

static int      s_refs    = 0;
static uint32_t s_dwellMs = 250;
static uint32_t s_lastHop = 0;
static uint8_t  s_chan    = WIFI_IDS_CHAN_MIN;
static bool     s_pinned  = false;

static void widsRecalcWantMask() {
  uint16_t m = 0;
  for (int i = 0; i < WIDS_MAX_DETECTORS; i++)
    if (s_det[i].fn) m |= s_det[i].mask;
  s_wantMask = m;
}

// ---- RX callback ------------------------------------------------------

// IRAM_ATTR to match deauth_detect's original and to be safe if a future
// core revision runs this closer to interrupt context. It calls only
// memcpy/memset on stack + static data and returns; all real work is
// deferred to the ring drain.
static void IRAM_ATTR widsRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (!s_ring || type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *p = pkt->payload;

  // Frame control byte 0: bits 2..3 = type (0 == management), 4..7 = subtype.
  if (((p[0] >> 2) & 0x3) != 0) return;
  uint8_t st = (p[0] >> 4) & 0xF;
  if (!((s_wantMask >> st) & 1u)) return;      // nobody subscribed to this subtype

  uint16_t next = (uint16_t)((s_head + 1) % WIDS_RING_LEN);
  if (next == s_tail) { s_dropped++; return; } // ring full: drop, keep the path bounded

  RingEntry &e = s_ring[s_head];
  e.subtype = st;
  e.channel = pkt->rx_ctrl.channel;
  e.rssi    = pkt->rx_ctrl.rssi;

  uint16_t L = pkt->rx_ctrl.sig_len;
  e.onAirLen = L;
  uint16_t n = (L > WIFI_IDS_SNAP_LEN) ? WIFI_IDS_SNAP_LEN : L;
  e.snapLen = n;

  // seq_ctrl and addr1..3 are in the fixed 24-byte mgmt header on every
  // subtype we carry; guard anyway against a runt frame.
  if (L >= 24) {
    e.seq = (uint16_t)((p[22] | (p[23] << 8)) >> 4);
    memcpy(e.a1, p + 4,  6);
    memcpy(e.a2, p + 10, 6);
    memcpy(e.a3, p + 16, 6);
  } else {
    e.seq = 0;
    memset(e.a1, 0, 6); memset(e.a2, 0, 6); memset(e.a3, 0, 6);
  }
  if (n) memcpy(e.buf, p, n);

  s_head = next;
}

// ---- hopper -----------------------------------------------------------

static void widsServiceHopper() {
  if (s_pinned) return;
  uint32_t now = millis();
  if (now - s_lastHop < s_dwellMs) return;
  s_lastHop = now;
  s_chan = (s_chan >= WIFI_IDS_CHAN_MAX) ? WIFI_IDS_CHAN_MIN : (uint8_t)(s_chan + 1);
  esp_wifi_set_channel(s_chan, WIFI_SECOND_CHAN_NONE);   // failure (12/13 off-regdomain) just leaves us where we were
}

// ---- public API -----------------------------------------------------

int wifiIdsRegister(WifiIdsDetectorFn fn, void *ctx, uint16_t subtypeMask) {
  if (!fn) return -1;
  for (int i = 0; i < WIDS_MAX_DETECTORS; i++) {
    if (!s_det[i].fn) {
      s_det[i].fn = fn; s_det[i].ctx = ctx; s_det[i].mask = subtypeMask;
      widsRecalcWantMask();
      return i;
    }
  }
  return -1;
}

void wifiIdsUnregister(int handle) {
  if (handle < 0 || handle >= WIDS_MAX_DETECTORS) return;
  s_det[handle].fn = nullptr; s_det[handle].ctx = nullptr; s_det[handle].mask = 0;
  widsRecalcWantMask();
}

void wifiIdsBegin() {
  if (s_refs++ > 0) return;                 // already running for another consumer

  s_ring = (RingEntry *)calloc(WIDS_RING_LEN, sizeof(RingEntry));
  if (!s_ring) {                            // OOM: give up cleanly, detectors just get nothing
    Serial.printf("[wifi_ids] ring alloc failed, free heap %u\n", (unsigned)ESP.getFreeHeap());
    s_refs = 0;
    return;
  }
  Serial.printf("[wifi_ids] begin, free heap %u\n", (unsigned)ESP.getFreeHeap());

  // Bring the driver up in STA (started, not associated) and switch to
  // promiscuous. NOT WIFI_MODE_NULL: on the current Arduino-ESP32 core that
  // de-inits WiFi, and esp_wifi_set_promiscuous() then fails silently with
  // WIFI_NOT_INIT -- the reason the old deauth detector never counted a
  // frame on hardware. STA + disconnect matches wifi_scan/flock/gps_wardrive.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  esp_wifi_set_promiscuous(true);

  // Ask the driver to only hand us management frames -- Tier 1 detectors are
  // all management-frame checks. Widening to data frames later (EAPOL for the
  // handshake-theft correlation in DESIGN.md's "later tiers") means changing
  // this mask and adding a data-subtype path to widsRxCb.
  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);

  esp_wifi_set_promiscuous_rx_cb(&widsRxCb);

  s_head = s_tail = 0;
  s_dropped = 0;
  s_pinned  = false;
  s_chan    = WIFI_IDS_CHAN_MIN;
  s_lastHop = millis();
  esp_wifi_set_channel(s_chan, WIFI_SECOND_CHAN_NONE);
}

void wifiIdsEnd() {
  if (s_refs == 0) return;
  if (--s_refs > 0) return;                 // another consumer still wants it

  esp_wifi_set_promiscuous_rx_cb(nullptr);
  esp_wifi_set_promiscuous(false);
  // Drop to STA, NOT WIFI_MODE_NULL. NULL de-inits the driver on this core,
  // so the next WiFi screen cold-re-inits and its first ~5 scans / a
  // WiFi.begin() come up empty while the RF recalibrates. STA leaves the
  // driver warm; BLE screens still WiFi.mode(WIFI_OFF) before their init.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);

  free(s_ring);
  s_ring = nullptr;
  Serial.printf("[wifi_ids] end, free heap %u\n", (unsigned)ESP.getFreeHeap());
}

bool wifiIdsActive() { return s_refs > 0; }

void wifiIdsLoop() {
  if (s_refs == 0 || !s_ring) return;

  widsServiceHopper();

  // Bounded: never walk more than one full ring per call, so a sustained
  // flood can't turn this into an unbounded loop that starves the UI.
  int budget = WIDS_RING_LEN;
  while (s_tail != s_head && budget-- > 0) {
    const RingEntry &e = s_ring[s_tail];

    WifiIdsFrame f;
    f.subtype  = e.subtype;
    f.channel  = e.channel;
    f.rssi     = e.rssi;
    f.seq      = e.seq;
    memcpy(f.addr1, e.a1, 6);
    memcpy(f.addr2, e.a2, 6);
    memcpy(f.addr3, e.a3, 6);
    f.raw      = e.buf;
    f.rawLen   = e.snapLen;
    f.onAirLen = e.onAirLen;

    uint16_t bit = (uint16_t)(1u << e.subtype);
    for (int i = 0; i < WIDS_MAX_DETECTORS; i++)
      if (s_det[i].fn && (s_det[i].mask & bit))
        s_det[i].fn(f, s_det[i].ctx);

    s_tail = (uint16_t)((s_tail + 1) % WIDS_RING_LEN);
  }
}

void wifiIdsSetDwell(uint32_t dwellMs) {
  if (dwellMs < 20) dwellMs = 20;           // sane floor -- below this the PHY barely settles per channel
  s_dwellMs = dwellMs;
}

void wifiIdsHopPin(uint8_t channel) {
  if (channel < WIFI_IDS_CHAN_MIN || channel > WIFI_IDS_CHAN_MAX) return;
  s_pinned = true;
  s_chan   = channel;
  if (s_refs > 0) esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

void wifiIdsHopResume() {
  s_pinned  = false;
  s_lastHop = 0;                            // hop on the next wifiIdsLoop()
}

uint8_t  wifiIdsChannel() { return s_chan; }
uint32_t wifiIdsDropped() { return s_dropped; }
