#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "driver/dac_continuous.h"
#include "pins.h"

extern Adafruit_ILI9341 tft;

struct TouchPoint {
  bool pressed;     // raw physical state -- true for the whole duration a finger is down
  bool isNewPress;  // true ONLY on the first sample of a new touch-down (rising edge);
                    // false for every subsequent sample while still held, even though
                    // `pressed` stays true. uiTouchInButton() checks this, not `pressed`,
                    // so a held touch can't re-trigger a button/screen change by itself --
                    // check `pressed` directly only where you deliberately want
                    // repeat-while-held behavior (e.g. the keyboard's backspace key).
  int x, y;   // screen-space (0..319, 0..239), rotation-1 landscape
};

struct Btn {
  int x, y, w, h;
  const char *label;
};

void uiInit();
// L-shaped 3-point tap calibration for the CURRENT rotation; persists to
// NVS per-rotation (see ui.cpp). Measures which raw touch axis is which
// rather than assuming it -- don't "optimize" this back to assuming a
// swap/invert formula, that was tried twice and was wrong both times.
void uiRunCalibration();
// Sets display rotation (0-3) directly and persists it to NVS. Triggers
// uiRunCalibration() if that rotation has no stored calibration yet.
void uiSetRotation(uint8_t r);
void uiCycleRotation();   // uiSetRotation((current + 1) % 4)
TouchPoint uiReadTouch();
// Blocks until the finger lifts (bounded by timeoutMs so a stuck touch
// driver can't hang forever). Call this after any tap that triggers a
// screen/mode transition -- a fixed delay() isn't enough, since a press
// held past the delay bleeds through as a fresh tap on whatever's newly
// drawn at the same coordinates.
void uiWaitForRelease(uint32_t timeoutMs = 2000);
// Theme background. Vice = vertical indigo->magenta gradient, Basic = flat
// black. Screens clear their area with these instead of fillRect(BLACK) so
// the gradient shows everywhere.
uint16_t uiBgColor(int y);
void uiClearRect(int x, int y, int w, int h);
void uiClearBelow(int y0);   // (0, y0) .. bottom-right

// Content-area background for menu / button screens: the dimmed Vice scene
// image instead of the flat gradient. UI_BG_BLACK (flat black / gradient) is
// the default and is restored by every uiDrawTopBar(); a button screen opts
// in by calling uiSetBgMode(UI_BG_IMAGE) right after its top bar (or before
// its first uiClearBelow, for the home menu which has no top bar). List and
// live-data screens leave it alone -> black. No effect in the Basic theme.
enum { UI_BG_BLACK = 0, UI_BG_IMAGE = 1 };
void uiSetBgMode(int mode);

// Persistent bottom chrome: a solid black strip UI_STATUSBAR_H tall with a
// 1px white rule along its top edge, holding the toast line, the clock and
// the battery glyph. Painted by uiClearBelow() and uiDrawTopBar().
static const int UI_STATUSBAR_H = 18;
// Width reserved on the right of the status bar for the clock + battery
// glyph (uiDrawClock, uiDrawBatteryIndicator) -- uiToast()'s text stops
// here so it doesn't run under them.
static const int UI_RIGHTZONE_W = 96;
void uiDrawStatusBar();

void uiDrawTopBar(const char *title);
bool uiTouchInBackButton(const TouchPoint &t);
bool uiTouchInBackArea(const TouchPoint &t);   // area only, for a held back button
// Global cap on button label text size. uiDrawButton() starts here and
// shrinks to fit, so every button in the app is at most this size -- one
// knob if you want them bigger/smaller. At 2 a ~15-char label fits a
// full-width (>=180px) button (15 * 6px * 2 = 180px).
static const uint8_t UI_MENU_BTN_MAXSIZE = 2;
void uiDrawButton(const Btn &b);
// Alias for uiDrawButton() -- kept for call sites that name it explicitly.
void uiDrawMenuButton(const Btn &b);
// Greyed-out / disabled button: same footprint and label-fit as
// uiDrawButton(), muted fill + grey text. For a menu row whose feature is
// unavailable (for example a radio that is not marked installed).
void uiDrawButtonDim(const Btn &b);
bool uiTouchInButton(const TouchPoint &t, const Btn &b);

