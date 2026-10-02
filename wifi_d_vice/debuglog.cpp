#include <Arduino.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include "debuglog.h"

static const int MAX_TAGS = 12, TAG_LEN = 16;
static char s_tags[MAX_TAGS][TAG_LEN];
static int  s_tagN = 0;
static bool s_allOn = false;

static bool tagEnabled(const char *tag) {
  if (s_allOn) return true;
  for (int i = 0; i < s_tagN; i++) if (strcmp(s_tags[i], tag) == 0) return true;
  return false;
}

static void enableTag(const char *tag) {
  if (strcasecmp(tag, "all") == 0) { s_allOn = true; return; }
  for (int i = 0; i < s_tagN; i++) if (strcmp(s_tags[i], tag) == 0) return;   // already on
  if (s_tagN < MAX_TAGS) {
    strncpy(s_tags[s_tagN], tag, TAG_LEN - 1);
    s_tags[s_tagN][TAG_LEN - 1] = 0;
    s_tagN++;
  } else {
    Serial.println("[dlog] tag table full -- 'log off <tag>' something first");
  }
}

static void disableTag(const char *tag) {
  if (strcasecmp(tag, "all") == 0) { s_allOn = false; s_tagN = 0; return; }
  for (int i = 0; i < s_tagN; i++) {
    if (strcmp(s_tags[i], tag) == 0) {
      for (int j = i; j < s_tagN - 1; j++) strcpy(s_tags[j], s_tags[j + 1]);
      s_tagN--;
      return;
    }
  }
}

static void listTags() {
  Serial.print("[dlog] enabled: ");
  if (s_allOn) { Serial.println("ALL"); return; }
  if (s_tagN == 0) { Serial.println("(none)"); return; }
  for (int i = 0; i < s_tagN; i++) { Serial.print(s_tags[i]); Serial.print(' '); }
  Serial.println();
}

// Only ever acts on lines starting with "log " -- anything else typed
// into the Serial Monitor (blank lines, stray input) is silently ignored
// rather than erroring, since this console isn't exclusively a command
// prompt.
static void handleLine(char *line) {
  char *verb = strtok(line, " ");
  if (!verb || strcasecmp(verb, "log") != 0) return;
  char *sub = strtok(nullptr, " ");
  if (!sub) return;
  if (strcasecmp(sub, "list") == 0) { listTags(); return; }
  char *tag = strtok(nullptr, " ");
  if (strcasecmp(sub, "off") == 0 && !tag) {   // bare "log off" == "log off all"
    disableTag("all");
    Serial.println("[dlog] all off");
    return;
  }
  if (!tag) return;
  if (strcasecmp(sub, "on") == 0)       { enableTag(tag);  Serial.printf("[dlog] on:  %s\n", tag); }
  else if (strcasecmp(sub, "off") == 0) { disableTag(tag); Serial.printf("[dlog] off: %s\n", tag); }
}

void dlogPoll() {
  static char buf[64];
  static int n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[n] = 0;
      if (n > 0) handleLine(buf);
      n = 0;
      continue;
    }
    if (n < (int)sizeof(buf) - 1) buf[n++] = c;
  }
}

void dlogPrintf(const char *tag, const char *fmt, ...) {
  if (!tagEnabled(tag)) return;
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  Serial.printf("[%s] %s\n", tag, msg);
}
