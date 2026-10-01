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

// ---- EAPOL / WPS: the one data-frame exception -------------------------
//
// A second, much smaller ring -- an EAPOL/WPS sighting needs ~21 bytes, not
// a 200-byte mgmt snapshot. One `kind` tag shares it between the two
// detector families (both are rare data-frame subsets admitted by the same
// filter widening and the same admission-filter chain in widsRxCb).
enum { WIDS_DF_EAPOL = 0, WIDS_DF_WPS = 1 };
struct EapolRingEntry {
  uint8_t  kind;
  uint8_t  bssid[6], sta[6];
  uint8_t  channel;
  int8_t   rssi;
  uint16_t seq;          // EAPOL only
  uint8_t  msg;          // EAPOL only: WifiIdsEapolMsg
  uint16_t replayHi;     // EAPOL only
  uint8_t  eapCode;      // WPS only
};
static const int WIDS_EAPOL_RING_LEN = 8;   // covers two overlapping handshakes
static EapolRingEntry *s_eapolRing = nullptr;
static volatile uint16_t s_eapolHead = 0, s_eapolTail = 0;
static bool     s_wantEapol = false;

static const int WIDS_MAX_EAPOL_DETECTORS = 2;
struct EapolDetector { WifiIdsEapolFn fn; void *ctx; };
static EapolDetector s_eapolDet[WIDS_MAX_EAPOL_DETECTORS];

static const int WIDS_MAX_WPS_DETECTORS = 2;
struct WpsDetector { WifiIdsWpsFn fn; void *ctx; };
static WpsDetector s_wpsDet[WIDS_MAX_WPS_DETECTORS];

// ---- hopper + lifecycle state -----------------------------------------

static int      s_refs    = 0;
static uint32_t s_dwellMs = 250;
static uint32_t s_lastHop = 0;
static uint8_t  s_chan    = WIFI_IDS_CHAN_MIN;
static bool     s_pinned  = false;

// Restricted hop set (wifiIdsHopSet) -- cycles only s_hopSet[0..s_hopSetN)
// instead of the full 1..13 sweep. s_hopSetN == 0 means "not active", the
// default/unchanged full-sweep path.
static const int WIDS_HOP_SET_MAX = 3;
static uint8_t  s_hopSet[WIDS_HOP_SET_MAX];
static uint8_t  s_hopSetN = 0;
static uint8_t  s_hopIdx  = 0;

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
// Pushes one EAPOL/WPS sighting; silently drops if that ring is full (same
// bounded-drop discipline as the mgmt ring, just no dedicated counter -- an
// EAPOL/WPS exchange is a handful of frames, not a flood target itself).
static inline void widsPushEapol(const EapolRingEntry &e) {
  if (!s_eapolRing) return;
  uint16_t next = (uint16_t)((s_eapolHead + 1) % WIDS_EAPOL_RING_LEN);
  if (next == s_eapolTail) return;
  s_eapolRing[s_eapolHead] = e;
  s_eapolHead = next;
}

