#pragma once
// Pin map for the 2.8" ESP32 CYD wireless multi-tool build.
// Base board pins per ~/.claude/skills/esp32-cyd/SKILL.md (LCDWiki
// E32R28T/E32N28T). RGB LED: red = status/armed, blue = busy heartbeat,
// green free. CC1101 CS moved to GPIO27 so the blue channel is a LED again.

// --- LCD (ILI9341, global `SPI` bus) ---
#define TFT_CS   15
#define TFT_DC   2
#define TFT_SCK  14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_BL   21

// --- Touch (XPT2046, bit-banged -- see cyd_selftest for why) ---
#define TP_SCK  25
#define TP_MOSI 32
#define TP_MISO 39
#define TP_CS   33
#define TP_IRQ  36

// --- Status LEDs (RGB, common-anode, active LOW). Red (22) =
//     armed-engagement blinker / alert. Blue (17) = "working" heartbeat
//     during scans. Green (16) = event blips (wardrive new contact,
//     passphrase validating) + alternates with blue on Skimmer. ---
#define LED_STATUS 22   // red
#define LED_BUSY   17   // blue -- "working" heartbeat during scans etc.
#define LED_GREEN  16   // green -- event blips

// --- SD card (dedicated HSPI bus, separate from the LCD's global-SPI bus
//     so neither driver fights over pin remapping) ---
#define SD_CS     5
#define SD_SCK    18
#define SD_MOSI   23
#define SD_MISO   19
// GPIO16 -- now LED_GREEN (was PN532_CS; NFC/RFID dropped, see skimmer_detect.cpp)

// --- CC1101 SubGHz (shares the LCD's global-SPI bus/pins, own CS) ---
#define CC1101_CS 27   // shares TFT_SCK/MOSI/MISO (14/13/12); moved off 17 (now LED_BUSY)

// --- 2.4 GHz add-on radio: nRF24L01+ OR CC2500 (shares the global-SPI bus).
//     No compiled-in default -- assign in System > Hardware > SPI/IRQ pins
//     after you pick which radio is installed. nRF24 CE ties to 3V3. ---
#define RADIO24_CS  -1
#define RADIO24_IRQ -1   // nRF24 IRQ / CC2500 GDO0

// --- Misc onboard ---
#define BAT_ADC   34
#define AUDIO_EN  4
#define AUDIO_DAC 26
#define BOOT_KEY  0

// --- GPS (NMEA, receive-only -- input-only pin, no command path needed) ---
#define GPS_RX 35
