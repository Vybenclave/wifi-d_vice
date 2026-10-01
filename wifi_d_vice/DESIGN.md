# WIFI D_VICE — design goals

WIFI D_VICE is a passive radio-monitoring tool for the 2.8-inch ESP32 CYD.
It analyzes received radio traffic. It does not generate attack traffic
(deauthentication, jamming, spoofing, floods, password attacks); validate
detectors with traffic from other equipment.

## Current detectors

- WiFi scan and an access-point direction finder that uses signal strength.
- Rogue access point and evil twin, with a known-good baseline on the SD
  card. The WiFi IDS screen also carries a baseline-free evil-twin score
  (same SSID, different BSSID, divergent on randomized MAC / security
  downgrade / RSSI / channel) and a Pwnagotchi-presence check.
- BLE scan and a tracker detector (AirTag, SmartTag, Tile, Chipolo, and the
  Google Find My Device network).
- Deauthentication / disassociation flood detector, plus beacon-flood and
  auth/assoc-flood detectors, on one shared promiscuous core. The same core
  also carries KARMA detection, channel-switch-announcement abuse, and --
  via a narrow, opt-in widening to EAPOL/WPS-carrying data frames --
  handshake-theft correlation, PMKID-harvest detection, KRACK detection, and
  WPS brute-force detection. See section 3 for all of these.
- Flock Safety camera detection.
- BLE skimmer detection (module names + the 0xFFD0 UART-bridge service).
- Flipper Zero and Meta / Ray-Ban glasses tagging in the BLE scan.
- CC1101 sub-GHz screen with three modes: band sweep, frequency analyzer
  (strongest ISM centre), and raw OOK capture to a Flipper .sub file. Raw
  capture needs the CC1101 GDO0 line wired to a spare GPIO (System >
  Hardware); a live probe gates the mode until it is connected.
- Meshtastic mesh monitor over BLE.
- GPS wardriving.

### Recon category

Passive analysis screens added for Marauder / Wireless Wizard / Flipper
feature parity:

- Probe-request watch -- the SSIDs nearby clients are directed-probing for,
  cross-checked against beacons seen in the same hop sweep. A probed SSID
  with no matching beacon nearby is flagged "ABSENT" in red: the device is
  leaking a preferred-network-list entry for a network that is not actually
  present here (out of range, powered off, or hidden) -- the gap a KARMA /
  "answer every SSID" rogue AP is built to exploit. A cloaked AP (blank SSID
  in its beacon) is unmasked when it answers a directed probe with its real
  SSID in the probe response; that correlation flags the row magenta as
  "HIDDEN-AP" instead of ABSENT.
- Client / station map -- client devices in the air, from management
  frames (probe / assoc / auth): MAC, OUI vendor, last AP, RSSI, frames.
- WiFi camera detector -- beacon BSSID OUI against a camera-vendor table,
  plus an SSID substring pass; tap to direction-find.
- Drone Remote ID -- ASTM F3411 / open-drone-id over WiFi (beacon vendor
  IE, NaN action frames) and BLE (0xFFFA service data, DJI). Decodes the
  UAS serial and the operator-reported position.
- BLE advertisement-spam watch -- advert rate, distinct-address estimate,
  and Apple Continuity / SwiftPair / Fast-Pair frame counts feed one
  severity verdict.

## Roadmap

### 1. Guardian mode

Guardian mode is one "on watch" mode. It has a menu. In the menu you select
which monitors run together:

- WiFi IDS (management-frame anomaly and flood detection)
- 2.4 GHz energy and jamming watch
- BLE anomaly (tracker follow, spoofed beacon)
- Deauthentication watch
- Sub-GHz sweep

Guardian mode sends every alert from every selected monitor to one status
screen, to the RGB LED, and (optional) to a tone. It writes a rolling event
log to the SD card when the engagement is armed.

