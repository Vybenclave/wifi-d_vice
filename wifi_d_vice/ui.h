#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <functional>
#include "driver/dac_continuous.h"
#include "pins.h"

extern Adafruit_ILI9341 tft;

struct TouchPoint {
  bool pressed;     // True while the finger touches the screen.
  bool isNewPress;  // True only on the first touch sample.
                    // False while the finger stays down.
                    // uiTouchInButton() checks this flag.
                    // A held touch cannot re-trigger a button.
                    // Check `pressed` only for repeat behavior.
  int x, y;   // Screen coordinates in landscape mode.
};

struct Btn {
  int x, y, w, h;
  const char *label;
};

void uiInit();
// Calibrate touch for the current rotation.
// Store results in NVS per rotation.
// Measure raw touch axes directly.
// Do not assume a swap or invert formula.
void uiRunCalibration();
// Set display rotation and store it in NVS.
// Run calibration if no data exists for this rotation.
void uiSetRotation(uint8_t r);
void uiCycleRotation();
TouchPoint uiReadTouch();
// Wait for the finger to lift.
// Use timeoutMs to prevent driver hangs.
// Call this after taps that change screens.
// A fixed delay fails if the press lasts longer.
void uiWaitForRelease(uint32_t timeoutMs = 2000);
// Return background color for a given Y coordinate.
// Vice uses a vertical gradient.
// Basic uses flat black.
// Use this function to clear screen areas.
uint16_t uiBgColor(int y);
void uiClearRect(int x, int y, int w, int h);
void uiClearBelow(int y0);   // Clear the rectangle from (0, y0) to the bottom right.
// Upscale a half-resolution image by 2x.
// Blend texels to avoid blocky pixels.
// Reuse this function for full-screen images.
// `img` points to the source data.
// `xx` and `yy` set the target position.
uint16_t uiBgSample(const uint16_t *img, int sw, int sh, int xx, int yy);

// --- flicker-free value / list redraw ------------------------------------
// Redraw only changed screen regions.
// Compare new text against the previous buffer.
// This handles shrinking and growing regions.
// It works with the Vice gradient background.

// Compare new text with the previous buffer.
// Copy the new text into the buffer.
// Return true if the text changed.
bool uiFieldChanged(char *prevBuf, size_t prevBufSz, const char *newText);

// Format the string and check for changes.
// Erase the old area and print the new text.
// Return true if the screen updated.
bool uiDrawFieldIfChanged(int x, int y, int w, int h, uint16_t fg, uint8_t sz,
                          char *prevBuf, size_t prevBufSz, const char *fmt, ...);

// Maximum signature length for list rows.
// Use this size for change detection only.
static const int UI_LIST_SIG_LEN = 64;

// Redraw only changed rows in a fixed list.
// Build a signature for each row.
// Erase and redraw rows that changed.
// Clear empty slots at the end of the list.
void uiDrawListIfChanged(int x, int rowY0, int rowW, int rowH, int count, int maxRows,
                         char prevBufs[][UI_LIST_SIG_LEN],
                         const std::function<void(int i, char *sigOut, size_t sigCap)> &rowSignature,
                         const std::function<void(int i)> &drawRow);

// Set the content background mode.
// The default is UI_BG_BLACK.
// uiDrawTopBar() resets this mode.
// List screens use UI_BG_IMAGE by default.
// Button screens must call this function.
// This setting has no effect in the Basic theme.
enum { UI_BG_BLACK = 0, UI_BG_IMAGE = 1 };
void uiSetBgMode(int mode);

// Check if list screens use the scene image.
// Store this setting in NVS.
// The Vice theme enables this by default.
bool uiListBgEnabled();
void uiSetListBgEnabled(bool on);

// Status bar height in pixels.
// Draw a black strip with a white top edge.
// This function paints the status bar.
static const int UI_STATUSBAR_H = 18;
// Right zone width for the clock and battery.
// Toast text stops at this boundary.
static const int UI_RIGHTZONE_W = 96;
void uiDrawStatusBar();

