// WIFI D_VICE -- passive WiFi / BLE / sub-GHz
// detection toolkit for the 2.8" ESP32 CYD. See README.md for the build,
// the global rules the code leans on (radio coexistence, single-screen
// ownership, UI conventions), and ~/.claude/skills/esp32-cyd + pins.h for
// the board / addon wiring.
//
// Features: WiFi + BLE scanning, a WiFi IDS (deauth/disassoc, beacon-flood,
// auth/assoc-flood, Pwnagotchi and a baseline-free evil-twin score on one
// shared promiscuous core), rogue-AP / evil-twin detection against an SD
// baseline, a WiFi-scan RSSI direction finder, Flock Safety camera
// detection, BLE tracker detection (incl. Google FMDN), Bluetooth skimmer
// detection, Flipper Zero / Meta-glasses tagging in the BLE scan, a CC1101
// SubGHz screen (sweep / frequency analyzer / GDO0-gated raw OOK capture to
// .sub), a Meshtastic mesh monitor, GPS-tagged wardriving, and a Recon
// category: probe-request watch, client/station map, WiFi camera-OUI
// detector, drone Remote ID (WiFi+BLE), and a BLE advertisement-spam watch.
// Analysis of received traffic -- no attack traffic is generated here (see
// DESIGN.md). NFC/RFID detection (PN532) was tried and dropped -- low value
// on its own, and covered better by a dedicated Chameleon-style tool over
// BLE.
//
// The Engagement Page (client/tester/passphrase, gated by native BLE
// Secure Connections passkey pairing + bonding as a second factor -- see
// ble_2fa.h) is arming infrastructure for whenever an active/offensive
// module gets added later with real engagement details; right now nothing
// in this build is offensive, so arming's only concrete effect today is
// switching gps_wardrive's SD logs from plaintext CSV to AES-256-GCM-
// encrypted records keyed from the passphrase. The same BLE connection also
// syncs wall-clock time from the phone/laptop via the standard Current Time
// Service (devtime.h) -- most OSes push this automatically, no extra step.
// The GPS module (gps_shared.h) is a second, WiFi/BLE-independent time
// source: it retries every 15s from boot until it gets a fix-derived UTC
// time, then resyncs hourly to correct drift. Every SD log timestamps rows
// in that same UTC clock (devTimeNowString()) regardless of which source
// set it; the bottom-bar clock shows it shifted by System > Display >
// Timezone, a display-only preference (tz.h) that never touches the logs.
//
// Everything here is unverified on physical hardware -- see pins.h and the
// esp32-cyd skill for the sourcing/caveats on the base pin map, and treat
// the addon (CC1101/GPS) wiring, and the BLE pairing-prompt behavior on
// your actual phone/laptop OS, as first-power-on territory.

#include <WiFi.h>
#include <nvs.h>
#include "esp_bt.h"
#include "ui.h"
#include "screens.h"
#include "system_screen.h"
#include "onboarding.h"
#include "devtime.h"
#include "tz.h"
#include "gps_shared.h"
#include "engagement.h"
#include "engstore.h"
#include "theme.h"
#include "accent.h"
#include "splash.h"
#include "modvis.h"
#include "pincfg.h"
#include "wifiauto.h"

// Two-level menu: the top level has Engagement and Meshtastic as standalone
// items, plus two categories (WiFi, Privacy) that open a submenu of their
// own items. System settings are reached from the gear icon in the menu's
// bottom-left corner, not a menu button. "Privacy" is the short label for
// "detecting surveillance aimed at you" -- Flock / Tracker / Skimmer / BLE
// scan / SubGHz are about spotting that, not opsec in general.
enum Screen {
  MENU,
  SUB_WIFI, SUB_CS, SUB_RECON,
  WIFI_SCAN, NET_STATS, BLE_SCAN, TRACKER, WIFI_IDS, ROGUE_AP, FLOCK, SKIMMER, SUBGHZ, MESHTASTIC, GPS_WARDRIVE,
  PROBE_WATCH, CLIENT_MAP, CAMERA_DET, DRONE_DET, BLE_SPAM,
  ENGAGEMENT, SYSTEM
};
Screen currentScreen = MENU;

