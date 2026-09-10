#include "engstore.h"
#include <SD.h>
#include "sd_bus.h"
#include "pins.h"
#include "crypto.h"

static const char *PTR_DIR   = "/eng";
static const char *PTR_FILE  = "/eng/active.txt";
static const char *LEGACY_ST = "/eng/state.txt";     // pre-per-client global engagement
static const char *LEGACY_WD = "/eng/wifi.dat";
static const char *VERIFY_PT  = "VYBEN-ENG-UNLOCK-v1";
static const char  SEP        = '\x1f';   // ssid/pass delimiter inside a record blob

static bool   s_sd = false;
static String s_client;                   // active client folder name ("" = none)
static bool   s_started = false;
static String s_tester, s_armedAt, s_verifierHex;
static String s_salt;                     // KDF salt + verifier AAD, stored verbatim.
                                          // New clients: the client name. Migrated
                                          // pre-per-client cards: "client|tester".
                                          // Kept out of `tester` so that field is
                                          // free metadata and can't break decryption.
static long   s_iters = 100000;           // KDF rounds for THIS client (old cards: 100k)

// ---- path helpers ----
// FAT32-safe: the client folder name is an operator-typed field, not a fixed
// set of values. Matches wlog.cpp's sanitizeForPath so the folder wlog logs
// into is the same one this store uses.
static String sanitizeName(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    out += ok ? c : '_';
  }
  return out;
}
static String clientDir() { return "/" + s_client; }
static String engPath()   { return "/" + s_client + "/eng.txt"; }
static String wifiPath()  { return s_client.length() ? ("/" + s_client + "/wifi.dat")
                                                     : String(LEGACY_WD); }

// ---- hex helpers ----
static String toHex(const uint8_t *b, size_t n) {
  static const char *H = "0123456789ABCDEF";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[b[i] >> 4]; s += H[b[i] & 0xF]; }
  return s;
}
static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static size_t fromHex(const String &s, uint8_t *out, size_t outCap) {
  size_t n = 0;
  for (size_t i = 0; i + 1 < s.length() && n < outCap; i += 2) {
    int hi = hexVal(s[i]), lo = hexVal(s[i + 1]);
    if (hi < 0 || lo < 0) break;
    out[n++] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

// ---- recursive delete (mirrors system_screen.cpp's wipeDir) ----
static void wipeTree(const String &path) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File entry = dir.openNextFile();
  while (entry) {
    String p = entry.path();
    bool isDir = entry.isDirectory();
    entry.close();
    if (isDir) { wipeTree(p); SD.rmdir(p); }
    else SD.remove(p);
    entry = dir.openNextFile();
  }
  dir.close();
}

// ---- eng.txt (per client) ----
static void loadClientState() {
  s_started = false;
  s_tester = s_armedAt = s_verifierHex = "";
  s_salt = s_client;      // default for a client with no eng.txt yet
  s_iters = 100000;
  if (!s_sd || s_client.length() == 0) return;
  String p = engPath();
  if (!SD.exists(p)) return;
  File f = SD.open(p, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String k = line.substring(0, eq), v = line.substring(eq + 1);
    if      (k == "started")  s_started = (v == "1");
    else if (k == "tester")   s_tester = v;
    else if (k == "armed_at") s_armedAt = v;
    else if (k == "verifier") s_verifierHex = v;
    else if (k == "salt")     s_salt = v;
    else if (k == "iters")    s_iters = v.toInt();
  }
  f.close();
}

static void writeClientState() {
  if (!s_sd || s_client.length() == 0) return;
  if (!SD.exists(clientDir())) SD.mkdir(clientDir());
  File f = SD.open(engPath(), FILE_WRITE);   // FILE_WRITE truncates on this core
  if (!f) return;
  f.printf("started=%d\n", s_started ? 1 : 0);
  f.printf("tester=%s\n", s_tester.c_str());
  f.printf("armed_at=%s\n", s_armedAt.c_str());
  f.printf("verifier=%s\n", s_verifierHex.c_str());
  f.printf("salt=%s\n", s_salt.c_str());
  f.printf("iters=%ld\n", s_iters);
  f.close();
}

// ---- active-client pointer ----
static String readActivePtr() {
  if (!s_sd || !SD.exists(PTR_FILE)) return String("");
  File f = SD.open(PTR_FILE, FILE_READ);
  if (!f) return String("");
  String s = f.readStringUntil('\n');
  f.close();
  s.trim();
  return s;
}
static void writeActivePtr(const String &name) {
  if (!s_sd) return;
  if (!SD.exists(PTR_DIR)) SD.mkdir(PTR_DIR);
  File f = SD.open(PTR_FILE, FILE_WRITE);
  if (!f) return;
  f.println(name);
  f.close();
}
static void clearActivePtr() {
  if (s_sd && SD.exists(PTR_FILE)) SD.remove(PTR_FILE);
}

// ---- one-time migration of a pre-per-client global engagement ----
static void migrateLegacy() {
  File f = SD.open(LEGACY_ST, FILE_READ);
  if (!f) return;
  String client, tester, armedAt, verifier;
  bool started = false;
  long iters = 100000;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String k = line.substring(0, eq), v = line.substring(eq + 1);
    if      (k == "started")  started = (v == "1");
    else if (k == "client")   client = v;
    else if (k == "tester")   tester = v;
    else if (k == "armed_at") armedAt = v;
    else if (k == "verifier") verifier = v;
    else if (k == "iters")    iters = v.toInt();
  }
  f.close();
  if (client.length() == 0) { SD.remove(LEGACY_ST); return; }

  s_client = sanitizeName(client);
  if (!SD.exists(clientDir())) SD.mkdir(clientDir());
  s_started = started;
  s_tester = tester;
  s_armedAt = armedAt;
  s_verifierHex = verifier;
  s_salt = client + "|" + tester;   // the legacy KDF salt, preserved verbatim
  s_iters = iters;
  writeClientState();
  writeActivePtr(s_client);

  if (SD.exists(LEGACY_WD) && !SD.exists(wifiPath())) SD.rename(String(LEGACY_WD), wifiPath());
  SD.remove(LEGACY_ST);
}

