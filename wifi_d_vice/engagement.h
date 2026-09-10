#pragma once
#include <Arduino.h>
// Engagement Page: client/tester/passphrase have to be filled in and the
// page explicitly ARMed before any active (transmit) feature may run.
// isArmed() is what any future active module must check before doing
// anything. The header line (client/tester + when it was armed, via the
// BLE-synced clock in devtime.h) gets embedded in every encrypted log's
// header for audit provenance.

struct Engagement {
  String client, tester;
  bool armed = false;
};

const Engagement &engagementGet();
bool engagementIsArmed();

// Boot-time gate: if /eng/state.txt on the SD card marks an engagement as
// started, block for the passphrase (verified against the stored blob) and
// restore the armed state + decryption key. Skippable -> stays locked.
void engagementBootUnlock();
// Wipe all RAM engagement state + key (called from the SD-wipe path).
void engagementResetAll();
// A one-line header embedded (as unencrypted, authenticated AAD) in every
// encrypted log record so captured data self-documents its engagement.
String engagementHeaderLine();