// Set when wifiAutoConnectOnBoot() kicked a "connect on boot" join; cleared
// the first time loop() sees the link up, at which point SNTP is started.
static bool bootWifiPending = false;

// `mod` = the modvis.h id for hide/show (-1 = never hideable, e.g. a
// category container).
struct TopItem { const char *label; Screen target; int mod; };
static const TopItem kTop[] = {
  {"Engagement",   ENGAGEMENT, MOD_ENGAGEMENT},
  {"WiFi",         SUB_WIFI,   -1},
  {"Privacy",      SUB_CS,     -1},
  {"Recon",        SUB_RECON,  -1},
  {"Meshtastic",   MESHTASTIC, MOD_MESHTASTIC},
};
static const int kTopCount = sizeof(kTop) / sizeof(kTop[0]);
static Btn topButtons[kTopCount];

struct SubItem { const char *label; Screen target; int mod; };
static const SubItem kWifiItems[] = {
  {"WiFi scan",       WIFI_SCAN,    MOD_WIFI_SCAN},
  {"Net stats",       NET_STATS,    MOD_NET_STATS},
  {"WiFi IDS",        WIFI_IDS,     MOD_WIFI_IDS},
  {"Rogue AP",        ROGUE_AP,     MOD_ROGUE_AP},
  {"Wardrive",        GPS_WARDRIVE, MOD_GPS},
};
static const SubItem kCsItems[] = {
  {"Flock detect",   FLOCK,      MOD_FLOCK},
  {"Tracker detect", TRACKER,    MOD_TRACKER},
  {"Skimmer detect", SKIMMER,    MOD_SKIMMER},
  {"BLE scan",       BLE_SCAN,   MOD_BLE_SCAN},
  {"SubGHz sweep",   SUBGHZ,     MOD_SUBGHZ},
};
// Recon: the passive-analysis screens added from Marauder / Wireless
// Wizard / Flipper feature parity. Kept as their own category so the WiFi
// and Privacy lists stay at five items (the tallest a submenu can draw
// without scrolling).
static const SubItem kReconItems[] = {
  {"Probe watch",    PROBE_WATCH, MOD_PROBE_WATCH},
  {"Client map",     CLIENT_MAP,  MOD_CLIENT_MAP},
  {"Camera detect",  CAMERA_DET,  MOD_CAMERA},
  {"Drone detect",   DRONE_DET,   MOD_DRONE},
  {"BLE spam watch", BLE_SPAM,    MOD_BLE_SPAM},
};
static const int kMaxSubItems = 5;   // largest of the three submenu lists above
static Btn subButtons[kMaxSubItems];

static const SubItem *subItemsFor(Screen sub, int *count) {
  if (sub == SUB_WIFI)  { *count = sizeof(kWifiItems)  / sizeof(kWifiItems[0]);  return kWifiItems; }
  if (sub == SUB_CS)    { *count = sizeof(kCsItems)    / sizeof(kCsItems[0]);    return kCsItems; }
  if (sub == SUB_RECON) { *count = sizeof(kReconItems) / sizeof(kReconItems[0]); return kReconItems; }
  *count = 0;
  return nullptr;
}

static const char *subTitleFor(Screen sub) {
  switch (sub) {
    case SUB_WIFI: return "WiFi";
    case SUB_CS: return "Privacy";
    case SUB_RECON: return "Recon";
    default: return "";
  }
}

// A category button shows only if at least one of its items is unhidden.
static bool catHasVisibleChild(Screen sub) {
  int count;
  const SubItem *items = subItemsFor(sub, &count);
  for (int i = 0; i < count; i++)
    if (!modvisHidden(items[i].mod)) return true;
  return false;
}

static bool topItemVisible(const TopItem &it) {
  if (it.mod >= 0) return !modvisHidden(it.mod);       // standalone (Engagement)
  return catHasVisibleChild(it.target);               // category container
}

