#include "flagfinding.h"
#include "ui.h"
#include "keyboard.h"
#include "accent.h"
#include "engagement.h"
#include "engstore.h"
#include "gps_shared.h"
#include "devtime.h"
#include "power.h"

static const char *kCannedTags[] = {
  "Rogue/evil-twin",
  "Weak encryption",
  "Default creds",
  "Deauth/jamming",
  "Unauthorized BLE",
  "Physical security",
  "Other (see note)",
};
static const int kCannedTagCount = sizeof(kCannedTags) / sizeof(kCannedTags[0]);

void flagDetectionShow(const char *source, uint8_t severity, const String &location) {
  if (!engagementIsArmed()) {
    uiDrawTopBar("Flag finding");
    uiClearBelow(0);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(8, tft.height() / 2 - 24);
    tft.print("Not armed");
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(8, tft.height() / 2 + 4);
    tft.print("Arm the engagement first");
    tft.setCursor(8, tft.height() / 2 + 20);
    tft.print("(Engagement screen -> ARM).");
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(8, tft.height() - UI_STATUSBAR_H - 20);
    tft.print("tap to continue");
    for (;;) {
      TouchPoint t = uiReadTouch();
      if (t.pressed) powerNoteActivity();
      powerServiceAutoOff();
      if (t.pressed) { uiWaitForRelease(); break; }
      delay(15);
    }
    return;
  }

  uiDrawTopBar("Flag finding");
  uiClearBelow(29);
  tft.setTextSize(1);
  tft.setTextColor(accentLabel());
  tft.setCursor(6, 33);
  tft.print("What kind of issue is this?");

  Btn rows[kCannedTagCount];
  const int ROW_H = 24, ROW_GAP = 3;
  int y = 46;
  for (int i = 0; i < kCannedTagCount; i++) {
    rows[i] = {4, y, tft.width() - 8, ROW_H, kCannedTags[i]};
    uiDrawMenuButton(rows[i]);
    y += ROW_H + ROW_GAP;
  }

  int picked = -1;
  while (picked < 0) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    for (int i = 0; i < kCannedTagCount; i++) {
      if (uiTouchInButton(t, rows[i])) { uiWaitForRelease(); picked = i; break; }
    }
  }

  String description = kCannedTags[picked];

  // Resolve GPS once before the summary loop. Show the location on the review screen and the final confirmation.
  String shownLoc = location;
  if (shownLoc.length() == 0 && gpsShared().location.isValid()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6f,%.6f", gpsShared().location.lat(), gpsShared().location.lng());
    shownLoc = buf;
  }

  // Show the exact data before writing. Loop until the user confirms. Do not write to SD until the user taps Flag it.
  String note = "";
  bool confirmed = false;
  while (!confirmed) {
    uiDrawTopBar("Flag summary");
    uiClearBelow(0);
    tft.setTextColor(accentLabel());
    tft.setTextSize(2);
    tft.setCursor(8, 34);
    tft.print("Review before flagging");
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(8, 66);
    tft.printf("Tag: %s", description.c_str());
    tft.setTextColor(uiSevColor(severity));
    tft.setCursor(8, 82);
    tft.printf("Severity: %s", uiSevTag(severity));
    tft.setTextColor(shownLoc.length() ? ILI9341_WHITE : ILI9341_YELLOW);
    tft.setCursor(8, 98);
    tft.printf("GPS: %s", shownLoc.length() ? shownLoc.c_str() : "no fix");
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(8, 114);
    tft.printf("Note: %s", note.length() ? note.c_str() : "(none)");

    Btn noteBtn = {8, 150, tft.width() - 16, 34, note.length() ? "Edit note" : "Add note"};
    Btn okBtn   = {8, 192, tft.width() - 16, 34, "Flag it"};
    uiDrawMenuButton(noteBtn);
    uiDrawMenuButton(okBtn);

    for (;;) {
      TouchPoint t = uiReadTouch();
      if (t.pressed) powerNoteActivity();
      powerServiceAutoOff();
      if (!t.pressed) { delay(15); continue; }
      if (uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
      if (uiTouchInButton(t, noteBtn)) {
        uiWaitForRelease();
        note = uiTextInput("Note (optional)", note, false);
        break;   // redraw the summary with the updated note
      }
      if (uiTouchInButton(t, okBtn)) { uiWaitForRelease(); confirmed = true; break; }
    }
  }

  String jobId     = engStoreJobOpen() ? String(engStoreJobId()) : "manual";

  // Capture the timestamp before the function call. engStoreFindingOpen() stamps openedAt internally. This avoids a microsecond delay.
  String openedAt = devTimeNowString();
  int id = engStoreFindingOpen(jobId, "", source, severity, description, location);
  if (id < 0) { uiToast("flag failed (SD?)"); return; }
  if (note.length()) engStoreFindingUpdate(id, "", "", "", note);

  uiClearBelow(0);
  tft.setTextColor(ILI9341_GREEN);
  tft.setTextSize(2);
  tft.setCursor(8, 34);
  tft.printf("Finding #%d", id);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(8, 66);
  tft.printf("Time: %s", openedAt.c_str());
  tft.setCursor(8, 82);
  tft.printf("Tag: %s", description.c_str());
  tft.setTextColor(uiSevColor(severity));
  tft.setCursor(8, 98);
  tft.printf("Severity: %s", uiSevTag(severity));
  tft.setTextColor(shownLoc.length() ? ILI9341_WHITE : ILI9341_YELLOW);
  tft.setCursor(8, 114);
  tft.printf("GPS: %s", shownLoc.length() ? shownLoc.c_str() : "no fix");
  if (note.length()) {
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(8, 130);
    tft.printf("Note: %s", note.c_str());
  }
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(8, tft.height() - UI_STATUSBAR_H - 20);
  tft.print("tap to continue");

  for (;;) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed) { uiWaitForRelease(); break; }
    delay(15);
  }
}

void showDetectionDetail(const char *headline, uint8_t severity, const char *timeStr,
                          const uint8_t *mac, const char *ssid,
                          bool hasGps, float lat, float lon, const char *source) {
  uiDrawTopBar("Detail");
  uiClearBelow(29);
  tft.setTextSize(1);
  tft.setTextColor(uiSevColor(severity));
  tft.setCursor(8, 34);
  tft.print(headline);

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(8, 58);
  tft.printf("Time: %s", timeStr);

  tft.setCursor(8, 74);
  if (mac) tft.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  else     tft.print("MAC: N/A");

  tft.setCursor(8, 90);
  tft.printf("SSID: %s", (ssid && ssid[0]) ? ssid : "N/A");

  tft.setCursor(8, 106);
  if (hasGps) tft.printf("GPS: %.6f,%.6f", lat, lon);
  else        tft.print("GPS: N/A");

  Btn flagBtn = {8, tft.height() - UI_STATUSBAR_H - 44, tft.width() - 16, 34, "Flag this"};
  uiDrawMenuButton(flagBtn);

  for (;;) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (!t.pressed) { delay(15); continue; }
    if (uiTouchInBackButton(t)) { uiWaitForRelease(); return; }
    if (uiTouchInButton(t, flagBtn)) {
      uiWaitForRelease();
      String loc = hasGps ? (String(lat, 6) + "," + String(lon, 6)) : String("");
      flagDetectionShow(source, severity, loc);
      return;
    }
  }
}
