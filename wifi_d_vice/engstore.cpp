#include "engstore.h"
#include <SD.h>
#include "sd_bus.h"
#include "pins.h"
#include "crypto.h"
#include "devtime.h"
#include "wlog.h"
#include "gps_shared.h"

static const char *PTR_DIR   = "/eng";
static const char *PTR_FILE  = "/eng/active.txt";
static const char *JOB_PTR_FILE = "/eng/job_active.txt";
static const char *LEGACY_ST = "/eng/state.txt";
static const char *LEGACY_WD = "/eng/wifi.dat";
static const char *VERIFY_PT  = "VYBEN-ENG-UNLOCK-v1";
static const char  SEP        = '\x1f';   // This character separates the ssid and pass fields.

static bool   s_sd = false;
static String s_client;                   // active client folder name ("" = none)
static bool   s_started = false;
static String s_tester, s_armedAt, s_verifierHex;
static String s_salt;                     // The system stores the KDF salt and verifier AAD verbatim.
                                          // New clients use the client name.
                                          // Migrated cards use the format client pipe tester.
                                          // The code keeps this value separate from the tester field.
                                          // This keeps the tester field free.
                                          // It also prevents decryption errors.
static long   s_iters = 100000;           // This value sets the KDF rounds for the current client.

// ---- job state (one bounded run, see engStoreJobStart()) ----
static bool   s_jobOpen = false;
static String s_jobPath;        // full path to the open job_NNN.txt
static int    s_jobId = -1;
static String s_jobStartedAt;

// ---- path helpers ----
// The client folder name comes from operator input.
// The code matches the path sanitizer in wlog.cpp.
// This ensures both modules use the same folder.
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

// ---- recursive delete ----
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
    if      (k == "started")   s_started = (v == "1");
    else if (k == "tester")    s_tester = v;
    else if (k == "armed_at")  s_armedAt = v;
    else if (k == "verifier")  s_verifierHex = v;
    else if (k == "salt")      s_salt = v;
    else if (k == "iters")     s_iters = v.toInt();
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

// ---- legacy migration ----
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

// The compiler requires this forward declaration.
// The function appears later in this file.
// The code keeps it file-local.
// It does not appear in the header file.
static void recoverOrphanedJob();

void engStoreBegin() {
  sdBusBegin();
  s_sd = SD.begin(SD_CS, sdSPI);
  s_client = "";
  if (!s_sd) { loadClientState(); return; }
  if (!SD.exists(PTR_DIR)) SD.mkdir(PTR_DIR);

  s_client = readActivePtr();
  if (s_client.length() == 0 && SD.exists(LEGACY_ST)) migrateLegacy();
  loadClientState();
  recoverOrphanedJob();
}

bool   engStoreAvailable()      { return s_sd; }
String engStoreCurrentClient()  { return s_client; }
String engStoreClient()         { return s_client; }
String engStoreTester()         { return s_tester; }
String engStoreArmedAt()        { return s_armedAt; }
// The system has not locked a key count yet.
// The function returns the count for the first ARM.
// This keeps the unlock key identical to the verifier key.
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

bool engStorePeekClient(const String &name, bool &started) {
  String s = sanitizeName(name);
  started = false;
  if (!s_sd || s.length() == 0) return false;
  File f = SD.open("/" + s + "/eng.txt", FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String k = line.substring(0, eq), v = line.substring(eq + 1);
    if (k == "started") started = (v == "1");
  }
  f.close();
  return true;
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
    // Any open job's backing file is gone with the folder -- just forget
    // it in RAM, nothing left to write.
    s_jobOpen = false;
    s_jobId = -1;
    s_jobPath = s_jobStartedAt = "";
    s_client = "";
    clearActivePtr();
    loadClientState();   // zeroes the RAM copy
  }
}