// Which screen the back button goes to from any given screen -- leaf
// screens go up to their category's submenu; submenus and the standalone
// top-level items (Engagement, Meshtastic) go to the root menu.
static Screen parentOf(Screen s) {
  switch (s) {
    case WIFI_SCAN: case NET_STATS: case WIFI_IDS: case ROGUE_AP: case GPS_WARDRIVE: return SUB_WIFI;
    case BLE_SCAN: case TRACKER: case FLOCK: case SKIMMER: case SUBGHZ: return SUB_CS;
    case PROBE_WATCH: case CLIENT_MAP: case CAMERA_DET: case DRONE_DET: case BLE_SPAM: return SUB_RECON;
    default: return MENU;   // MESHTASTIC / ENGAGEMENT / SYSTEM are top-level
  }
}

// A feature that needs an add-on radio is greyed out (not hidden) in the
// menu until that radio is marked installed in System > Hardware > SPI/IRQ
// pins. Deselecting a radio there dims every screen that depends on it.
static bool itemAvailable(Screen s) {
  switch (s) {
    case SUBGHZ: return pincfgCC1101();
    default:     return true;
  }
}

// Gear icon in the menu's bottom-left corner -- opens System settings.
// Replaces the old "System" menu button so the top level is just the
// functional items.
static const int GEAR_R = 10;
static void gearCenter(int &cx, int &cy) { cx = 18; cy = tft.height() - 18; }

static void drawGear() {
  int cx, cy; gearCenter(cx, cy);
  // Sits on top of the menu background image -- give it an opaque puck so it
  // reads against the busy scene.
  tft.fillCircle(cx, cy, GEAR_R + 6, ILI9341_BLACK);
  for (int a = 0; a < 360; a += 45) {          // teeth
    float r = a * 0.0174533f;
    int tx = cx + (int)(cosf(r) * (GEAR_R + 3));
    int ty = cy + (int)(sinf(r) * (GEAR_R + 3));
    tft.fillRect(tx - 2, ty - 2, 4, 4, ILI9341_DARKGREY);
  }
  tft.fillCircle(cx, cy, GEAR_R, ILI9341_DARKGREY);
  tft.fillCircle(cx, cy, 4, ILI9341_BLACK);    // hub hole
}

static bool touchInGear(const TouchPoint &t) {
  int cx, cy; gearCenter(cx, cy);
  int dx = t.x - cx, dy = t.y - cy;
  int rr = GEAR_R + 8;
  return dx * dx + dy * dy <= rr * rr;
}

void drawMenu() {
  uiSetBgMode(UI_BG_IMAGE);   // the dimmed scene behind the category buttons
  uiClearBelow(0);            // bg image (Vice) or flat black (Basic)
  tft.setTextColor(accentLabel());   // universal accent -- shows under Basic too now
  tft.setTextSize(3);
  tft.setCursor(8, 4);
  tft.print("WIFI D_VICE");
  tft.setTextSize(1);
  tft.setTextColor(0xA11E);   // purple -- version tag trailing the title
  tft.setCursor(tft.getCursorX() + 4, 17);
  tft.print("v0.9");
  tft.setTextSize(2);

  // One column when the display is taller than wide (0/180 orientations),
  // two when it's wider than tall (90/270). Buttons are then spread to fill
  // the space between the title and the gear row with equal gaps, rather
  // than a fixed row height that leaves dead space in one orientation.
  // Only the visible top items get laid out; hidden ones (and empty
  // categories) get a zeroed rect so the touch handler never matches them.
  int vis[kTopCount], nvis = 0;
  for (int i = 0; i < kTopCount; i++) {
    if (topItemVisible(kTop[i])) vis[nvis++] = i;
    else topButtons[i] = {0, 0, 0, 0, kTop[i].label};
  }
  const int cols = (tft.width() > tft.height()) ? 2 : 1;
  const int nrows = (nvis + cols - 1) / cols;
  const int gap = 8;
  const int top = 30;
  const int bot = tft.height() - 36;            // clear of the gear
  const int colW = (tft.width() - gap * (cols + 1)) / cols;
  const int rowH = nrows > 0 ? (bot - top - gap * (nrows - 1)) / nrows : 0;
  for (int k = 0; k < nvis; k++) {
    int i = vis[k];
    int col = k % cols, row = k / cols;
    int x = gap + col * (colW + gap);
    int y = top + row * (rowH + gap);
    topButtons[i] = {x, y, colW, rowH, kTop[i].label};
    uiDrawMenuButton(topButtons[i]);
  }
  drawGear();
}

