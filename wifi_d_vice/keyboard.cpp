#include "keyboard.h"
#include "ui.h"
#include "accent.h"

struct KeyRect { int x, y, w, h; char c; };
static KeyRect keys[80];
static int keyCount = 0;

// Special key codes -- values that never appear in a typed passphrase, so
// they can share the `char c` slot with printable keys.
static const char K_SPACE  = ' ';
static const char K_BKSP   = '\b';
static const char K_CANCEL = 27;
static const char K_DONE   = '\n';
static const char K_SHIFT  = 1;    // toggle letter case (letters layer only)
static const char K_LAYER  = 2;    // toggle letters <-> symbols

// Two layers + a case toggle -- Wi-Fi passphrases need lowercase and
// punctuation, which the original A-Z/0-9-only grid couldn't produce.
// `shift` is sticky (caps-lock style, with the key highlighted while on)
// rather than one-shot: fewer taps on a laggy resistive panel, and the
// highlight makes the state obvious.
static bool symbols = false;
static bool shift = false;

static const char *lettersRows[4] = {
  "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"
};
static const char *symbolRows[4] = {
  "1234567890", "!@#$%^&*()", "-_=+[]{}<>", ";:'\"\\|,./?"
};

static void layoutKeys() {
  keyCount = 0;
  const char **rows = symbols ? symbolRows : lettersRows;
  int y = 56;
  for (int r = 0; r < 4; r++) {
    int n = strlen(rows[r]);
    int w = (tft.width() - 4) / n;
    int x = 2 + (tft.width() - 4 - w * n) / 2;
    for (int i = 0; i < n; i++) keys[keyCount++] = {x + i * w, y, w - 2, 26, rows[r][i]};
    y += 30;
  }
  // function row: layer toggle, shift, space, backspace, cancel, done
  static const char frow[6] = {K_LAYER, K_SHIFT, K_SPACE, K_BKSP, K_CANCEL, K_DONE};
  int w = (tft.width() - 4) / 6;
  for (int i = 0; i < 6; i++) keys[keyCount++] = {2 + i * w, y, w - 2, 28, frow[i]};
}

static const char *labelFor(char c) {
  static char buf[2] = {0, 0};
  switch (c) {
    case K_SPACE:  return "SPACE";
    case K_BKSP:   return "BKSP";
    case K_CANCEL: return "CANCEL";
    case K_DONE:   return "DONE";
    case K_SHIFT:  return shift ? "Aa" : "aA";
    case K_LAYER:  return symbols ? "ABC" : "123";
  }
  buf[0] = (!symbols && shift && c >= 'a' && c <= 'z') ? (c - 32) : c;
  return buf;
}

static void drawKeys() {
  uiClearRect(0, 52, tft.width(), tft.height() - 52);
  for (int i = 0; i < keyCount; i++) {
    bool hot = (keys[i].c == K_SHIFT && shift && !symbols) ||
               (keys[i].c == K_LAYER && symbols);
    if (hot) tft.fillRect(keys[i].x, keys[i].y, keys[i].w, keys[i].h, ILI9341_DARKGREY);
    tft.drawRect(keys[i].x, keys[i].y, keys[i].w, keys[i].h, ILI9341_WHITE);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    const char *lbl = labelFor(keys[i].c);
    int16_t bx, by; uint16_t bw, bh;
    tft.getTextBounds(lbl, 0, 0, &bx, &by, &bw, &bh);
    tft.setCursor(keys[i].x + (keys[i].w - bw) / 2, keys[i].y + (keys[i].h - bh) / 2);
    tft.print(lbl);
  }
}

String uiTextInput(const char *prompt, const String &initial, bool mask) {
  symbols = false;
  shift = false;
  layoutKeys();
  String buf = initial;
  bool done = false, cancelled = false;
  bool revealed = false;   // masked fields start hidden; SHOW toggles this

  // "show password" checkbox (masked fields only) -- box + label, tapping
  // anywhere on it toggles `revealed`.
  Btn showBtn = {tft.width() - 76, 16, 74, 22, "show"};

  uiClearBelow(0);
  tft.setTextColor(accentLabel());
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.print(prompt);
  drawKeys();

  auto redrawField = [&]() {
    int fieldW = tft.width() - 4 - (mask ? (showBtn.w + 4) : 0);
    tft.fillRect(2, 16, tft.width() - 4, 24, ILI9341_BLACK);
    tft.drawRect(2, 16, fieldW, 24, ILI9341_WHITE);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 19);
    if (mask && !revealed) { for (uint32_t i = 0; i < buf.length(); i++) tft.print('*'); }
    else tft.print(buf);
    if (mask) {
      const int s = 16;
      int cbx = showBtn.x, cby = showBtn.y + 2;
      tft.fillRect(cbx, cby, s, s, ILI9341_BLACK);
      tft.drawRect(cbx, cby, s, s, ILI9341_WHITE);
      if (revealed) {
        tft.drawLine(cbx + 3, cby + 8, cbx + 6, cby + 12, ILI9341_GREEN);
        tft.drawLine(cbx + 6, cby + 12, cbx + 13, cby + 3, ILI9341_GREEN);
      }
      tft.setTextSize(1);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(cbx + s + 4, cby + 5);
      tft.print("show");
    }
  };
  redrawField();

  while (!done && !cancelled) {
    TouchPoint t = uiReadTouch();
    if (!t.pressed) { delay(15); continue; }
    if (mask && uiTouchInButton(t, showBtn)) {
      revealed = !revealed;
      redrawField();
      uiWaitForRelease();
      continue;
    }
    for (int i = 0; i < keyCount; i++) {
      if (t.x >= keys[i].x && t.x < keys[i].x + keys[i].w &&
          t.y >= keys[i].y && t.y < keys[i].y + keys[i].h) {
        char c = keys[i].c;
        if (c == K_BKSP) {
          // The one deliberate exception to single-fire-per-touch (see
          // TouchPoint's comment in ui.h): backspace repeats while held,
          // rate-limited so it doesn't delete faster than ~1 char/150ms.
          static uint32_t lastRepeat = 0;
          if (t.isNewPress || millis() - lastRepeat > 150) {
            lastRepeat = millis();
            if (buf.length() > 0) buf.remove(buf.length() - 1);
            redrawField();
          }
          break;
        }
        if (!t.isNewPress) break;   // every other key: single-fire only
        if (c == K_DONE) done = true;
        else if (c == K_CANCEL) cancelled = true;
        else if (c == K_SHIFT) {
          shift = !shift;
          drawKeys();
          uiWaitForRelease();
          break;
        }
        else if (c == K_LAYER) {
          symbols = !symbols;
          shift = false;
          layoutKeys();
          drawKeys();
          uiWaitForRelease();
          break;
        }
        else {
          char out = (!symbols && shift && c >= 'a' && c <= 'z') ? (c - 32) : c;
          if (buf.length() < 48) buf += out;
        }
        redrawField();
        uiWaitForRelease();   // don't let a held tap bleed past DONE/CANCEL
        break;
      }
    }
  }
  return cancelled ? initial : buf;
}