void uiDrawTopBar(const char *title);
bool uiTouchInBackButton(const TouchPoint &t);
bool uiTouchInBackArea(const TouchPoint &t);   // Check touch position for a held back button.
// Maximum button label text size.
// uiDrawButton() shrinks text to fit.
// Change this value to resize all buttons.
static const uint8_t UI_MENU_BTN_MAXSIZE = 2;
void uiDrawButton(const Btn &b);
// Call uiDrawButton() with a different name.
void uiDrawMenuButton(const Btn &b);
// Draw a disabled button.
// Use muted colors and grey text.
// Show this for unavailable features.
void uiDrawButtonDim(const Btn &b);
// Draw a button with a custom fill color.
// Use this for distinct button states.
// Calculate text contrast against the fill color.
void uiDrawButtonColored(const Btn &b, uint16_t fill);
// Draw a button with independent colors.
// Set fill, edge, and text colors separately.
// Use this for three-state indicators.
void uiDrawButtonTricolor(const Btn &b, uint16_t fill, uint16_t edge, uint16_t text);
// Return black or white for maximum contrast.
uint16_t uiContrastText(uint16_t fill);
bool uiTouchInButton(const TouchPoint &t, const Btn &b);

// Reserve the top bar for the back button and title only.
// Do not add extra buttons to this row.
// Place additional buttons in the action row.
// Start screen content at UI_CONTENT_Y.
static const int UI_TOPBAR_H = 28;
static const int UI_ACTIONROW_Y = UI_TOPBAR_H + 1;
static const int UI_ACTIONROW_H = 26;
static const int UI_CONTENT_Y_PLAIN = UI_TOPBAR_H + 1;                     // Content start Y without an action row.
static const int UI_CONTENT_Y = UI_ACTIONROW_Y + UI_ACTIONROW_H + 1;       // Content start Y with an action row.
// Draw a row of equal-width buttons.
// Set button labels before calling.
// This function computes button positions.
// Use the same array for hit testing.
void uiDrawActionRow(Btn *btns, int count);
// Draw a standard paging control.
// Every paged screen must use this function.
// Dim the previous button on the first page.
// Dim the next button on the last page.
// Always draw the control to keep layout stable.
static const int UI_PAGER_H = 26;
void uiDrawPager(int y, int page, int pages, Btn &prevBtn, Btn &nextBtn);
// Calculate page geometry for a list or grid.
// Count rows that fit between y0 and the pager.
// Reserve space for the pager and bottom margin.
// Cap the row count to hardCap.
// Multiply rows by cols for grid layouts.
// Do not clamp row capacity to the item count.
// This keeps the pager position stable.
// Return the number of rows per page.
int uiPagerLayout(int y0, int rowH, int gap, int bottomMargin, int pagerGap,
                  int count, int hardCap, int cols,
                  int &itemsPerPage, int &pages, int &pagerY);
// Open a pull-down menu picker.
// Display a list of text options.
// Page the list if it does not fit.
// Return immediately when a row is tapped.
// Pressing Back cancels the selection.
// This function updates the clock and battery.
// Use a custom picker for color swatches.
int uiDropdownPick(const char *title, int count, const char *(*itemLabel)(int), int current);
void uiToast(const char *msg);   // Show a one-line status message.
                                 // Place the text at the bottom of the screen.
                                 // Leave space for the clock on the right.
                                 // Scroll the text if it exceeds the width.
// Clear the toast message.
// Call this function on every screen exit.
// This prevents old messages from redrawing.
void uiClearToast();
// Open a full-screen numeric keypad.
// Display a 3x4 grid of digit keys.
// Use this for IP addresses and port numbers.
// Return the typed string or the initial value.
// Perform strict validation in the caller.
String uiNumpadInput(const char *prompt, const String &initial = "");
// Draw the clock in the bottom-right corner.
// Update this clock from the main loop.
// Show local time when the clock syncs.
// Show dimmed dashes before synchronization.
void uiDrawClock();
// Clear the content area and show a loading message.
// Call this before blocking operations.
// This provides visible feedback during waits.
void uiShowLoading(const char *msg);
// Draw rotated text at a center point.
// Rotate text in 90-degree steps.
// Render to an offscreen canvas first.
// This avoids the library's lack of rotation support.
void uiDrawRotatedText(int cx, int cy, const char *text, uint8_t rotSteps, uint8_t textSize, uint16_t color);