void drawSubMenu(Screen sub) {
  uiDrawTopBar(subTitleFor(sub));
  uiSetBgMode(UI_BG_IMAGE);   // button screen -> scene background
  int count;
  const SubItem *items = subItemsFor(sub, &count);
  uiClearBelow(UI_TOPBAR_H + 1);   // bg image (Vice) or flat black (Basic)

  // Button height adapts to the item count so a 5-item list still fits
  // above the status bar in the shorter (landscape) orientation.
  int nvis = 0;
  for (int i = 0; i < count; i++) if (!modvisHidden(items[i].mod)) nvis++;
  const int top = UI_TOPBAR_H + 6;
  const int bot = tft.height() - UI_STATUSBAR_H - 4;
  const int gap = 6;
  int bh = 36;
  if (nvis > 0) {
    int fit = (bot - top - gap * (nvis - 1)) / nvis;
    if (fit < bh) bh = fit < 22 ? 22 : fit;
  }

  int y = top;
  int shown = 0;
  for (int i = 0; i < count; i++) {
    if (modvisHidden(items[i].mod)) { subButtons[i] = {0, 0, 0, 0, items[i].label}; continue; }
    subButtons[i] = {8, y, tft.width() - 16, bh, items[i].label};
    if (itemAvailable(items[i].target)) uiDrawMenuButton(subButtons[i]);
    else                                uiDrawButtonDim(subButtons[i]);
    y += bh + gap;
    shown++;
  }
  if (shown == 0) {
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(8, y + 4);
    tft.print("all hidden -- System > Modules");
  }
}

void exitScreen(Screen s) {
  switch (s) {
    case WIFI_SCAN:     wifiScanExit(); break;
    case NET_STATS:     netstatsExit(); break;
    case BLE_SCAN:      bleScanExit();  break;
    case TRACKER:       trackerExit();  break;
    case WIFI_IDS:      widsExit();     break;
    case ROGUE_AP:      rogueExit();    break;
    case FLOCK:         flockExit();    break;
    case SKIMMER:       skimmerExit();  break;
    case SUBGHZ:        subghzExit();  break;
    case PROBE_WATCH:   probeWatchExit(); break;
    case CLIENT_MAP:    clientMapExit(); break;
    case CAMERA_DET:    cameraExit();   break;
    case DRONE_DET:     droneExit();    break;
    case BLE_SPAM:      bleSpamExit();  break;
    case MESHTASTIC:    meshExit();     break;
    case GPS_WARDRIVE:  gpsExit();      break;
    case ENGAGEMENT:    engagementExit(); break;
    case SYSTEM:        systemExit();     break;
    default: break;   // submenus have no exit handler
  }
}

// Screens that switch the 2.4 GHz radio into a mode incompatible with an
// active WiFi STA link (BLE stack up, or promiscuous sniffer). Entering one
// while connected silently drops the link / any running transfer -- so ask.
static bool screenDropsWifi(Screen s) {
  switch (s) {
    case BLE_SCAN: case TRACKER: case FLOCK: case SKIMMER:
    case MESHTASTIC: case WIFI_IDS:
    case PROBE_WATCH: case CLIENT_MAP: case DRONE_DET: case BLE_SPAM:
      return true;
    default:
      return false;   // CAMERA_DET uses WiFi.scanNetworks -- no promiscuous, keeps the link
  }
}

// Returns true to proceed into `target`, false if the user backed out.
static bool confirmRadioSwitch(Screen target) {
  if (!screenDropsWifi(target) || WiFi.status() != WL_CONNECTED) return true;

  uiDrawTopBar("Switch radio?");
  uiClearBelow(29);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(6, 40);
  tft.printf("Connected to %s", WiFi.SSID().c_str());
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(6, 62); tft.print("This feature needs another radio");
  tft.setCursor(6, 76); tft.print("mode and will drop the WiFi link.");
  Btn go = {6, 108, tft.width() - 12, 40, "Continue"};
  Btn no = {6, 156, tft.width() - 12, 40, "Cancel"};
  uiDrawMenuButton(go);
  uiDrawMenuButton(no);
  for (;;) {
    TouchPoint t = uiReadTouch();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInButton(t, go)) { uiWaitForRelease(); return true; }
    if (uiTouchInButton(t, no) || uiTouchInBackArea(t)) { uiWaitForRelease(); return false; }
  }
}