void engStoreClear() {
  // The full-card wipe already removed every folder; just drop the pointer
  // and the RAM state. Any open job's backing file is gone too.
  s_jobOpen = false;
  s_jobId = -1;
  s_jobPath = s_jobStartedAt = "";
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

// ---- findings register ----
// The file uses the same hex-blob format as the wifi file.
// The code uses a fixed AAD string to separate domains.
// Each blob contains thirteen fields.
// The code truncates text fields to one hundred sixty characters.
// It also strips embedded newlines.
// This prevents row splitting during reads.
static const char *FINDINGS_AAD = "find";
static const size_t FINDING_BUF = 768;   // comfortably covers 13 fields incl. two 160-char free-text ones + GCM overhead

static String findingsPath() { return clientDir() + "/findings.csv"; }

static String sanitizeFindingText(const String &s, size_t maxLen) {
  String out = s.substring(0, maxLen);
  out.replace("\n", " ");
  out.replace("\r", " ");
  return out;
}

static String encodeFinding(const Finding &fd) {
  String blob = String(fd.id) + SEP + fd.openedAt + SEP + fd.jobId + SEP +
                fd.controlRef + SEP + fd.source + SEP + String(fd.severity) + SEP +
                fd.description + SEP + fd.status + SEP + fd.owner + SEP + fd.targetDate + SEP +
                fd.closedAt + SEP + fd.location + SEP + fd.notes;
  if (cryptoHasKey()) {
    uint8_t out[FINDING_BUF];
    size_t n = 0;
    if (blob.length() + 28 <= sizeof(out) &&
        cryptoEncryptRecord((const uint8_t *)blob.c_str(), blob.length(),
                            (const uint8_t *)FINDINGS_AAD, 4, out, sizeof(out), &n)) {
      return "E " + toHex(out, n);
    }
  }
  return "P " + toHex((const uint8_t *)blob.c_str(), blob.length());
}

// The parser splits the blob into thirteen fields.
// The notes field takes the remainder.
// This prevents desync if the notes contain the separator.
// The keyboard cannot type the separator character.
static bool decodeFinding(const String &line, Finding &fd) {
  if (line.length() < 3 || line[1] != ' ') return false;
  char kind = line[0];
  String hex = line.substring(2);
  uint8_t raw[FINDING_BUF];
  size_t rawN = fromHex(hex, raw, sizeof(raw));
  if (rawN == 0) return false;

  uint8_t blob[FINDING_BUF];
  size_t blobN = 0;
  if (kind == 'P') {
    memcpy(blob, raw, rawN);
    blobN = rawN;
  } else if (kind == 'E') {
    if (!cryptoDecryptRecord(raw, rawN, (const uint8_t *)FINDINGS_AAD, 4, blob, sizeof(blob), &blobN)) return false;
  } else {
    return false;
  }

  String s;
  s.reserve(blobN);
  for (size_t i = 0; i < blobN; i++) s += (char)blob[i];

  String parts[13];
  int idx = 0, start = 0;
  for (int i = 0; i < (int)s.length() && idx < 12; i++) {
    if (s[i] == SEP) { parts[idx++] = s.substring(start, i); start = i + 1; }
  }
  if (idx != 12) return false;      // malformed -- didn't find all 12 separators
  parts[idx] = s.substring(start);  // 13th field (notes) is the remainder

  fd.id         = parts[0].toInt();
  fd.openedAt   = parts[1];
  fd.jobId      = parts[2];
  fd.controlRef = parts[3];
  fd.source     = parts[4];
  fd.severity   = (uint8_t)parts[5].toInt();
  fd.description = parts[6];
  fd.status     = parts[7];
  fd.owner      = parts[8];
  fd.targetDate = parts[9];
  fd.closedAt   = parts[10];
  fd.location   = parts[11];
  fd.notes      = parts[12];
  return true;
}

int engStoreFindingOpen(const String &jobId, const String &controlRef,
                        const String &source, uint8_t severity,
                        const String &description, const String &location) {
  if (!s_sd || s_client.length() == 0) return -1;
  if (!SD.exists(clientDir())) SD.mkdir(clientDir());

  // The code uses a separate counter file.
  // Scanning the main file would require decryption.
  // Decryption fails if the current key differs from older keys.
  // This counter file works in all cases.
  String seqPath = clientDir() + "/findings_seq.txt";
  int id = 0;
  File sf = SD.open(seqPath, FILE_READ);
  if (sf) { id = sf.parseInt(); sf.close(); }
  File sw = SD.open(seqPath, FILE_WRITE);   // truncates
  if (sw) { sw.print(id + 1); sw.close(); }

  Finding fd;
  fd.id = id;
  fd.openedAt   = devTimeNowString();
  fd.jobId      = jobId;
  fd.controlRef = controlRef;
  fd.source     = sanitizeFindingText(source, 40);
  fd.severity   = severity;
  fd.description = sanitizeFindingText(description, 160);
  fd.status     = "open";
  fd.owner      = s_tester;   // default to the current tester; engStoreFindingUpdate() can change it
  fd.targetDate = "";
  fd.closedAt   = "";
  // The function uses the caller coordinate if available.
  // It falls back to the shared GPS reader otherwise.
  // The field stays empty if neither source has a fix.
  fd.location = location;
  if (fd.location.length() == 0 && gpsShared().location.isValid()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6f,%.6f", gpsShared().location.lat(), gpsShared().location.lng());
    fd.location = buf;
  }
  fd.notes      = "";

  File f = SD.open(findingsPath(), FILE_APPEND);
  if (!f) f = SD.open(findingsPath(), FILE_WRITE);   // The SD library may not create a file with FILE_APPEND.
                                                         // The code falls back to FILE_WRITE to create it.
  if (!f) return -1;
  f.println(encodeFinding(fd));
  f.close();
  return fd.id;
}

bool engStoreFindingUpdate(int id, const String &status, const String &owner,
                           const String &targetDate, const String &notes) {
  if (!s_sd || s_client.length() == 0) return false;
  String path = findingsPath();
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  String rewritten;
  bool found = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    Finding fd;
    if (!found && decodeFinding(line, fd) && fd.id == id) {
      found = true;
      if (status.length()) {
        fd.status = status;
        if (status == "closed" && fd.closedAt.length() == 0) fd.closedAt = devTimeNowString();
      }
      if (owner.length())      fd.owner = sanitizeFindingText(owner, 40);
      if (targetDate.length()) fd.targetDate = targetDate;
      if (notes.length())      fd.notes = sanitizeFindingText(notes, 160);
      rewritten += encodeFinding(fd) + "\n";
    } else {
      rewritten += line + "\n";   // The code passes through unmatched lines verbatim.
                                   // This prevents data loss for undecodable rows.
    }
  }
  f.close();
  if (!found) return false;

  File w = SD.open(path, FILE_WRITE);   // truncates
  if (!w) return false;
  w.print(rewritten);
  w.close();
  return true;
}