// Admits a DATA frame only as far as confirming it carries EAPOL (ethertype
// 0x888E after the 802.11 header + 8-byte LLC/SNAP), then classifies it as
// EAPOL-Key (handshake) or EAP-Packet/WSC (WPS) or discards it. Checks are
// ordered cheapest-first so the non-EAPOL data-frame majority (encrypted IP
// traffic) is rejected in a handful of integer ops, before the one
// memcmp-class check (the LLC/SNAP compare).
static void IRAM_ATTR widsHandleDataFrame(const wifi_promiscuous_pkt_t *pkt) {
  const uint8_t *p = pkt->payload;
  uint16_t L = pkt->rx_ctrl.sig_len;

  // Frame control byte 0: bits 2..3 = type (2 == data). Covers QoS-Data,
  // plain Data, Null, QoS-Null etc. -- the header-length calc below handles
  // the QoS-Control-field difference; frames with no LLC payload (Null/
  // QoS-Null/CF-ACK) just fail the length checks a few lines down.
  if (((p[0] >> 2) & 0x3) != 2) return;

  uint8_t fc1 = p[1];
  bool toDs   = fc1 & 0x01, fromDs = fc1 & 0x02;
  if (toDs == fromDs) return;   // need exactly one of ToDS/FromDS: reject ad-hoc/IBSS (00,
                                 // no AP involved) and WDS (11, inter-AP -- a different address-
                                 // field shape entirely, not a client<->AP link we can resolve
                                 // BSSID/STA from the same way)

  bool qos    = (p[0] & 0xF0) == 0x80;    // subtype nibble 0x8 (QoS-Data) with bit3 set
  bool order  = fc1 & 0x80;
  uint16_t hdrLen = 24;                   // addr1,addr2,addr3,seq_ctrl
  if (qos)   hdrLen += 2;                 // QoS Control
  if (order) hdrLen += 4;                 // HT Control

  // header + 8-byte LLC/SNAP (ethertype is the last 2 of those 8) + the
  // first 2 bytes of the 802.1X header (version, type) read just below.
  if (L < (uint16_t)(hdrLen + 8 + 2)) return;

  static const uint8_t SNAP_HDR[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
  const uint8_t *llc = p + hdrLen;
  if (memcmp(llc, SNAP_HDR, 6) != 0) return;
  uint16_t ethertype = (uint16_t)((llc[6] << 8) | llc[7]);
  if (ethertype != 0x888E) return;        // not EAPOL -- the overwhelming majority of data
                                          // frames (encrypted payload) die here or earlier

  const uint8_t *eapol = llc + 8;         // 802.1X header: version(1) type(1) length(2)
  uint8_t ieee8021xType = eapol[1];

  // Resolve BSSID/STA from ToDS/FromDS (802.11-2020 Table 9-26): FromDS=1,
  // ToDS=0 => AP->STA, addr2(TA)=BSSID, addr1(RA)=STA. ToDS=1,FromDS=0 =>
  // STA->AP, addr1(RA)=BSSID, addr2(TA)=STA. The toDs==fromDs check above
  // already ruled out the other two combinations.
  const uint8_t *bssid, *sta;
  if (fromDs) { bssid = p + 10; sta = p + 4; }
  else        { bssid = p + 4;  sta = p + 10; }

  EapolRingEntry e;
  memset(&e, 0, sizeof(e));
  memcpy(e.bssid, bssid, 6);
  memcpy(e.sta, sta, 6);
  e.channel = pkt->rx_ctrl.channel;
  e.rssi    = pkt->rx_ctrl.rssi;

  if (ieee8021xType == 3) {
    // EAPOL-Key. Body: descriptor-type(1) keyInfo(2) keyLen(2) replayCounter(8) ...
    if (L < (uint16_t)(hdrLen + 8 + 11)) return;
    uint16_t keyInfo = (uint16_t)((eapol[5] << 8) | eapol[6]);
    bool ack = keyInfo & 0x0080, mic = keyInfo & 0x0100, install = keyInfo & 0x0040, secure = keyInfo & 0x0200;
    WifiIdsEapolMsg msg = WIDS_EAPOL_OTHER;
    if (ack && !mic)                                 msg = WIDS_EAPOL_M1;
    else if (!ack && mic && !install && !secure)     msg = WIDS_EAPOL_M2;
    else if (ack && mic && install)                  msg = WIDS_EAPOL_M3;
    else if (!ack && mic && secure)                  msg = WIDS_EAPOL_M4;
    if (msg == WIDS_EAPOL_OTHER) return;   // group-key handshake / malformed -- not our concern here

    e.kind = WIDS_DF_EAPOL;
    e.seq  = (L >= 24) ? (uint16_t)((p[22] | (p[23] << 8)) >> 4) : 0;
    e.msg  = (uint8_t)msg;
    e.replayHi = (uint16_t)((eapol[9] << 8) | eapol[10]);   // top 16 bits of the 64-bit counter
    widsPushEapol(e);

  } else if (ieee8021xType == 0) {
    // EAP-Packet -- only interesting to us if it's WSC/WPS: Expanded EAP
    // type (254) carrying the WFA vendor id 00:37:2A, vendor-type 1
    // (SimpleConfig). Layout after the 4-byte 802.1X header: code(1) id(1)
    // len(2) type(1) vendorId(3) vendorType(4) -- RFC 3748 Expanded Type.
    if (L < (uint16_t)(hdrLen + 8 + 16)) return;
    uint8_t code = eapol[4];
    uint8_t eapType = eapol[8];
    if (eapType != 254) return;
    static const uint8_t WFA_OUI[3] = { 0x00, 0x37, 0x2A };
    if (memcmp(eapol + 9, WFA_OUI, 3) != 0) return;
    uint32_t vendorType = ((uint32_t)eapol[12] << 24) | ((uint32_t)eapol[13] << 16) |
                           ((uint32_t)eapol[14] << 8)  | eapol[15];
    if (vendorType != 1) return;           // not WFA SimpleConfig
    if (code != 1 && code != 2) return;    // only Request/Response matter (1=Req, 2=Resp)

    e.kind = WIDS_DF_WPS;
    e.eapCode = code;
    widsPushEapol(e);
  }
  // other 802.1X packet types (1=EAPOL-Start, 2=EAPOL-Logoff, 4=EAPOL-Encapsulated-ASF-Alert) ignored
}

static void IRAM_ATTR widsRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;

  if (type == WIFI_PKT_DATA) {
    if (s_wantEapol) widsHandleDataFrame(pkt);
    return;
  }
  if (!s_ring || type != WIFI_PKT_MGMT) return;
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
  if (s_hopSetN > 0) {
    s_hopIdx = (uint8_t)((s_hopIdx + 1) % s_hopSetN);
    s_chan = s_hopSet[s_hopIdx];
  } else {
    s_chan = (s_chan >= WIFI_IDS_CHAN_MAX) ? WIFI_IDS_CHAN_MIN : (uint8_t)(s_chan + 1);
  }
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

// Applies the current filter mask (MGMT, plus DATA when an EAPOL/WPS
// consumer wants it) to the running driver. Called from wifiIdsBegin() and,
// if the core is already up, from wifiIdsWantEapol() -- so flipping EAPOL
// capture on/off mid-session takes effect immediately instead of needing a
// screen re-entry.
static void widsApplyFilter() {
  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                      (s_wantEapol ? WIFI_PROMIS_FILTER_MASK_DATA : 0);
  esp_wifi_set_promiscuous_filter(&filt);
}

void wifiIdsBegin() {
  if (s_refs++ > 0) return;                 // already running for another consumer

  s_ring = (RingEntry *)calloc(WIDS_RING_LEN, sizeof(RingEntry));
  if (!s_ring) {                            // OOM: give up cleanly, detectors just get nothing
    Serial.printf("[wifi_ids] ring alloc failed, free heap %u\n", (unsigned)ESP.getFreeHeap());
    s_refs = 0;
    return;
  }
  if (s_wantEapol) {
    s_eapolRing = (EapolRingEntry *)calloc(WIDS_EAPOL_RING_LEN, sizeof(EapolRingEntry));
    // OOM here is non-fatal: EAPOL/WPS detectors just see nothing, same
    // "give up cleanly" policy as the mgmt ring -- the screen still works.
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

  widsApplyFilter();   // MGMT, plus DATA if wifiIdsWantEapol(true) was called

  esp_wifi_set_promiscuous_rx_cb(&widsRxCb);

  s_head = s_tail = 0;
  s_eapolHead = s_eapolTail = 0;
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
  free(s_eapolRing);
  s_eapolRing = nullptr;
  Serial.printf("[wifi_ids] end, free heap %u\n", (unsigned)ESP.getFreeHeap());
}

int wifiIdsRegisterEapol(WifiIdsEapolFn fn, void *ctx) {
  if (!fn) return -1;
  for (int i = 0; i < WIDS_MAX_EAPOL_DETECTORS; i++) {
    if (!s_eapolDet[i].fn) { s_eapolDet[i].fn = fn; s_eapolDet[i].ctx = ctx; return i; }
  }
  return -1;
}
void wifiIdsUnregisterEapol(int handle) {
  if (handle < 0 || handle >= WIDS_MAX_EAPOL_DETECTORS) return;
  s_eapolDet[handle].fn = nullptr; s_eapolDet[handle].ctx = nullptr;
}

int wifiIdsRegisterWps(WifiIdsWpsFn fn, void *ctx) {
  if (!fn) return -1;
  for (int i = 0; i < WIDS_MAX_WPS_DETECTORS; i++) {
    if (!s_wpsDet[i].fn) { s_wpsDet[i].fn = fn; s_wpsDet[i].ctx = ctx; return i; }
  }
  return -1;
}
void wifiIdsUnregisterWps(int handle) {
  if (handle < 0 || handle >= WIDS_MAX_WPS_DETECTORS) return;
  s_wpsDet[handle].fn = nullptr; s_wpsDet[handle].ctx = nullptr;
}

void wifiIdsWantEapol(bool on) {
  if (s_wantEapol == on) return;
  s_wantEapol = on;
  if (s_refs == 0) return;   // not running yet -- wifiIdsBegin() picks this up when it starts
  widsApplyFilter();         // already running -- take effect immediately
  if (on && !s_eapolRing) {
    s_eapolRing = (EapolRingEntry *)calloc(WIDS_EAPOL_RING_LEN, sizeof(EapolRingEntry));
    s_eapolHead = s_eapolTail = 0;
  } else if (!on && s_eapolRing) {
    free(s_eapolRing);
    s_eapolRing = nullptr;
  }
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

  // EAPOL/WPS ring -- same bounded-drain discipline, separate small ring
  // (see widsHandleDataFrame). No-op (empty ring, head==tail) when nobody
  // called wifiIdsWantEapol(true).
  int eapolBudget = WIDS_EAPOL_RING_LEN;
  while (s_eapolTail != s_eapolHead && eapolBudget-- > 0) {
    const EapolRingEntry &e = s_eapolRing[s_eapolTail];
    if (e.kind == WIDS_DF_EAPOL) {
      WifiIdsEapol ev;
      memcpy(ev.bssid, e.bssid, 6);
      memcpy(ev.sta, e.sta, 6);
      ev.channel = e.channel;
      ev.rssi    = e.rssi;
      ev.seq     = e.seq;
      ev.msg     = (WifiIdsEapolMsg)e.msg;
      ev.replayCounterHi = e.replayHi;
      for (int i = 0; i < WIDS_MAX_EAPOL_DETECTORS; i++)
        if (s_eapolDet[i].fn) s_eapolDet[i].fn(ev, s_eapolDet[i].ctx);
    } else {
      WifiIdsWps wv;
      memcpy(wv.bssid, e.bssid, 6);
      memcpy(wv.sta, e.sta, 6);
      wv.channel = e.channel;
      wv.rssi    = e.rssi;
      wv.eapCode = e.eapCode;
      for (int i = 0; i < WIDS_MAX_WPS_DETECTORS; i++)
        if (s_wpsDet[i].fn) s_wpsDet[i].fn(wv, s_wpsDet[i].ctx);
    }
    s_eapolTail = (uint16_t)((s_eapolTail + 1) % WIDS_EAPOL_RING_LEN);
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

void wifiIdsHopSet(const uint8_t *channels, uint8_t count) {
  if (count > WIDS_HOP_SET_MAX) count = WIDS_HOP_SET_MAX;
  memcpy(s_hopSet, channels, count);
  s_hopSetN = count;
  s_pinned  = false;
  s_hopIdx  = 0;
  if (s_refs > 0 && count > 0) {
    s_chan = s_hopSet[0];
    esp_wifi_set_channel(s_chan, WIFI_SECOND_CHAN_NONE);
  }
  s_lastHop = millis();
}

void wifiIdsHopResume() {
  s_pinned  = false;
  s_hopSetN = 0;
  s_lastHop = 0;                            // hop on the next wifiIdsLoop()
}

uint8_t  wifiIdsChannel() { return s_chan; }
uint32_t wifiIdsDropped() { return s_dropped; }
