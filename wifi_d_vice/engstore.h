#pragma once
#include <Arduino.h>

// SD-card store for engagement persistence.
// Each client gets its own folder at the card root.
// The verifier is a GCM record encrypted with the passphrase key.
// The system writes the verifier once.
// It never rewrites the verifier.
// This prevents the client log key from changing silently.
// The started flag means the device must prompt for the passphrase on boot.
// DISARM clears this flag.
// Clear data removes the entire client folder.

void   engStoreBegin();        // mount SD, read the active client, load its eng.txt
bool   engStoreAvailable();    // SD mounted OK

// ---- client selection ----
int    engStoreListClients(String *out, int maxN);
bool   engStoreClientExists(const String &name);
// This function reads the armed status without switching context.
// It does not load the crypto key.
// It returns false if the client folder is missing.
bool   engStorePeekClient(const String &name, bool &started);
// This function sets the active client.
// It does not load the crypto key.
// The caller must run the passphrase validation flow.
void   engStoreSelectClient(const String &name);
String engStoreCurrentClient();
bool   engStoreHasVerifier();

bool   engStoreStarted();
String engStoreClient();
String engStoreTester();
String engStoreArmedAt();
long   engStoreIters();
String engStoreSalt();         // Derive the key against this salt.
                               // Do not use the client name or tester name.

// Write the started flag and tester data.
// Write the verifier only if the client lacks one.
// The system requires an active crypto key.
void   engStoreMarkStarted(const String &client, const String &tester, const String &armedAt);
void   engStoreMarkSuspended();
bool   engStoreUnlockCheck();

// Delete the client folder and all contents.
// Clear the active pointer if this was the active client.
void   engStoreDeleteClient(const String &name);
// Clear the active pointer and RAM state.
// The files are already gone.
void   engStoreClear();

// Save Wi-Fi profiles.
// The system de-duplicates entries by SSID.
void   engStoreSaveWifi(const String &ssid, const String &pass);
bool   engStoreFindWifi(const String &ssid, String &passOut);
int    engStoreWifiCount();
// List files under the active client folder.
// Return paths relative to that folder.
// Return file sizes.
int    engStoreListClientFiles(String *relPaths, uint32_t *sizes, int maxN);

// ---- jobs: one bounded self-assessment run inside an armed engagement ----
// Only one job can be open at a time.
int    engStoreJobStart();
void   engStoreJobEnd(const String &summary);
bool   engStoreJobOpen();
int    engStoreJobId();

// ---- notes journal: free-text, timestamped, append-only ----
// This module uses the existing wlog.cpp storage logic.
// Notes store two columns: utc and text.
// The text column appears last to prevent comma corruption.
// The system strips embedded newlines to spaces.
//
// The wlog module names the destination folder from engagementGet().client.
// This file tracks a different client variable.
// The engagement screen keeps these variables in sync.
// Call these functions only after the normal engagement screen flow.
// Do not call them immediately after engStoreSelectClient().
bool engStoreNotesOpen();
void engStoreNotesAdd(const String &text);
void engStoreNotesClose();

// ---- findings / POA&M register (client-level, persists across jobs) ----
// This register stores findings in a CSV file.
// The system must rewrite individual rows.
// An append-only encrypted stream cannot support this.
// The module reads the whole file, modifies it, and writes it back.
// Each row uses an armed or plaintext hex blob.
// The system uses a fixed AAD string for this file.
struct Finding {
  int    id;
  String openedAt, jobId, controlRef, source;
  uint8_t severity;   // UI_SEV_OK/WATCH/ALERT, see ui.h -- same scale every detector uses
  String description, status, owner, targetDate, closedAt;
  String location;    // "lat,lon" (%.6f each) or "" -- see engStoreFindingOpen()
  String notes;
};

// Open a new finding with an open status.
// Pass an empty location string to auto-fill from the live GPS fix.
// Return the new finding id.
int  engStoreFindingOpen(const String &jobId, const String &controlRef,
                         const String &source, uint8_t severity,
                         const String &description, const String &location = "");
// Update mutable fields for an existing finding.
// Pass empty strings to skip changes.
// The system auto-stamps the closed date when you set the status to closed.
bool engStoreFindingUpdate(int id, const String &status, const String &owner,
                           const String &targetDate, const String &notes);
bool engStoreFindingClose(int id, const String &notes);
// Dump the whole register to an output array.
// Return the number of written entries.
int  engStoreFindingsList(Finding *out, int maxN);
// Count open findings for the active client only.
// This function requires the active decryption key.
int  engStoreFindingsOpenCount();
// Count findings for a specific job id.
// This function reuses a single record buffer.
int  engStoreFindingsCountForJob(const String &jobId);
