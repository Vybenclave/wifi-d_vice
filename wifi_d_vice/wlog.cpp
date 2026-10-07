#include <SD.h>
#include "wlog.h"
#include "sd_bus.h"
#include "pins.h"
#include "crypto.h"
#include "engagement.h"
#include "devtime.h"
#include "debuglog.h"
#include <mbedtls/sha256.h>

// The code processes one file at a time.
// Each file uses a single AAD value.
// Each file tracks one fail counter.
static File     s_file;
static String   s_aad;          // The code reads the AAD value from the engagement header.
static uint32_t s_encFails = 0;

// The code tracks a running SHA-256 hash over every data row.
// It excludes the three header lines.
// The hash finalizes on close.
// The code appends the hash to a manifest file.
// This allows offline tamper checks without the decryption key.
static mbedtls_sha256_context s_sha256_ctx;
static uint32_t               s_rowCount = 0;
static String                 s_dir;        // The directory path stores the file location.
static String                 s_bareName;   // The filename stores the name without the directory path.

// The function sanitizes strings for FAT32 paths.
// The client folder name comes from an operator field.
// The code replaces unsafe characters with underscores.
static String sanitizeForPath(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    out += ok ? c : '_';
  }
  return out.length() ? out : String("unknown_client");
}

bool wlogOpen(const char *name, const char *columns) {
  wlogClose();                       // The function closes any open handle first.
  sdBusBegin();
  if (!SD.begin(SD_CS, sdSPI)) return false;

  String d1  = "/" + sanitizeForPath(engagementGet().client);
  String dir = d1 + "/" + devDateString();
  if (!SD.exists(d1))  SD.mkdir(d1);
  if (!SD.exists(dir)) SD.mkdir(dir);

  char path[96];
  int n = 0;
  do {
    snprintf(path, sizeof(path), "%s/%s_%03d.csv", dir.c_str(), name, n++);
  } while (SD.exists(path) && n < 1000);

  s_file = SD.open(path, FILE_WRITE);
  if (!s_file) return false;

  mbedtls_sha256_init(&s_sha256_ctx);
  mbedtls_sha256_starts(&s_sha256_ctx, 0);
  s_rowCount = 0;
  s_dir = dir;
  String bareName = path;
  int slash = bareName.lastIndexOf('/');
  if (slash >= 0) bareName = bareName.substring(slash + 1);
  s_bareName = bareName;

  s_aad = engagementHeaderLine();    // The AAD value stays fixed for every row.
  s_encFails = 0;
  s_file.println(s_aad);
  s_file.println(cryptoHasKey() ? "encrypted=aes256gcm-hex" : "encrypted=none");
  s_file.println(columns);
  s_file.flush();
  return true;
}

void wlogRow(const char *row) {
  if (!s_file) return;

  if (cryptoHasKey()) {
    uint8_t outBuf[256];
    size_t  outLen, len = strlen(row);
    if (len + 28 <= sizeof(outBuf) &&
        cryptoEncryptRecord((const uint8_t *)row, len,
                            (const uint8_t *)s_aad.c_str(), s_aad.length(),
                            outBuf, sizeof(outBuf), &outLen)) {
      // The code builds the hex string in a buffer first.
      // This ensures the manifest hash matches the exact bytes on disk.
      // The buffer size accounts for the null terminator.
      char hexBuf[sizeof(outBuf) * 2 + 1];
      size_t hLen = 0;
      for (size_t i = 0; i < outLen; i++) {
        hLen += sprintf(hexBuf + hLen, "%02X", outBuf[i]);
      }
      mbedtls_sha256_update(&s_sha256_ctx, (const unsigned char *)hexBuf, hLen);
      s_rowCount++;
      s_file.write((const uint8_t *)hexBuf, hLen);
      s_file.println();
      return;
    }
    // The encryption failed.
    // The code drops the row.
    // It writes a non-sensitive marker instead.
    // It increments the fail counter for the UI.
    s_encFails++;
    DLOG("wlog", "encrypt FAILED (#%lu), row len=%u -- dropped",
         (unsigned long)s_encFails, (unsigned)len);
    static const char *failStr = "ENC_FAIL";
    mbedtls_sha256_update(&s_sha256_ctx, (const unsigned char *)failStr, strlen(failStr));
    s_rowCount++;
    s_file.println(failStr);
    return;
  }

  // Encryption is disabled.
  // The code hashes the exact row bytes.
  // It writes the plaintext row.
  mbedtls_sha256_update(&s_sha256_ctx, (const unsigned char *)row, strlen(row));
  s_rowCount++;
  s_file.println(row);
}

void wlogFlush()        { if (s_file) s_file.flush(); }

void wlogClose() {
  if (s_file) {
    unsigned char hash[32];
    mbedtls_sha256_finish(&s_sha256_ctx, hash);
    mbedtls_sha256_free(&s_sha256_ctx);

    char hashHex[65];
    for (int i = 0; i < 32; i++) sprintf(hashHex + i * 2, "%02x", hash[i]);
    hashHex[64] = '\0';

    String manifestPath = s_dir + "/manifest.txt";
    File manifest = SD.open(manifestPath, FILE_APPEND);
    if (!manifest) manifest = SD.open(manifestPath, FILE_WRITE);   // The append flag does not create missing files.
    if (manifest) {
      manifest.printf("%s,%s,%u\n", s_bareName.c_str(), hashHex, (unsigned)s_rowCount);
      manifest.close();
    }

    s_file.close();   // The close method nulls the file handle.
  }
}

bool wlogIsOpen()       { return (bool)s_file; }
uint32_t wlogEncFails() { return s_encFails; }
