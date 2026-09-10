#pragma once
#include <Arduino.h>
// Shared SD record logger, factored out of gps_wardrive.cpp so the
// encryption / provenance rules live in exactly ONE place instead of being
// re-implemented per scan screen.
//
//   * one file open at a time -- these screens run one at a time, so a
//     single static File is all that's needed (no per-caller handle).
//   * a file opens with the plaintext engagement-header line (client /
//     tester / armed-at), an `encrypted=` marker, then the CSV column
//     header -- so a pulled card self-documents its provenance without the
//     key.
//   * that header line is captured VERBATIM as the AES-256-GCM AAD for
//     every row. Do NOT rebuild it per row from engagementHeaderLine() --
//     that embeds a live clock and nothing would ever decrypt.
//   * when a key is armed (Engagement Page passphrase set) every data row
//     is encrypted, hex-encoded, one record per line. If an encrypt ever
//     fails the row is DROPPED and an `ENC_FAIL` marker written -- never
//     plaintext while armed. The drop is counted (wlogEncFails()).
//   * with nothing armed, rows are plaintext (bring-up / no-engagement).

// sdBusBegin() + SD.begin(), ensure /<client>/<date>/, open the next free
// <name>_NNN.csv, write the engagement header + `encrypted=` marker +
// `columns`, capture the AAD and reset the fail counter. Any previously
// open file is closed first. Returns false (and logs nothing) if the SD
// card isn't ready.
bool wlogOpen(const char *name, const char *columns);

// Append one CSV row (no trailing newline needed -- added here). Encrypts
// when a key is armed, per the rules above. No-op if no file is open.
void wlogRow(const char *row);
inline void wlogRow(const String &row) { wlogRow(row.c_str()); }

void wlogFlush();        // flush after a scan batch, not per row
void wlogClose();
bool wlogIsOpen();
uint32_t wlogEncFails(); // rows dropped because an armed encrypt failed
