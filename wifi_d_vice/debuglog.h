#pragma once
// Runtime-toggleable debug logging over the USB serial console. Replaces
// one-off Serial.printf() calls that need a recompile+reflash to turn on
// or off -- tags are enabled/disabled live by typing commands into the
// serial monitor while the device is running, no rebuild needed. All
// tags are OFF by default (session-only, not persisted to NVS -- a fresh
// boot starts quiet).
//
// Usage at a call site:
//   DLOG("wids", "tap x=%d y=%d hit=%d", t.x, t.y, (int)hit);
// Prints nothing unless "wids" (or "all") is currently enabled.
//
// Serial commands (type into the Serial Monitor, newline-terminated):
//   log on <tag>     -- enable one tag, e.g. "log on wids"
//   log off <tag>    -- disable one tag
//   log on all       -- enable every tag regardless of name
//   log off all      -- disable everything (back to the default quiet state)
//   log off          -- same as "log off all"
//   log list         -- print which tags are currently enabled
//
// dlogPoll() must be called once per loop() iteration (unconditionally,
// same pattern as gpsSharedLoop()) so commands typed at any time are
// picked up regardless of which screen is active.
void dlogPoll();
void dlogPrintf(const char *tag, const char *fmt, ...);
#define DLOG(tag, fmt, ...) dlogPrintf(tag, fmt, ##__VA_ARGS__)