// UI rule: the top-bar row is back+title ONLY, always -- never squeeze
// extra per-screen buttons into it. That's caused real bugs (a button
// drawn there survived a mode switch that only redrew content below the
// top bar; text collided with the back button). Any screen needing extra
// buttons (a tab, a mode toggle, a Track button, etc.) puts them in the
// action row via uiDrawActionRow() instead, and starts its own content at
// UI_CONTENT_Y (not the plain 29px case) to leave room for it.
static const int UI_TOPBAR_H = 28;
static const int UI_ACTIONROW_Y = UI_TOPBAR_H + 1;
static const int UI_ACTIONROW_H = 26;
static const int UI_CONTENT_Y_PLAIN = UI_TOPBAR_H + 1;                     // no action row
static const int UI_CONTENT_Y = UI_ACTIONROW_Y + UI_ACTIONROW_H + 1;       // with an action row
// Lays out `count` equal-width buttons filling one row directly below the
// top bar and draws them; set each btns[i].label before calling (x/y/w/h
// are computed here and overwritten). Returns nothing -- read the same
// array back for hit-testing (uiTouchInButton against btns[i]).
void uiDrawActionRow(Btn *btns, int count);
// The project's ONE paging control -- every paged list/grid screen uses
// this, not its own prev/next buttons or a "tap the empty space" gesture.
// Draws "< prev   N / M   next >" in one row at (x=8..width-8, y, h);
// prevBtn/nextBtn come back dimmed (via uiDrawButtonDim()) at either end
// when there's no previous/next page. The caller still does its own
// touch handling on the two Btn outs, same shape every time:
//   if (uiTouchInButton(t, prevBtn) && page > 0)         { ...; page--; }
//   if (uiTouchInButton(t, nextBtn) && page < pages - 1) { ...; page++; }
// Always draws (even at page 1/1, both ends dimmed) rather than hiding
// itself for a single page -- keeps a paged screen's layout constant
// instead of jumping around depending on item count.
static const int UI_PAGER_H = 26;
void uiDrawPager(int y, int page, int pages, Btn &prevBtn, Btn &nextBtn);
// Pull-down-menu picker: opens a full list of `count` text options
// (itemLabel(i) supplies each one) below the top bar, paged if it doesn't
// fit; tapping a row selects it and returns immediately (no separate
// Apply step -- that's the caller's business if it wants one). Back
// cancels and returns `current` unchanged. Services the clock/battery
// corner itself each frame (see uiServiceChrome()) so it's safe to call
// from any screen without that corner going dark for as long as the list
// is open. For a plain text list -- something that wants to preview a
// COLOR per row (a swatch) needs its own picker; see
// system_screen.cpp's Accent Color picker for that shape.
int uiDropdownPick(const char *title, int count, const char *(*itemLabel)(int), int current);
void uiToast(const char *msg);   // one-line status text at the bottom of the screen (leaves
                                 // room on the right for uiDrawClock(), see below) -- character-
                                 // steps a ticker (see uiServiceChrome()) if it's too long to fit
// Stops the ticker and blanks the toast area. MUST be called on every
// screen exit -- see exitScreen() in the .ino, the one place this is
// wired in -- or a message from a screen you've left keeps redrawing
// itself indefinitely (uiTickToast() runs off the main loop(), not tied
// to any particular screen).
void uiClearToast();
// Full-screen modal numeric keypad -- a stripped-down cousin of
// uiTextInput() (keyboard.cpp) with no letters/layers: a 3x4 grid of big
// digit keys plus '.', backspace, Cancel and OK, sized for fat-finger taps
// on the resistive panel. For IP-address / port entry (Net stats > LAN
// speed). Returns the typed string, or `initial` unchanged on Cancel / a
// tap in the top-left back-button area -- same cancel contract as
// uiTextInput(). Validation is loose (one '.' max, 20 chars); the caller
// validates the actual value.
String uiNumpadInput(const char *prompt, const String &initial = "");
// Small bottom-right clock, drawn on every screen from the main loop
// (independent of whatever screen is active). Shows "HH:MM" local time
// (devtime.h's UTC clock shifted by the tz.h offset) once devtime.h has a
// synced clock; shows "--:--" (dimmed) before that -- no separate
// "unsynced" indicator, the dashes ARE the indicator.
void uiDrawClock();
// Clears the content area (below the top bar) and shows a one-line yellow
// status message. Call this before any blocking radio/SD init or scan a
// screen's Enter() does, so there's visible feedback instead of a stale or
// blank screen during the wait.
void uiShowLoading(const char *msg);
// Draws `text` (default font) rotated in 90-degree steps (0-3, clockwise),
// centered on (cx, cy). Used for the rotation-picker submenu so each
// button's label previews the orientation it would apply. Renders to a
// small offscreen GFXcanvas1 first, then blits it rotated pixel-by-pixel --
// Adafruit_GFX has no built-in rotated text, and we only need the four
// cardinal angles, not arbitrary rotation.
void uiDrawRotatedText(int cx, int cy, const char *text, uint8_t rotSteps, uint8_t textSize, uint16_t color);