bool engStoreFindingClose(int id, const String &notes) {
  return engStoreFindingUpdate(id, "closed", "", "", notes);
}

int engStoreFindingsList(Finding *out, int maxN) {
  if (!s_sd || s_client.length() == 0 || maxN <= 0) return 0;
  File f = SD.open(findingsPath(), FILE_READ);
  if (!f) return 0;
  int n = 0;
  while (f.available() && n < maxN) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (decodeFinding(line, out[n])) n++;
  }
  f.close();
  return n;
}

int engStoreFindingsOpenCount() {
  if (!s_sd || s_client.length() == 0) return 0;
  File f = SD.open(findingsPath(), FILE_READ);
  if (!f) return 0;
  int n = 0;
  Finding fd;   // reused per line -- no need for a whole array just to count
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (decodeFinding(line, fd) && fd.status != "closed") n++;
  }
  f.close();
  return n;
}

int engStoreFindingsCountForJob(const String &jobId) {
  if (!s_sd || s_client.length() == 0) return 0;
  File f = SD.open(findingsPath(), FILE_READ);
  if (!f) return 0;
  int n = 0;
  Finding fd;   // reused per line -- same as engStoreFindingsOpenCount()
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (decodeFinding(line, fd) && fd.jobId == jobId) n++;
  }
  f.close();
  return n;
}

// ---- jobs ----------------------------------------------------------
int engStoreJobStart() {
  if (!s_sd || s_client.length() == 0) return -1;
  if (s_jobOpen) engStoreJobEnd("");   // The code closes any open job before starting a new one.
                                       // This prevents resource leaks.

  String dir = clientDir() + "/" + devDateString();
  if (!SD.exists(clientDir())) SD.mkdir(clientDir());
  if (!SD.exists(dir)) SD.mkdir(dir);

  char path[96];
  int n = 0;
  do {
    snprintf(path, sizeof(path), "%s/job_%03d.txt", dir.c_str(), n++);
  } while (SD.exists(path) && n < 1000);

  File f = SD.open(path, FILE_WRITE);
  if (!f) return -1;

  s_jobId = n - 1;
  s_jobPath = path;
  s_jobStartedAt = devTimeNowString();

  f.printf("job_id=%d\n", s_jobId);
  f.printf("started_at=%s\n", s_jobStartedAt.c_str());
  f.printf("status=active\n");
  f.close();

  s_jobOpen = true;

  File ptr = SD.open(JOB_PTR_FILE, FILE_WRITE);
  if (ptr) { ptr.println(s_jobPath); ptr.close(); }

  return s_jobId;
}