// Draw the RSSI value and signal bar.
// Show a dimmed message when the signal is lost.
// Set the bar range with rssiMin and rssiMax.
// Color the bar green, yellow, or red.
// Erase the area only when the value changes.
// Return true if the screen updated.
bool uiDrawLocateReading(int x, int y, int w, uint16_t presentColor,
                         int rssiMin, int rssiMax, int *prevRssi, int rssi);

// Define severity levels for anomaly screens.
// Use these values for color and tag functions.
// Local enums may use different names.
// Match the numeric values exactly.
enum { UI_SEV_OK = 0, UI_SEV_WATCH = 1, UI_SEV_ALERT = 2 };
uint16_t uiSevColor(uint8_t sev);     // Return the color for a severity level.
const char *uiSevTag(uint8_t sev);    // Return the tag string for a severity level.

void ledSet(bool on);      // Control the red LED channel.
void ledGreen(bool on);    // Control the green LED channel.
void ledGreenPwm(uint8_t brightness);   // Set green LED brightness with PWM.
// All three LED channels render in one background task.
// This prevents independent timers from fighting.
// Alert has the highest priority.
// Busy has the middle priority.
// Heartbeat has the lowest priority.
void ledColorRGB(uint8_t r, uint8_t g, uint8_t b);     // Write directly to all three channels.
void ledColorAccent(float intensity);                  // Apply accent color with intensity.
// Show a working heartbeat during active jobs.
// Use ledBusy() for explicit jobs.
// Use ledBusyScreen() for scanning screens.
void ledBusy(bool on);
void ledBusyScreen(bool on);
// Show an ambient heartbeat while idle.
// Use plain red instead of the accent color.
// Feed the on/off state to the shared render task.
void ledHeartbeat(bool on);
// Show an alert with alternating red and yellow.
// This overrides busy and heartbeat while active.
void ledAlert(bool on);
// Show a solid accent color while a 2FA bond connects.
void ledConnected(bool on);
// Pause the shared render task entirely.
// Use this for direct LED animations.
// Always pair true with a later false.
void ledSuspendRender(bool suspend);
// Play three fast chirps and green blinks.
// Fire this when a detector finds a new target.
void alertDetected();
// Volume scales the LEDC duty cycle.
// Zero volume produces silence.
// beep() still blocks for the requested duration.
int uiGetBeepVolume();        // Return volume from 0 to 100.
void uiSetBeepVolume(int v);  // Persist volume to NVS.
void beep(uint32_t ms, uint32_t toneHz);
// Keep the amplifier powered between short beeps.
// This prevents the unmute ramp from cutting the sound.
// Call beepHold(true) before repeated chirps.
// Call beepHold(false) after the chirps finish.
void beepHold(bool on);
// Return the shared DAC channel handle.
// This channel also serves the MOD player.
// Return null if the driver failed to initialize.
dac_continuous_handle_t uiDac();
int uiDacRate();
// Reinitialize the shared DAC channel.
// Call this after the MOD player exits.
// This clears DMA underruns that silence beeps.
void uiAudioReset();
// Play a range-finder chirp.
// Scale rate and pitch with rssi.
// This function limits its own call rate.
void rangeBeep(int rssi);

// Read LiPo voltage from GPIO34.
// Apply the calibrated ADC factor.
int uiBatteryMv();
// Estimate state of charge from resting voltage.
// Return -1 if the cell is missing or overcharged.
int uiBatteryPct();

// --- battery calibration (System > Hardware > Battery calibrate) ---
// Apply the calibration factor to raw voltage.
// Store the factor in NVS.
void  uiBatteryCalLoad();            // Load calibration from NVS.
int   uiBatteryRawMv();              // Return voltage before calibration.
float uiBatteryCal();               // Return the current calibration factor.
void  uiBatterySetCal(float k);     // Set and persist the calibration factor.
void  uiBatterySetCalFromActual(int actualMv);   // Calculate factor from actual voltage.
// Draw the battery glyph and percentage.
// Update this indicator every loop iteration.
// Pulse colors when the charge drops below 15%.
void uiDrawBatteryIndicator();
// Update the clock, battery, and toast ticker.
// Call this function every loop iteration.
// Other blocking loops must call this function too.
void uiServiceChrome();
