# WIFI D_VICE

WIFI D_VICE is a passive radio-monitoring tool for the 2.8-inch ESP32
"Cheap Yellow Display" (CYD). The board is an E32R28T: an ESP32-WROOM-32,
an ILI9341 display, and an XPT2046 touch controller.

The tool analyzes received radio traffic. It does not generate attack
traffic; validate detectors with traffic from other equipment.

This document gives the build steps and the internal rules. `DESIGN.md`
gives the roadmap.

## Build and flash

```
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
arduino-cli upload  -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

Rules:

- Give `.` as the last argument. Give the sketch directory, not a file.
- Use Arduino-ESP32 core 3.3.x.
- The CYD has a CH340 serial bridge. On Linux the port is `/dev/ttyUSB*`,
  not `/dev/ttyACM*`. The port number can change between `ttyUSB0` and
  `ttyUSB1` when you disconnect and reconnect the cable.

Recovery:

- To erase the SPI pin overrides, hold the BOOT button when you apply
  power.
- To start a touch recalibration, hold the BOOT button for 1.5 seconds at
  any time.

## Internal rules

The code refers to these rules. It does not repeat them at each location.

### Radio coexistence

DRAM is the internal RAM of the ESP32. The WiFi driver uses approximately
50 KB of DRAM. The BLE host and controller stack also use a large part of
the DRAM. There is not enough DRAM for both plus the rest of the firmware.
If a screen starts the second stack while the first is still resident, the
init usually does not get enough memory and the firmware stops with a
panic. If a WiFi station link was active, the result is a continuous
restart. If no link was active, the result is a stop with no output. So a
screen tears one stack down before it starts the other.

Rules:

- Before a screen that uses BLE starts BLE (`ble_scan`, `tracker_detect`,
  `skimmer_detect`, `meshtastic_mon`, `engagement`, `webdl`,
  `ble_spam_detect`, and `drone_detect` on its BLE tab): call
  `WiFi.disconnect(true, false)`, then `WiFi.mode(WIFI_OFF)`, then a short
  `delay`, then `BLEDevice::init()`. `WiFi.disconnect(true, false)` ends an
  active link and releases its network sockets. It keeps the saved
  credentials.
- The Recon screens `probe_watch` and `client_map`, and `drone_detect` on
  its WiFi tab, hold the shared `wifi_ids` promiscuous core -- same
  constraint as `wifi_ids` itself: not compatible with a concurrent STA
  link or with BLE. `drone_detect` tears one stack fully down before
  bringing up the other when its tab is switched.
- When a screen that uses BLE stops: call `BLEDevice::deinit(false)` and set
  the cached `BLEScan*` pointer to null. This releases the memory for the
  next WiFi screen.
- Flock detect is the exception. It scans WiFi and BLE in turn on a repeat
  loop, and it keeps BOTH stacks resident for the whole time the screen is
  open. The two scans do not run at the same instant, but the WiFi driver
  and the BLE stack hold their memory together. This only fits because of
  the Classic Bluetooth release below. BLE is freed on flockExit(). A true
  hand-off (WiFi off before each BLE phase) is a planned change; today it
  is not done.
- `setup()` calls `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)`.
  This build uses BLE only. This call releases approximately 28 KB of
  Classic Bluetooth controller memory for the full session. It is what
  gives Flock detect the headroom to hold both stacks.

### Do not use `WIFI_MODE_NULL` between screens

`WiFi.mode(WIFI_MODE_NULL)` stops the WiFi driver on this core. The next
start of station mode then does a full radio recalibration. The
recalibration takes 15 to 20 seconds. During the recalibration, scans
return no results and `WiFi.begin()` fails with an incorrect "wrong
password" status.

A screen that owns the radio leaves it in station mode when it exits. For
example, `wifi_ids` leaves promiscuous mode and returns to station mode. It
does not use `WIFI_MODE_NULL`. A new station-mode user waits 200 to 250
milliseconds before the first scan or the first `begin()`, because the
radio can be cold.

### One active screen

One feature screen is active at a time. That screen owns the radio, the
callback slot for promiscuous receive (`wifi_ids` has the only one), the
digital-to-analog converter (DAC), and the SD file handle (`wlog` keeps one
file open). `Enter()` does the setup. `Exit()` does the teardown. `Loop()`
runs only while the screen is active. A screen does not need to protect
against a second screen at the same time. Guardian mode (roadmap) will be
the first feature that runs a set of monitors together.

### User-interface rules

- The top bar holds the back button and the title only. Put other buttons
  in the action row with `uiDrawActionRow`. Do not put them in the top bar.
- Clear the content area with `uiClearBelow` or `uiClearRect`. These follow
  the theme: the Vice gradient or scene image, or flat black. Do not use
  `fillRect(BLACK)`.
- `UI_MENU_BTN_MAXSIZE` in `ui.h` sets the largest button-label text size.
  `uiDrawButton` makes the text smaller when it does not fit.
- The bottom `UI_STATUSBAR_H` pixels are a fixed bar: a black strip with a
  white line on top. The bar holds the message line, the clock-sync dot,
  and the battery symbol.
- A loop that does not yield (`delay(0)` or `yield()`) causes a task
  watchdog restart after approximately 5 seconds. A transfer loop or a scan
  loop must yield.

### Engagement and encryption

The Engagement page holds a client name, a tester name, and a passphrase. A
BLE Secure Connections pairing is the second factor that unlocks it. Today
the only effect of an armed engagement is on the SD logs. When the
engagement is armed, `wlog` and `gps_wardrive` write AES-256-GCM records in
hexadecimal instead of plain-text CSV. The firmware derives the key from the
passphrase with PBKDF2.

Each client is a folder at the root of the SD card. `/eng/active.txt` holds
the name of the selected client. The passphrase is the key. The firmware
does not write the key or the passphrase to flash memory.