void engStoreBegin() {
  sdBusBegin();
  s_sd = SD.begin(SD_CS, sdSPI);
  s_client = "";
  if (!s_sd) { loadClientState(); return; }
  if (!SD.exists(PTR_DIR)) SD.mkdir(PTR_DIR);

  s_client = readActivePtr();
  if (s_client.length() == 0 && SD.exists(LEGACY_ST)) migrateLegacy();
  loadClientState();
}

bool   engStoreAvailable()      { return s_sd; }
String engStoreCurrentClient()  { return s_client; }
String engStoreClient()         { return s_client; }
String engStoreTester()         { return s_tester; }
String engStoreArmedAt()        { return s_armedAt; }
// Until a verifier is committed (first ARM) nothing is locked to a count yet,
// so report the count the first ARM will use -- keeps the key a not-yet-armed
// client is unlocked with identical to the one its verifier gets built under.
long   engStoreIters()          { return s_verifierHex.length() ? s_iters : (long)CRYPTO_KDF_ITERS; }
String engStoreSalt()           { return s_salt.length() ? s_salt : s_client; }
bool   engStoreStarted()        { return s_sd && s_client.length() && s_started; }
bool   engStoreHasVerifier()    { return s_verifierHex.length() > 0; }

int engStoreListClients(String *out, int maxN) {
  if (!s_sd || maxN <= 0) return 0;
  File root = SD.open("/");
  if (!root) return 0;
  int n = 0;
  for (File e = root.openNextFile(); e && n < maxN; e = root.openNextFile()) {
    bool isDir = e.isDirectory();
    String nm = e.name();
    e.close();
    if (!isDir) continue;
    int sl = nm.lastIndexOf('/');
    if (sl >= 0) nm = nm.substring(sl + 1);
    if (nm.length() == 0 || nm[0] == '.') continue;
    if (nm == "eng" || nm == "System Volume Information" || nm == "SYSTEM~1") continue;
    out[n++] = nm;
  }
  root.close();
  return n;
}

bool engStoreClientExists(const String &name) {
  String s = sanitizeName(name);
  return s_sd && s.length() && SD.exists("/" + s);
}

void engStoreSelectClient(const String &name) {
  String s = sanitizeName(name);
  if (s.length() == 0 || s == s_client) return;
  s_client = s;
  if (s_sd && !SD.exists(clientDir())) SD.mkdir(clientDir());
  loadClientState();
  writeActivePtr(s_client);
}

void engStoreMarkStarted(const String &client, const String &tester, const String &armedAt) {
  if (!s_sd) return;
  String s = sanitizeName(client);
  if (s.length() == 0) return;
  if (s != s_client) { s_client = s; loadClientState(); }

  s_tester = tester;
  s_armedAt = armedAt;
  s_started = true;

  // Verifier is written ONCE, on the first ARM of a client. An existing
  // client keeps the verifier (and iters) its logs were encrypted against --
  // re-arming validates the typed passphrase against it, never replaces it.
  if (s_verifierHex.length() == 0) {
    s_iters = CRYPTO_KDF_ITERS;
    s_salt  = s_client;   // new client: salt is just the folder name
    uint8_t out[64];
    size_t n = 0;
    if (cryptoEncryptRecord((const uint8_t *)VERIFY_PT, strlen(VERIFY_PT),
                            (const uint8_t *)s_salt.c_str(), s_salt.length(),
                            out, sizeof(out), &n)) {
      s_verifierHex = toHex(out, n);
    }
  }
  writeClientState();
  writeActivePtr(s_client);
}

