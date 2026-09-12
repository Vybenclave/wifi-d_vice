#include "theme.h"
#include <Preferences.h>

static int s_id = THEME_BASIC;
static const char *NAMES[THEME_N] = { "Basic", "Vice" };

void themeLoad() {
  Preferences p;
  p.begin("touchcal", true);
  s_id = p.getInt("theme", THEME_BASIC);
  p.end();
  if (s_id < 0 || s_id >= THEME_N) s_id = THEME_BASIC;
}

void themeSet(int id) {
  if (id < 0 || id >= THEME_N) return;
  s_id = id;
  Preferences p;
  p.begin("touchcal", false);
  p.putInt("theme", id);
  p.end();
}

int         themeGet()          { return s_id; }
const char *themeName(int id)   { return (id >= 0 && id < THEME_N) ? NAMES[id] : "?"; }
bool        themeIsVice()       { return s_id == THEME_VICE; }