The menu is also the resource control. Not all monitors can run at the same
time. WiFi promiscuous capture, a 2.4 GHz sweep, and a BLE scan all use the
2.4 GHz band. The ESP32-WROOM-32 also cannot hold the WiFi driver and the
BLE stack at the same time (see `wifi_d_vice/README.md`, "Radio
coexistence"). Guardian mode selects a set that can run together and shares
one channel-hop schedule between the monitors that need it.

### 2. 2.4 GHz energy and jamming detection

Goal: detect 2.4 GHz energy that is not WiFi. This includes broadband noise
(barrage jamming), sweep jammers, continuous-wave carriers, and general
congestion.

- Radio: nRF24L01+ (on hand). Sweep channels RF_CH 0 to 125. On each
  channel, read the RPD bit 200 to 500 times and count the "1" results.
  The result is a per-channel occupancy value of 0 to 100 percent. The RPD
  bit sets at approximately -64 dBm. This method finds strong signals only.
  It finds barrage jammers, sweep jammers, continuous-wave jammers, and
  congestion. It does not measure the noise floor and it does not find weak
  or distant sources.
- Upgrade option: CC2500. It uses the same SPI bus and the same chip-select
  pin. It gives an RSSI value with approximately 1 dB resolution, a tunable
  receive bandwidth, and noise-floor detection. Add it only if the -64 dBm
  limit of the nRF24L01+ is too coarse.
- Alert logic: measure a per-channel occupancy baseline when the band is
  quiet and store it on the SD card. Then flag these conditions: a
  band-wide rise that stays high (barrage); a high-occupancy group that
  moves across channels on each sweep (sweep jammer); one channel near 100
  percent for a long time (continuous wave). Do not flag a microwave oven:
  it shows a strong signal near 2450 to 2460 MHz with a 50 Hz or 60 Hz
  on/off pattern.
- Wiring: the add-on radio shares the CC1101 SPI bus (SCK, MOSI, MISO).
  Connect the nRF24L01+ CE pin to 3.3 V for continuous receive and read the
  IRQ pin in software. The radio needs one free output GPIO for chip
  select. Do not use GPIO34 to GPIO39: they are input only. Turn off the
  ESP32 WiFi and BLE during a sweep, because the ESP32 radio adds energy to
  the low channels.

### 3. WiFi attack detection — expansion

One shared promiscuous-mode capture core (`wifi_ids.cpp`) feeds several
detectors. Each detector does not register its own receive callback. The
first detector was the deauthentication detector. It is now part of this
core.

- Evil twin and rogue access point — DONE. See `rogue_ap.cpp` and the
  "Rogue AP" screen. The tool learns a known-good baseline to
  `/rogueap.csv` on the SD card (ESSID, BSSID, security, channel, typical
  RSSI). It learns the baseline over 4 scan passes. `rogueApCheck()` gives
  an alert for these conditions: a known ESSID from an unknown BSSID (evil
  twin); a known ESSID and BSSID with weaker security (downgrade); a
  channel change; an RSSI change of more than 30 dB. The standalone screen
  uses the exact security type from `WiFi.encryptionType`. The WiFi IDS
  screen uses the coarse "encrypted or open" bit from the beacon. A match
  failure raises a "ROGUE-AP" alert in the WiFi IDS view. Not done yet: a
  check for a well-known brand SSID on a locally administered BSSID; a
  per-BSSID RSSI range instead of one typical value.
- Beacon flood — DONE. `wifi_ids_screen.cpp`'s `onBeacon()` / `sevBeacon()`.
  A Bloom-filter distinct-BSSID estimate over a 5 s window, random/binary
  SSID detection, locally-administered (randomized) BSSID MAC detection, and
  near-sequential BSSID detection (the mdk4 / aireplay beacon-spam
  signature).
- Authentication and association flood — DONE. `onAuthAssoc()` /
  `sevAuth()`. A Bloom-filter distinct-source-MAC estimate, a
  locally-administered source-MAC flag, and a per-BSSID target-hit table,
  over the same 5 s window.
- Deauthentication detector, version 2 — DONE. `onDeauthFamily()` /
  `sevDeauth()`. Disassociation frames, reason codes, and spoof detection: a
  deauth/disassoc that claims to come from a BSSID's own beacon baseline
  (learned live from `onBeacon()`) but whose channel, RSSI, or sequence
  number don't match.
- Pwnagotchi presence — DONE. `onBeacon()`: fixed-MAC match
  (`de:ad:be:ef:de:ad`) plus JSON name extraction from the beacon SSID. Not
  an attack in itself; surfaced as a WATCH-tier note, not an alert.
- KARMA detection — DONE. One BSSID answering probe requests for several
  different SSIDs. Management-frame only (`WIDS_PROBE_RESP`), no capture-core
  change needed.
- Channel-switch-announcement (CSA) abuse — DONE. An off-regulatory-domain
  target channel (hard violation, immediate alert), or a CSA re-announced /
  re-targeted without a real channel switch ever happening (the mdk4-style
  disruption signature). Management-frame only (action frames, subtype
  `0xD`, same precedent `drone_detect.cpp` already uses for Remote ID), no
  capture-core change needed.
- Handshake-theft correlation — DONE. A deauth for client X, then an EAPOL
  4-way handshake (message 2) for X on the same BSSID within 10 seconds.
  Needed widening the shared promiscuous core (`wifi_ids.cpp`) to admit
  EAPOL-carrying data frames — see below.
- PMKID-harvest detection — DONE. An EAPOL message-1 with no matching
  recent real association for that (STA, BSSID) pair — the clientless
  direct-association PMKID-grab signature (hcxdumptool-style). Framed as
  suspicion (WATCH), escalating to ALERT only with corroboration (2+
  distinct targeted STAs, or a message-1 that never saw a message-2 reply).
- KRACK detection — DONE. The same EAPOL message-3 replay counter seen
  twice for one (STA, BSSID) pair (the 2017 Vanhoef key-reinstallation
  signature). Rides the same EAPOL classification handshake-theft needed, no
  additional capture-core work.
- WPS brute-force detection — DONE. WSC (WiFi Simple Config) EAP-Packet
  attempt rate per BSSID, in the same 5 s window the flood detectors use
  (the Reaver/Bully PIN-brute-force signature). Needed a second
  capture-core admission path (802.1X EAP-Packet / Expanded-Type / WFA
  vendor id, alongside the EAPOL-Key path above) — see below. Alert
  thresholds are a starting point, not yet tuned against real Reaver/Bully
  traffic on hardware.

**EAPOL/WPS capture-core widening** (`wifi_ids.cpp`, `wifi_ids.h`): the
capture core was management-frame-only, twice over (the driver's own
promiscuous filter, and a second check in the RX callback). Handshake-theft,
PMKID-harvest, KRACK, and WPS brute-force all need to see EAPOL (WPA 4-way
handshake) or WSC (WPS) traffic, which rides on 802.11 DATA frames. Both are
themselves unencrypted (they ARE the key exchange / provisioning handshake),
so this is still fully passive — no PSK needed, nothing transmitted.
`wifiIdsWantEapol(true)` widens the filter to admit data frames; a cheap,
cascading admission filter in the RX callback (frame-type check → 802.11
header-length arithmetic from ToDS/FromDS/QoS/Order → LLC/SNAP + ethertype
`0x888E` → 802.1X packet type) rejects the non-EAPOL data-frame majority
(encrypted IP traffic) before any real parsing, and is opt-in per consumer
so a screen that never calls it pays zero extra per-packet cost. See
`wifi_ids.h`'s `WifiIdsEapol`/`WifiIdsWps` structs and
`wifiIdsRegisterEapol()`/`wifiIdsRegisterWps()`.

**Not done, deferred pending a feasibility probe (not a hardware gap):**
control-frame-based detection — a virtual carrier-sense / NAV-duration DoS
(bogus RTS/CTS with an inflated NAV field making other stations defer
transmission indefinitely) and PS-Poll spoofing (desyncing a client's
power-save buffered-frame delivery). Unlike the EAPOL case, there's no cheap
late-stage filter to reject most control-frame traffic before real parsing
(the two things worth inspecting — NAV duration, PS-Poll — sit in the very
frames you'd capture), control frames are typically the *highest*-rate
frame family on a busy channel, and ESP32 promiscuous-mode control-frame
delivery reliability is unverified from here. Needs a short isolated
hardware probe (flip on `WIFI_PROMIS_FILTER_MASK_CTRL`, watch
`wifiIdsDropped()` on a busy channel) before it's worth designing detector
logic for.

### 4. Cross-radio correlation

The most reliable alerts come from evidence that agrees across radios:

- The 2.4 GHz noise floor rises, AND the WiFi scan returns few or no access
  points, AND the BLE scan is empty. This is a jamming event. It is not
  only a strange sweep result.
- A deauthentication burst for client X, AND an EAPOL 4-way handshake for
  client X on the same channel within seconds. This is a handshake theft in
  progress. The single-radio version of this check (WiFi IDS screen only, no
  cross-radio evidence) is DONE — see section 3's handshake-theft
  correlator. Guardian mode's job here is to factor in BLE/sub-GHz evidence
  on top when deciding how loudly to alert, not to reimplement the
  correlation.

Guardian mode is the place for these checks. It has the output of every
monitor.

### 5. WiFi Pineapple Pager feature parity

The Hak5 WiFi Pineapple Pager's *detection* side (Recon dashboard, Alert
Payloads, Handshake Collection) has passive equivalents worth porting; its
offensive side (PineAP impersonation / Evil WPA / SSID Pool, forced-deauth
handshake capture, `DEAUTH_CLIENT`) does not fit this tool and stays out per
"Out of scope" below. Planned, in no particular order:

- Recon live graph -- a rolling view of packets/sec, AP count, and client
  count (data already tallied in `wifi_ids.cpp` / `wifi_scan.cpp`; needs a
  screen, not new capture logic).
- Per-entry "Examine" drill-down -- tap an AP or client in `wifi_scan.cpp` /
  `client_map.cpp` for a detail view: encryption, RSSI history, packet
  counts, associated clients/SSIDs.
- A unified alert framework -- the direct equivalent of Alert Payloads:
  per-category enable/disable (deauth flood, rogue AP, tracker, camera,
  drone, BLE skimmer, absent-SSID probe) with a configurable action
  (on-screen banner, tone, LED). This is the same thing as Guardian mode
  above; Guardian mode is the umbrella, this is its alert-config piece.
- Passive EAPOL handshake detection/logging -- log the WPA 4-way handshake
  as it happens naturally (client join, or the ~5-minute rekey), without
  forcing it via deauth. Store as pcap evidence only ("this handshake was
  capturable by a passive listener"); do not convert to a crackable format
  (hashcat hcapx etc.) -- that step is attack tooling, not detection.
- Raw WiFi PCAP logging to SD -- same idea as the sub-GHz raw OOK capture
  (`subghz_cc1101.cpp`), for 802.11 frames from the shared `wifi_ids` core.
- Recon view filters -- filter `client_map.cpp` / `wifi_scan.cpp` lists by
  vendor/OUI (`mac_vendor.cpp` already has the table) or by SSID substring.

## Out of scope

WiFi, BLE, and sub-GHz attack functions. If a detector needs attack traffic
to test, that traffic comes from separate equipment. The Meshtastic
alert-message path (above) is the exception.

## Meshtastic — planned expansion

The Meshtastic monitor is a BLE client to a Meshtastic node. Planned work,
while the tool is paired to a node over BLE:

- Show the node list with names, signal-to-noise ratio, hop count, and
  last-heard time.
- Show received text messages and telemetry in a live feed.
- Show channel and radio settings that the node reports.
- Use the node's time to set the tool clock when there is no WiFi.
- Send alert messages over the mesh to one or more private channels that
  the user defines. For example, send a short text message when the WiFi
  IDS raises an alert. The user selects which alerts send a message and
  which channel receives it.

The tool asks the paired Meshtastic node to send a standard text message.
The monitor does not relay or inject other mesh traffic.

## Plugin framework — planned

Goal: let the community add tools and screens without a change to the core.

- A manifest describes each plugin: name, menu location, the module
  visibility ID, and the radio or hardware it needs.
- The core validates the manifest and lists the plugin in the correct menu.
- A later stage adds an optional script virtual machine for plugin logic.
- Each plugin follows the same rules as a built-in screen: one active
  screen at a time, the radio coexistence rules, and the passive-only
  principle.
