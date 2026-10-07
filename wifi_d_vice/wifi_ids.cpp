// Shared passive 802.11 promiscuous-capture core.
// See wifi_ids.h for the API and coexistence notes.
//
// This file contains two main parts.
// The RX callback runs in the WiFi driver task context.
// It parses the fixed header.
// It copies a snapshot into a lock-free ring.
// It returns immediately.
// The drain function runs in the main task context.
// It advances the hopper and pops the ring.
// It calls each subscribed detector once per frame.
//
// We use a ring buffer instead of direct dispatch.
// Direct dispatch forces detector callbacks to be RX-context-safe.
// It runs detector work inside the capture path during floods.
// The ring decouples the capture path from the detector work.
// A flood outrunning the drain function drops frames when the ring fills.
// This tradeoff suits flood detection.
// The drop count signals the flood.

#include <WiFi.h>
#include "esp_wifi.h"
#include "wifi_ids.h"
#include "debuglog.h"

// Capture ring

// The ring holds 24 entries. This size absorbs short bursts between loop calls.
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

// We allocate the ring on the heap. This saves static RAM for other screens.
static RingEntry        *s_ring = nullptr;
static volatile uint16_t s_head = 0;      // producer: widsRxCb
static volatile uint16_t s_tail = 0;      // consumer: wifiIdsLoop
static volatile uint32_t s_dropped = 0;

// Detector table

// We reserve space for four detectors plus two extra slots.
static const int WIDS_MAX_DETECTORS = 6;

struct Detector {
  WifiIdsDetectorFn fn;
  void             *ctx;
  uint16_t          mask;
};
static Detector          s_det[WIDS_MAX_DETECTORS];
// This mask tracks all registered detector requests. The RX callback drops unrequested subtypes. This keeps the capture path fast.
static volatile uint16_t s_wantMask = 0;

// EAPOL and WPS exception
//
// EAPOL and WPS frames need less space than management frames. We use a smaller ring for them. A kind tag shares this ring between both families.
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

// Hopper and lifecycle state

static int      s_refs    = 0;
static uint32_t s_dwellMs = 250;
static uint32_t s_lastHop = 0;
static uint8_t  s_chan    = WIFI_IDS_CHAN_MIN;
static bool     s_pinned  = false;

// This array holds a restricted channel list. The hopper cycles through this list. A count of zero means the hopper uses the full channel sweep.
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

// RX callback

// We mark this function for IRAM. This keeps it safe if the core runs it near interrupt context. The function only copies data to the ring. It defers all heavy work.
// This function pushes one EAPOL or WPS sighting. It drops the frame silently if the ring is full. We do not count these drops.
static inline void widsPushEapol(const EapolRingEntry &e) {
  if (!s_eapolRing) return;
  uint16_t next = (uint16_t)((s_eapolHead + 1) % WIDS_EAPOL_RING_LEN);
  if (next == s_eapolTail) return;
  s_eapolRing[s_eapolHead] = e;
  s_eapolHead = next;
}

