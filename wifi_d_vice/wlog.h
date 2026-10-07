#pragma once
#include <Arduino.h>
// Shared SD record logger. The module centralizes encryption rules.
//
// Open one file at a time. The module captures the header line verbatim.
// Do not rebuild the header per row. A live clock breaks decryption.
// Encrypted rows drop on failure. The module writes an ENC_FAIL marker.
// The close function appends a manifest line. The line tracks the SHA-256 of data rows.

// Open the next free CSV file. The function closes any open file first.
// Return false if the SD card is not ready.
bool wlogOpen(const char *name, const char *columns);

// Append one CSV row (no trailing newline needed -- added here). Encrypts
// when a key is armed, per the rules above. No-op if no file is open.
void wlogRow(const char *row);
inline void wlogRow(const String &row) { wlogRow(row.c_str()); }

void wlogFlush();
void wlogClose();
bool wlogIsOpen();
uint32_t wlogEncFails();