void engStoreMarkSuspended() {
  if (!s_sd || s_client.length() == 0) return;
  s_started = false;
  writeClientState();   // keeps the active pointer -> screen reopens on this client
}

bool engStoreUnlockCheck() {
  if (s_verifierHex.length() == 0) return false;
  uint8_t blob[64];
  size_t blobN = fromHex(s_verifierHex, blob, sizeof(blob));
  if (blobN == 0) return false;
  String aad = engStoreSalt();
  uint8_t pt[48];
  size_t ptN = 0;
  if (!cryptoDecryptRecord(blob, blobN,
                           (const uint8_t *)aad.c_str(), aad.length(),
                           pt, sizeof(pt), &ptN)) return false;
  return ptN == strlen(VERIFY_PT) && memcmp(pt, VERIFY_PT, ptN) == 0;
}

void engStoreDeleteClient(const String &name) {
  String s = sanitizeName(name);
  if (!s_sd || s.length() == 0) return;
  String dir = "/" + s;
  wipeTree(dir);
  SD.rmdir(dir);
  if (s == s_client) {
    s_client = "";
    clearActivePtr();
    loadClientState();   // zeroes the RAM copy
  }
}

void engStoreClear() {
  // The full-card wipe already removed every folder; just drop the pointer
  // and the RAM state.
  if (s_sd && SD.exists(PTR_FILE)) SD.remove(PTR_FILE);
  s_client = "";
  loadClientState();
}

// ---- Wi-Fi profiles (per active client, or /eng/wifi.dat when none) ----
static bool decodeWifiLine(const String &line, String &ssid, String &pass) {
  if (line.length() < 3 || line[1] != ' ') return false;
  char kind = line[0];
  String hex = line.substring(2);
  uint8_t raw[192];
  size_t rawN = fromHex(hex, raw, sizeof(raw));
  if (rawN == 0) return false;

  uint8_t blob[192];
  size_t blobN = 0;
  if (kind == 'P') {
    memcpy(blob, raw, rawN);
    blobN = rawN;
  } else if (kind == 'E') {
    if (!cryptoDecryptRecord(raw, rawN, (const uint8_t *)"wifi", 4,
                             blob, sizeof(blob), &blobN)) return false;
  } else {
    return false;
  }
  int sep = -1;
  for (size_t i = 0; i < blobN; i++) if (blob[i] == (uint8_t)SEP) { sep = (int)i; break; }
  if (sep < 0) return false;
  ssid = ""; pass = "";
  for (int i = 0; i < sep; i++) ssid += (char)blob[i];
  for (size_t i = sep + 1; i < blobN; i++) pass += (char)blob[i];
  return true;
}

static String encodeWifiLine(const String &ssid, const String &pass) {
  String blob = ssid + String(SEP) + pass;
  if (cryptoHasKey()) {
    uint8_t out[192];
    size_t n = 0;
    if (blob.length() + 28 <= sizeof(out) &&
        cryptoEncryptRecord((const uint8_t *)blob.c_str(), blob.length(),
                            (const uint8_t *)"wifi", 4, out, sizeof(out), &n)) {
      return "E " + toHex(out, n);
    }
  }
  return "P " + toHex((const uint8_t *)blob.c_str(), blob.length());
}

void engStoreSaveWifi(const String &ssid, const String &pass) {
  if (!s_sd || ssid.length() == 0) return;
  String dir = s_client.length() ? clientDir() : String(PTR_DIR);
  if (!SD.exists(dir)) SD.mkdir(dir);
  String path = wifiPath();

  String kept;
  File f = SD.open(path, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      String es, ep;
      if (decodeWifiLine(line, es, ep) && es == ssid) continue;   // replace it
      kept += line + "\n";
    }
    f.close();
  }
  kept += encodeWifiLine(ssid, pass) + "\n";

  File w = SD.open(path, FILE_WRITE);   // truncates
  if (!w) return;
  w.print(kept);
  w.close();
}

bool engStoreFindWifi(const String &ssid, String &passOut) {
  if (!s_sd) return false;
  File f = SD.open(wifiPath(), FILE_READ);
  if (!f) return false;
  bool found = false;
  while (f.available() && !found) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    String es, ep;
    if (decodeWifiLine(line, es, ep) && es == ssid) { passOut = ep; found = true; }
  }
  f.close();
  return found;
}

int engStoreWifiCount() {
  if (!s_sd) return 0;
  File f = SD.open(wifiPath(), FILE_READ);
  if (!f) return 0;
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) n++;
  }
  f.close();
  return n;
}