void engStoreJobEnd(const String &summary) {
  if (!s_sd || !s_jobOpen) return;
  // FILE_WRITE truncates on this core (same as writeClientState()) --
  // rewrite every field fresh from RAM rather than trying to append/edit
  // in place.
  File f = SD.open(s_jobPath, FILE_WRITE);
  if (f) {
    f.printf("job_id=%d\n", s_jobId);
    f.printf("started_at=%s\n", s_jobStartedAt.c_str());
    f.printf("ended_at=%s\n", devTimeNowString().c_str());
    f.printf("status=completed\n");
    f.printf("summary=%s\n", summary.c_str());
    f.close();
  }
  s_jobOpen = false;
  s_jobId = -1;
  s_jobPath = "";
  s_jobStartedAt = "";

  if (s_sd && SD.exists(JOB_PTR_FILE)) SD.remove(JOB_PTR_FILE);
}

bool   engStoreJobOpen()      { return s_jobOpen; }
int    engStoreJobId()        { return s_jobOpen ? s_jobId : -1; }

// ---- notes journal ---------------------------------------------------
bool engStoreNotesOpen() { return wlogOpen("notes", "utc,text"); }

void engStoreNotesAdd(const String &text) {
  String clean = text;
  clean.replace("\n", " ");
  clean.replace("\r", " ");   // The code replaces carriage returns with spaces.
                              // This prevents row splitting in the log file.
  String row = devTimeNowString() + "," + clean;   // text is deliberately last -- a comma inside it doesn't corrupt a first-comma split
  wlogRow(row);
  wlogFlush();
}

void engStoreNotesClose() { wlogClose(); }

// ---- boot-safe job recovery ------------------------------------------
static void recoverOrphanedJob() {
  if (!s_sd || !SD.exists(JOB_PTR_FILE)) return;
  File ptr = SD.open(JOB_PTR_FILE, FILE_READ);
  if (!ptr) return;
  String jobPath = ptr.readStringUntil('\n');
  ptr.close();
  jobPath.trim();

  if (jobPath.length() == 0 || !SD.exists(jobPath)) {
    SD.remove(JOB_PTR_FILE);
    return;
  }

  File f = SD.open(jobPath, FILE_READ);
  if (!f) { SD.remove(JOB_PTR_FILE); return; }
  int recoveredId = -1;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("job_id=")) {
      recoveredId = line.substring(7).toInt();
      break;
    }
  }
  f.close();

  f = SD.open(jobPath, FILE_WRITE);
  if (f) {
    f.printf("job_id=%d\n", recoveredId);
    f.printf("ended_at=%s\n", devTimeNowString().c_str());
    f.printf("status=completed\n");
    f.printf("summary=ended by reboot\n");
    f.close();
  }
  SD.remove(JOB_PTR_FILE);
}

// ---- client file listing ---------------------------------------------
static void listClientFilesRec(const String &path, String *relPaths, uint32_t *sizes, int &n, int maxN, const String &baseDir) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File entry = dir.openNextFile();
  while (entry && n < maxN) {
    String p = entry.path();
    bool isDir = entry.isDirectory();
    uint32_t sz = entry.size();
    entry.close();
    if (isDir) {
      listClientFilesRec(p, relPaths, sizes, n, maxN, baseDir);
    } else {
      if (n < maxN) {
        String rel = p.substring(baseDir.length());
        if (rel.length() > 0 && rel[0] == '/') rel = rel.substring(1);
        relPaths[n] = rel;
        sizes[n] = sz;
        n++;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();
}

int engStoreListClientFiles(String *relPaths, uint32_t *sizes, int maxN) {
  if (!s_sd || s_client.length() == 0 || maxN <= 0) return 0;
  int n = 0;
  listClientFilesRec(clientDir(), relPaths, sizes, n, maxN, clientDir());
  return n;
}