void ledSet(bool on);      // red channel
void ledGreen(bool on);    // green channel (wardrive new-contact blip, etc.)
// Blue "working" heartbeat (50ms on / 25ms off) for the duration of a
// scan / speed test / other job. Independent of the red armed blinker.
// ledBusy() = explicit job callers; ledBusyScreen() = loop()'s per-frame
// "on a scanning screen" signal (OR'd). ledBusyAlt() alternates the
// flash blue/green (Skimmer).
void ledBusy(bool on);
void ledBusyScreen(bool on);
void ledBusyAlt(bool on);
// 3 fast chirps + green blinks -- fire once when a detector picks up a new
// target (Flock / Skimmer). Blocking (~0.25s).
void alertDetected();
// Volume scales the LEDC duty cycle (this hardware has no separate analog
// volume control, just a PWM square wave into the amp). 0 = silent -- beep()
// still blocks for `ms` at volume 0 so callers that rely on it for timing
// (e.g. the WiFi locate screen's beep-rate-as-signal-strength) keep working.
int uiGetBeepVolume();        // 0-100
void uiSetBeepVolume(int v);  // persists to NVS
void beep(uint32_t ms, uint32_t toneHz);
// Keep the amp + DAC powered across many back-to-back beeps (e.g. the
// range-finder chirps). Without this, every short chirp restarts from the
// amp's ~20ms unmute ramp and is mostly inaudible. Call beepHold(true) on
// entering a screen that chirps repeatedly, beepHold(false) on leaving.
void beepHold(bool on);
// The shared GPIO26 DAC channel (22050 Hz) -- also used by the easter egg's
// MOD player so it never has to allocate/free its own. May be null if the
// DAC driver failed to init.
dac_continuous_handle_t uiDac();
int uiDacRate();
// Tear down and recreate the shared DAC channel. Call after something that
// drives the DAC from its own task (the splash MOD player) exits -- a DMA
// underrun there can leave the channel wedged so later beeps are silent.
void uiAudioReset();
// Range-finder chirp: beep RATE and PITCH both scale with `rssi` as a
// closeness proxy (far = slow + low, close = fast + high, accelerating
// hard near the target). Self-rate-limited by an internal timer -- call it
// every loop iteration while a target is in range and it no-ops between
// chirps. Honors the beep-volume setting; a wrapper around beep().
void rangeBeep(int rssi);

// Onboard LiPo, read from GPIO34 through the CYD's ~2:1 divider (calibrated
// ADC, 16-sample average), then scaled by the user cal factor. No
// charge-status line is broken out to a GPIO on this board -- only voltage.
int uiBatteryMv();
// Rough 1S state-of-charge from resting voltage; -1 = no cell (>4.3V charger
// rail, or <2.8V). Sags low under a heavy scan.
int uiBatteryPct();

// --- battery calibration (System > Hardware > Battery calibrate) ---
// The board's actual resistor divider + the eFuse ADC cal vary unit to
// unit, so uiBatteryMv() = raw * factor. Factor persists in NVS ("batcal").
void  uiBatteryCalLoad();            // call once from uiInit()
int   uiBatteryRawMv();              // before the cal factor
float uiBatteryCal();               // current factor (default 1.0)
void  uiBatterySetCal(float k);     // clamped 0.5..2.0, persisted
void  uiBatterySetCalFromActual(int actualMv);   // factor = actualMv / rawMv
// Battery glyph + % in the bottom-right corner (left of it: the clock,
// uiDrawClock()). Call every loop() iteration (self-throttled); pulses
// colours when below 15%. uiDrawTopBar() forces a repaint on a screen
// change.
void uiDrawBatteryIndicator();
// The clock, the battery glyph, and the toast ticker's next scroll step
// (see uiToast()) -- one call. main loop() calls this every iteration
// (see the .ino); any OTHER blocking touch-poll loop (a settings
// sub-page, a modal wait) needs to call this itself once per iteration,
// or all three just go dark/stop scrolling for as long as that loop owns
// the CPU -- they're normally only alive because loop() keeps running.
void uiServiceChrome();