void enterScreen(Screen s) {
  currentScreen = s;
  switch (s) {
    case SUB_WIFI: case SUB_CS: case SUB_RECON: drawSubMenu(s); return;
    case WIFI_SCAN:     wifiScanEnter(); break;
    case NET_STATS:     netstatsEnter(); break;
    case BLE_SCAN:      bleScanEnter();  break;
    case TRACKER:       trackerEnter();  break;
    case WIFI_IDS:      widsEnter();    break;
    case ROGUE_AP:      rogueEnter();   break;
    case FLOCK:         flockEnter();    break;
    case SKIMMER:       skimmerEnter();  break;
    case SUBGHZ:         subghzEnter();  break;
    case PROBE_WATCH:   probeWatchEnter(); break;
    case CLIENT_MAP:    clientMapEnter(); break;
    case CAMERA_DET:    cameraEnter();   break;
    case DRONE_DET:     droneEnter();    break;
    case BLE_SPAM:      bleSpamEnter();  break;
    case MESHTASTIC:    meshEnter();     break;
    case GPS_WARDRIVE:  gpsEnter();      break;
    case ENGAGEMENT:    engagementEnter(); break;
    case SYSTEM:        systemEnter();    break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== WIFI D_VICE boot ===");

  // This build is BLE-only -- never Classic BT. Release the Classic-BT
  // controller's DRAM up front (~28 KB, permanent). Must happen before any
  // BT/BLE init. Matters because Flock scans WiFi + BLE co-resident and the
  // WROOM-32's DRAM is tight.
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  Serial.printf("boot free heap %u\n", (unsigned)ESP.getFreeHeap());

  // BEFORE any WiFi call: wipe the esp_wifi NVS namespace. An earlier
  // build's SoftAP session left a config there that makes esp_wifi_start()
  // bring up hostap and panic (ieee80211_hostap_attach, null deref). This
  // project never relies on the radio remembering anything across a
  // reboot, so clearing it every boot is safe and self-healing.
  {
    nvs_handle_t h;
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
      nvs_erase_all(h);
      nvs_commit(h);
      nvs_close(h);
    }
  }
  WiFi.persistent(false);

  // Pin overrides must load before uiInit() wires the touch IRQ. Holding
  // BOOT while powering on wipes them -- the escape hatch if a bad pin
  // number left touch dead.
  pinMode(BOOT_KEY, INPUT_PULLUP);
  pincfgLoad();
  if (digitalRead(BOOT_KEY) == LOW) {
    pincfgResetAll();
    Serial.println("BOOT held: SPI pin overrides reset to defaults");
  }

  uiInit();
  themeLoad();
  accentLoad();
  tzLoad();
  modvisLoad();
  engStoreBegin();
  engagementBootUnlock();      // no-op unless the SD card marks an engagement active
  bootWifiPending = wifiAutoConnectOnBoot();   // non-blocking; link comes up during the splash
  // GPS is receive-only on its own UART (GPS_RX, see pins.h) -- safe to
  // open at boot and leave running the whole session. gpsSharedLoop() (see
  // loop() below) drains it continuously and drives the GPS time sync
  // (every 15s until the clock is set, then hourly) regardless of which
  // screen is up; the Wardrive screen and the Hardware > Test GPS page
  // both just read the same live fix instead of opening their own UART.
  gpsSharedBegin();
  showSplash(2600);           // WIFI D_VICE splash, tap to skip
  onboardingRunIfNeeded();
  drawMenu();
}

// Armed indicator: a brief 50ms blip every 3000ms (period 3050ms) rather
// than a solid light. Skipped on the detection screens that drive
// LED_STATUS themselves for alerts -- they call ledSet(false) on exit, so
// the blip resumes automatically.
static void serviceArmedLed() {
  static bool wasArmed = false;
  bool armed = engagementIsArmed();
  bool ledScreen = (currentScreen == WIFI_IDS || currentScreen == FLOCK || currentScreen == SKIMMER);
  if (!armed || ledScreen) {
    if (wasArmed && !ledScreen) ledSet(false);   // just disarmed: make sure it's off
    wasArmed = armed;
    return;
  }
  wasArmed = true;
  ledSet((millis() % 3050) < 50);
}

