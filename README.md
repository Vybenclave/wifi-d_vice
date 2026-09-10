<img src="docs/vice.9_portrait.jpeg" width="360" alt="WIFI D_VICE">

# WIFI D_VICE

WIFI D_VICE is a passive radio-monitoring tool for the 2.8-inch ESP32
"Cheap Yellow Display" (CYD). It receives and shows WiFi, Bluetooth Low
Energy (BLE), and sub-GHz radio activity.

## About the project

Other CYD tools, such as Marauder and Bruce, showed that this low-cost
board can do useful radio work. WIFI D_VICE starts from that idea and takes
a different direction:

- It analyzes received radio traffic. It does not generate attack traffic.
- It uses as much of the CYD's small memory and radio time as possible. It
  runs a shared capture core, tight fixed-size data structures, and one
  active screen at a time.
- It keeps the radio hardware and the tool set flexible. You can add
  radios on the shared SPI bus (see "Add-on radios" below) and you can add
  screens.

The goal is a low-cost way to learn about radio protocols and about
reconnaissance. Each detector is small and readable. You can see what a
WiFi deauthentication flood, a beacon flood, or an evil-twin access point
looks like in the data. Later work will make the tool more useful for
learning and for field reconnaissance.

## Features

- WiFi scan and connect, plus an access-point direction finder that uses
  a tone that changes with signal strength.
- WiFi IDS (intrusion detection): one shared promiscuous-mode capture core
  with four detectors: deauthentication and disassociation flood; beacon
  flood; authentication and association flood; rogue access point.
- Rogue access point and evil twin: learn a known-good baseline of your
  access points to the SD card. Get an alert when a known network name
  comes from an unknown radio, or with weaker security, or on a different
  channel.
- BLE scan and a tracker detector (for example AirTag, SmartTag, and
  Tile).
- Flock Safety camera detection.
- BLE skimmer detection.
- CC1101 sub-GHz sweep (315, 433, 868, and 915 MHz).
- Meshtastic mesh monitor over BLE. It links to a Meshtastic node and shows
  the traffic that the node hears.
- GPS wardriving, with optional AES-256-GCM encrypted logs.
- Net stats: internet and local-network throughput tests, connection
  information, and a NAT (network address translation) test.
- Engagement: operator tracking and scan-data protection for authorized
  work. See below.

## Engagement

For use in a professional or authorized setting, an "engagement" records
who runs the tool and protects the data it writes to the SD card.

- You set a client name, a tester name, and a passphrase. A BLE Secure
  Connections pairing with a phone or laptop is the second factor that
  arms the engagement.
- While the engagement is armed, the SD logs (scans and wardriving) are
  written as AES-256-GCM records instead of plain text. The key comes from
  the passphrase through PBKDF2 and is never written to flash.
- The client name, tester name, and arm time are in each log header, so a
  pulled card shows its own provenance without the key.

This part is early. It will grow over time: more of the tool's output
under the same protection, and clearer operator and job tracking.

## Hardware

- Board: ESP32-WROOM-32 CYD (E32R28T). A 2.8-inch ILI9341 display. An
  XPT2046 touch controller.
- Optional GPS module on a serial port.
- Optional 1S lithium-polymer (LiPo) battery. The firmware reads the
  battery voltage on GPIO34 and shows a calibrated level.

### Add-on radios

All add-on radios share one SPI bus (SCK, MOSI, MISO) and use one
chip-select GPIO each. Do not use GPIO34 to GPIO39 for chip select: they
are input only.

- CC1101 (sub-GHz). Used now for the 315, 433, 868, and 915 MHz sweep.
- nRF24L01+ (2.4 GHz). Planned for a 2.4 GHz energy and jamming watch: a
  channel sweep that reads the RPD (received power detector) bit many times
  per channel for a per-channel occupancy value. Connect CE to 3.3 V and
  read IRQ in software, so the radio needs only one free GPIO for chip
  select.
- CC2500 (2.4 GHz). A drop-in option for the same sweep on the same SPI
  bus and chip-select pin. It adds a real RSSI value and noise-floor
  detection. Use it if the nRF24L01+ RPD threshold (about -64 dBm) is too
  coarse.

See [`wifi_d_vice/DESIGN.md`](wifi_d_vice/DESIGN.md) for the 2.4 GHz sweep
design.

## Build

See [`wifi_d_vice/README.md`](wifi_d_vice/README.md) for the full build
steps and the internal rules. In short:

```
cd wifi_d_vice
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
arduino-cli upload  -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

## Roadmap

- Expanded Meshtastic features while the tool is paired to a Meshtastic
  node over BLE: node list, live message and telemetry feed, node radio
  settings, and clock set from the mesh. It will also send alert text
  messages to user-defined private channels, for example when the WiFi
  IDS raises an alert.
- A plugin framework, so the community can add tools and screens without a
  change to the core. A manifest describes each plugin; a later stage adds
  an optional script virtual machine.
- Guardian mode: one "on watch" mode that runs a selected set of monitors
  together and sends every alert to one screen.
- A 2.4 GHz energy and jamming watch with the nRF24L01+ or a CC2500.
- Wider engagement coverage: more of the tool's output under the armed-mode
  protection, and fuller operator and job tracking.

See [`wifi_d_vice/DESIGN.md`](wifi_d_vice/DESIGN.md) for details.

## Known issues

- Speed test (Net stats > Speed test) does not reach the expected maximum.
  An ESP32-WROOM-32 can move about 20 to 40 Mbps total on WiFi, but the
  test reports much less. The cause is not yet found. Read the result as a
  rough "the link works" check, not as a true bandwidth figure. The LAN
  speed test on the same screen has the same limit.

## Legal use

- Monitor only the radio traffic that you have permission to monitor. The
  laws on interception of radio communications are different in each
  country. You are responsible for legal use.
- Keep the sub-GHz and 2.4 GHz add-on radios inside the legal frequency
  bands and power limits for your country. The firmware does not enforce a
  region; you set the frequency.

## About this build

This is an enthusiast project, built for fun and as an experiment. I leaned
on AI coding assistants to explore what a tool of this kind can do on cheap
hardware, and to learn first-hand where "vibe coding" pays off and where it
bites back. It moved the UI and detector work along quickly and put a
working device on the bench sooner than I would have managed alone. It also
produced a few subtle bugs that only hardware testing exposed, some dead
ends, and code that needed a cleanup pass for consistency. Take it as a
learning project first and a field tool second, and read the code before
you rely on it.

## Credits

- The MAC OUI -> vendor table (`wifi_d_vice/mac_vendor.cpp`) is a curated
  subset of Wireshark's `manuf` list:
  https://www.wireshark.org/download/automated/data/manuf

## License

MIT. See [`LICENSE`](LICENSE).
