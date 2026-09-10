# WIFI D_VICE — design goals

WIFI D_VICE is a passive radio-monitoring tool for the 2.8-inch ESP32 CYD.
It analyzes received radio traffic. It does not generate attack traffic
(deauthentication, jamming, spoofing, floods, password attacks); validate
detectors with traffic from other equipment.

## Current detectors

- WiFi scan and an access-point direction finder that uses signal strength.
- Rogue access point and evil twin, with a known-good baseline on the SD
  card.
- BLE scan and a tracker detector (for example AirTag and SmartTag).
- Deauthentication-flood detector.
- Flock Safety camera detection.
- BLE skimmer detection.
- CC1101 sub-GHz sweep (315, 433, 868, and 915 MHz).
- Meshtastic mesh monitor over BLE.
- GPS wardriving.

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
- Beacon flood — a rise in the count of unique SSIDs or in the beacon rate,
  above a moving baseline; random-character SSIDs; sequential or random
  BSSID MAC addresses.
- Authentication and association flood — a high rate of authentication or
  association requests to one BSSID, from many different (and often
  invalid) source MAC addresses, in a short time.
- Deauthentication detector, version 2 — add disassociation frames, reason
  codes, and spoof detection. A spoof is a deauthentication that claims to
  come from a BSSID, but the sequence number or the RSSI does not match the
  real beacons of that BSSID.

Later work (separate notes): correlation of handshake theft (a
deauthentication, then an EAPOL 4-way handshake for the same client within
seconds); a PMKID-harvest pattern; the presence of a Pwnagotchi; KARMA (one
BSSID that answers with many ESSIDs); channel-switch-announcement abuse.

### 4. Cross-radio correlation

The most reliable alerts come from evidence that agrees across radios:

- The 2.4 GHz noise floor rises, AND the WiFi scan returns few or no access
  points, AND the BLE scan is empty. This is a jamming event. It is not
  only a strange sweep result.
- A deauthentication burst for client X, AND an EAPOL 4-way handshake for
  client X on the same channel within seconds. This is a handshake theft in
  progress.

Guardian mode is the place for these checks. It has the output of every
monitor.

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
