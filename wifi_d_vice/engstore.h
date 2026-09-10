#pragma once
#include <Arduino.h>

// SD-card store for engagement persistence. Each client gets its OWN folder
// at the card root, and everything for that engagement lives inside it:
//
//   /<client>/eng.txt        -- k=v lines: started, tester, armed_at,
//                               verifier, iters. `verifier` is a GCM record
//                               of a fixed known string, encrypted with the
//                               passphrase-derived key; selecting an
//                               existing client re-derives the key from the
//                               typed passphrase and decrypts it -- a good
//                               tag means the passphrase is right. The
//                               verifier is written ONCE (first ARM of a new
//                               client) and never rewritten, so the key for
//                               a client's logs can't silently change.
//   /<client>/wifi.dat       -- one saved Wi-Fi profile per line, same
//                               "E "/"P " armed/plaintext split as before.
//   /<client>/<date>/*.csv   -- scan logs (written by wlog.cpp).
//
//   /eng/active.txt          -- the one small global file: the name of the
//                               currently selected client. Boot reads this
//                               to know which folder to unlock.
//
// `started` means "armed, and the next boot should prompt for the
// passphrase". DISARM clears it (engStoreMarkSuspended) -- so a disarmed
// device boots straight in. "Clear data" (engStoreDeleteClient) removes the
// whole client folder.

void   engStoreBegin();        // mount SD, read the active client, load its eng.txt
bool   engStoreAvailable();    // SD mounted OK

// ---- client selection ----
// Names of the client folders on the card (root dirs, minus reserved ones).
int    engStoreListClients(String *out, int maxN);
bool   engStoreClientExists(const String &name);
// Make `name` the active client: sanitize, mkdir if new, load its eng.txt,
// persist the active pointer. Does NOT touch the crypto key -- the caller
// runs the passphrase validate/set flow.
void   engStoreSelectClient(const String &name);
String engStoreCurrentClient();
bool   engStoreHasVerifier();  // active client already has a stored verifier

bool   engStoreStarted();      // active client's eng.txt has started=1 (boot gate)
String engStoreClient();       // == engStoreCurrentClient()
String engStoreTester();
String engStoreArmedAt();
long   engStoreIters();        // PBKDF2 round count recorded for the active client
String engStoreSalt();         // KDF salt / verifier AAD for the active client --
                               // derive the key against THIS, not client|tester

// ARM: write started=1 (+ tester/armed_at). Writes the verifier only if the
// client doesn't have one yet. Requires cryptoHasKey().
void   engStoreMarkStarted(const String &client, const String &tester, const String &armedAt);
// DISARM: clear started=1 so the next boot doesn't prompt. Keeps the folder,
// the verifier and every log.
void   engStoreMarkSuspended();
// Decrypt the active client's stored verifier with the current key -> true if it matches.
bool   engStoreUnlockCheck();

// Recursively delete /<name>/ and everything in it. Clears the active
// pointer + RAM state if it was the active client.
void   engStoreDeleteClient(const String &name);
// Full-card-wipe path: forget the active pointer + RAM state (files are
// already gone).
void   engStoreClear();

// Wi-Fi profiles for the active client (or a shared /eng/wifi.dat when no
// client is selected -- bring-up). Saving de-dupes on SSID.
void   engStoreSaveWifi(const String &ssid, const String &pass);
bool   engStoreFindWifi(const String &ssid, String &passOut);
int    engStoreWifiCount();