// Hold BOOT for 1.5s from anywhere to force recalibration -- a physical
// escape hatch that doesn't depend on touch already working, since touch
// being badly miscalibrated is exactly the situation this needs to recover
// from (e.g. right after rotating to an orientation with no calibration
// data of its own).
static uint32_t bootHeldSince = 0;
static bool bootHoldFired = false;
static const uint32_t BOOT_HOLD_MS = 1500;

static void checkBootHoldRecalibrate() {
  bool held = digitalRead(BOOT_KEY) == LOW;
  if (!held) { bootHeldSince = 0; bootHoldFired = false; return; }
  if (bootHeldSince == 0) bootHeldSince = millis();
  if (!bootHoldFired && millis() - bootHeldSince > BOOT_HOLD_MS) {
    bootHoldFired = true;
    uiRunCalibration();
    if (currentScreen == MENU) drawMenu();
    else enterScreen(currentScreen);   // redraw whatever screen was up
  }
}

void loop() {
  checkBootHoldRecalibrate();
  if (bootWifiPending && WiFi.status() == WL_CONNECTED) {
    bootWifiPending = false;
    devTimeBeginNet();     // boot auto-connect is up -- start SNTP
  }
  devTimePoll();           // promote to synced once an SNTP reply lands
  gpsSharedLoop();         // drain the GPS UART + run its own 15s/1hr time-sync schedule
  serviceArmedLed();       // 250ms/1250ms pulse while an engagement is armed

  // Blue "working" heartbeat on the continuously-scanning screens. The
  // async jobs (WiFi list scan/connect, speed test, network-info query)
  // drive ledBusy() themselves for "waiting for results"; this OR's in the
  // screens that scan the entire time they're open.
  ledBusyScreen(currentScreen == FLOCK || currentScreen == SKIMMER ||
                currentScreen == SUBGHZ || currentScreen == TRACKER ||
                currentScreen == BLE_SCAN || currentScreen == GPS_WARDRIVE ||
                currentScreen == PROBE_WATCH || currentScreen == CLIENT_MAP ||
                currentScreen == CAMERA_DET || currentScreen == DRONE_DET ||
                currentScreen == BLE_SPAM);
  uiServiceChrome();       // bottom-right clock + battery glyph + the toast ticker's
                           // next scroll step, every screen (note: doesn't
                           // appear during modal sub-loops like the
                           // keyboard, calibration, or BLE pairing wait -- those
                           // don't return to this loop() until they finish)
  TouchPoint t = uiReadTouch();

  // Back button: a quick tap steps up one level (per-screen HandleBack
  // consulted first); holding it ~600ms jumps straight to the home menu.
  // The action is deferred to release so tap vs hold can be told apart.
  static uint32_t backDownAt = 0;
  if (currentScreen != MENU) {
    bool inArea = uiTouchInBackArea(t);
    if (inArea) {
      if (backDownAt == 0) backDownAt = millis();
      if (millis() - backDownAt > 600) {          // HOLD -> home
        backDownAt = 0;
        bool isSub = (currentScreen == SUB_WIFI || currentScreen == SUB_CS || currentScreen == SUB_RECON);
        if (!isSub) exitScreen(currentScreen);
        currentScreen = MENU;
        drawMenu();
        uiWaitForRelease();
      }
      return;                                     // swallow while deciding
    }
    if (backDownAt != 0) {
      bool released = !t.pressed;
      backDownAt = 0;
      if (released) {                             // TAP -> up one level
        bool isSub = (currentScreen == SUB_WIFI || currentScreen == SUB_CS || currentScreen == SUB_RECON);
        if (isSub) {
          currentScreen = MENU;
          drawMenu();
        } else if (!((currentScreen == NET_STATS  && netstatsHandleBack()) ||
                     (currentScreen == WIFI_SCAN  && wifiScanHandleBack()) ||
                     (currentScreen == BLE_SCAN   && bleScanHandleBack()) ||
                     (currentScreen == TRACKER    && trackerHandleBack()) ||
                     (currentScreen == ROGUE_AP   && rogueHandleBack()) ||
                     (currentScreen == MESHTASTIC && meshHandleBack()))) {
          exitScreen(currentScreen);
          Screen parent = parentOf(currentScreen);
          currentScreen = parent;
          if (parent == MENU) drawMenu(); else drawSubMenu(parent);
        }
        uiWaitForRelease();
        return;
      }
      // moved off the back button while still held -> cancel, fall through
    }
  }

  if (currentScreen == MENU) {
    if (t.pressed) {
      if (t.isNewPress && touchInGear(t)) {
        enterScreen(SYSTEM);
        uiWaitForRelease();
        return;
      }
      for (int i = 0; i < kTopCount; i++) {
        if (uiTouchInButton(t, topButtons[i])) {
          if (confirmRadioSwitch(kTop[i].target)) enterScreen(kTop[i].target);
          else drawMenu();   // stayed put (Meshtastic while WiFi up, declined)
          uiWaitForRelease();   // don't let a still-held tap bleed onto the new screen
          return;
        }
      }
    }
    return;
  }

  if (currentScreen == SUB_WIFI || currentScreen == SUB_CS || currentScreen == SUB_RECON) {
    // Back is handled above (tap = to menu, hold = home). Only item taps here.
    if (t.pressed) {
      int count;
      const SubItem *items = subItemsFor(currentScreen, &count);
      for (int i = 0; i < count; i++) {
        if (uiTouchInButton(t, subButtons[i])) {
          if (!itemAvailable(items[i].target)) {
            uiToast("radio not installed -- see SPI/IRQ pins");
            uiWaitForRelease();
            return;
          }
          if (confirmRadioSwitch(items[i].target)) enterScreen(items[i].target);
          else drawSubMenu(currentScreen);   // stayed put -- repaint under the modal
          uiWaitForRelease();
          return;
        }
      }
    }
    return;
  }

  switch (currentScreen) {
    case WIFI_SCAN: {
      wifiScanLoop();
      if (t.pressed) wifiScanTouch(t);
      int jump = wifiScanTakePendingJump();   // post-connect shortcut buttons
      if (jump != WSJUMP_NONE) {
        exitScreen(WIFI_SCAN);
        enterScreen(NET_STATS);
        if (jump == WSJUMP_NETSTATS_SPEED)     netstatsGoSpeedTest();
        else if (jump == WSJUMP_NETSTATS_CONN) netstatsGoConn();
      }
      break;
    }
    case NET_STATS:     netstatsLoop(); if (t.pressed) netstatsTouch(t);  break;
    case BLE_SCAN:      bleScanLoop();   if (t.pressed) bleScanTouch(t);   break;
    case TRACKER:       trackerLoop();   if (t.pressed) trackerTouch(t);   break;
    case WIFI_IDS:
      widsLoop();
      if (t.pressed) widsTouch(t);
      if (widsTakeJumpToRogue()) { exitScreen(WIFI_IDS); enterScreen(ROGUE_AP); }
      break;
    case ROGUE_AP:      rogueLoop();    if (t.pressed) rogueTouch(t);    break;
    case FLOCK:         flockLoop();     if (t.pressed) flockTouch(t);     break;
    case SKIMMER:       skimmerLoop();   if (t.pressed) skimmerTouch(t);   break;
    case SUBGHZ:        subghzLoop();    if (t.pressed) subghzTouch(t);    break;
    case PROBE_WATCH:   probeWatchLoop(); if (t.pressed) probeWatchTouch(t); break;
    case CLIENT_MAP:    clientMapLoop();  if (t.pressed) clientMapTouch(t);  break;
    case CAMERA_DET:    cameraLoop();     if (t.pressed) cameraTouch(t);     break;
    case DRONE_DET:     droneLoop();      if (t.pressed) droneTouch(t);      break;
    case BLE_SPAM:      bleSpamLoop();    if (t.pressed) bleSpamTouch(t);    break;
    case MESHTASTIC:    meshLoop();      if (t.pressed) meshTouch(t);      break;
    case GPS_WARDRIVE:  gpsLoop();       if (t.pressed) gpsTouch(t);       break;
    case ENGAGEMENT:    engagementLoop(); if (t.pressed) engagementTouch(t); break;
    case SYSTEM:        systemLoop();    if (t.pressed) systemTouch(t);    break;
    default: break;
  }
}