// This function checks if a data frame carries EAPOL or WPS. It orders checks from cheapest to most expensive. This rejects encrypted IP traffic quickly.
static void IRAM_ATTR widsHandleDataFrame(const wifi_promiscuous_pkt_t *pkt) {
  const uint8_t *p = pkt->payload;
  uint16_t L = pkt->rx_ctrl.sig_len;

  // We check the frame type bits. Type two indicates a data frame. The header length calculation handles QoS fields. Frames without LLC payloads fail the length check later.
  if (((p[0] >> 2) & 0x3) != 2) return;

  uint8_t fc1 = p[1];
  bool toDs   = fc1 & 0x01, fromDs = fc1 & 0x02;
  // We require exactly one of ToDS or FromDS to be set. This rejects ad-hoc networks and inter-AP links. We only process client-to-AP or AP-to-client links.
  if (toDs == fromDs) return;   // need exactly one of ToDS/FromDS: reject ad-hoc/IBSS (00,
                                 // no AP involved) and WDS (11, inter-AP -- a different address-
                                 // field shape entirely, not a client<->AP link we can resolve
                                 // BSSID/STA from the same way)

  bool qos    = (p[0] & 0xF0) == 0x80;    // subtype nibble 0x8 (QoS-Data) with bit3 set
  bool order  = fc1 & 0x80;
  uint16_t hdrLen = 24;                   // addr1,addr2,addr3,seq_ctrl
  if (qos)   hdrLen += 2;                 // QoS Control
  if (order) hdrLen += 4;                 // HT Control

  // We verify the frame length. It must contain the header, the LLC/SNAP block, and the 802.1X header.
  if (L < (uint16_t)(hdrLen + 8 + 2)) return;

  static const uint8_t SNAP_HDR[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
  const uint8_t *llc = p + hdrLen;
  if (memcmp(llc, SNAP_HDR, 6) != 0) return;
  uint16_t ethertype = (uint16_t)((llc[6] << 8) | llc[7]);
  // We check the ethertype. Most data frames carry encrypted payloads. They fail this check.
  if (ethertype != 0x888E) return;        // not EAPOL -- the overwhelming majority of data
                                          // frames (encrypted payload) die here or earlier

  const uint8_t *eapol = llc + 8;         // 802.1X header: version(1) type(1) length(2)
  uint8_t ieee8021xType = eapol[1];

  // We resolve the BSSID and station addresses. The ToDS and FromDS flags determine which address field holds each value.
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
    // We process EAPOL-Key frames. The key body contains descriptor, key info, key length, and replay counter fields.
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
    // We check for WPS packets. We look for Expanded EAP type 254. We verify the WFA vendor ID and vendor type.
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
  // We ignore other 802.1X packet types.
}

static void IRAM_ATTR widsRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;

  if (type == WIFI_PKT_DATA) {
    if (s_wantEapol) widsHandleDataFrame(pkt);
    return;
  }
  if (!s_ring || type != WIFI_PKT_MGMT) return;
  const uint8_t *p = pkt->payload;

  // We check the frame type bits. Type zero indicates a management frame.
  if (((p[0] >> 2) & 0x3) != 0) return;
  uint8_t st = (p[0] >> 4) & 0xF;
  // We drop frames for subtypes that have no registered detectors.
  if (!((s_wantMask >> st) & 1u)) return;      // nobody subscribed to this subtype

  uint16_t next = (uint16_t)((s_head + 1) % WIDS_RING_LEN);
  // The ring is full. We drop the frame to keep the capture path bounded.
  if (next == s_tail) { s_dropped++; return; } // ring full: drop, keep the path bounded

  RingEntry &e = s_ring[s_head];
  e.subtype = st;
  e.channel = pkt->rx_ctrl.channel;
  e.rssi    = pkt->rx_ctrl.rssi;

  uint16_t L = pkt->rx_ctrl.sig_len;
  e.onAirLen = L;
  uint16_t n = (L > WIFI_IDS_SNAP_LEN) ? WIFI_IDS_SNAP_LEN : L;
  e.snapLen = n;

  // We copy sequence and address fields. We check the frame length to avoid reading runt frames.
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

// Hopper

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
  // The channel set call may fail on restricted regions. We simply stay on the current channel.
  esp_wifi_set_channel(s_chan, WIFI_SECOND_CHAN_NONE);   // failure (12/13 off-regdomain) just leaves us where we were
}

// Public API

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

// This function applies the current filter mask to the driver. It enables data frame capture when EAPOL or WPS detection is active. The filter updates immediately without restarting the driver.
static void widsApplyFilter() {
  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                      (s_wantEapol ? WIFI_PROMIS_FILTER_MASK_DATA : 0);
  esp_wifi_set_promiscuous_filter(&filt);
}

void wifiIdsBegin() {
  if (s_refs++ > 0) return;                 // already running for another consumer

  s_ring = (RingEntry *)calloc(WIDS_RING_LEN, sizeof(RingEntry));
  // Memory allocation failed. We stop the driver and return.
  if (!s_ring) {                            // OOM: give up cleanly, detectors just get nothing
    DLOG("wifi_ids", "ring alloc failed, free heap %u", (unsigned)ESP.getFreeHeap());
    s_refs = 0;
    return;
  }
  if (s_wantEapol) {
    s_eapolRing = (EapolRingEntry *)calloc(WIDS_EAPOL_RING_LEN, sizeof(EapolRingEntry));
    // EAPOL ring allocation failed. We continue without it. The main screen still works.
    // OOM here is non-fatal: EAPOL/WPS detectors just see nothing, same
    // "give up cleanly" policy as the mgmt ring -- the screen still works.
  }
  DLOG("wifi_ids", "begin, free heap %u", (unsigned)ESP.getFreeHeap());

  // We start the driver in STA mode. We do not use NULL mode. NULL mode de-initializes the WiFi driver on this core. STA mode keeps the driver active.
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
  // We drop to STA mode. We avoid NULL mode. NULL mode de-initializes the driver. STA mode keeps the driver warm for subsequent scans.
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
  DLOG("wifi_ids", "end, free heap %u", (unsigned)ESP.getFreeHeap());
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

  // We limit the drain loop to one full ring per call. This prevents a sustained flood from starving the UI.
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

  // We drain the EAPOL and WPS ring. We apply the same bounded loop discipline. The loop skips if the ring is empty.
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
  // We enforce a minimum dwell time. The PHY needs this time to settle on each channel.
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
