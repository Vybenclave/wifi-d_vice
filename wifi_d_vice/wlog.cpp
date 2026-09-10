#include <SD.h>
#include "wlog.h"
#include "sd_bus.h"
#include "pins.h"
#include "crypto.h"
#include "engagement.h"
#include "devtime.h"

// One file, one AAD, one fail counter -- these screens run one at a time.
static File     s_file;
static String   s_aad;          // engagement header, verbatim -- the GCM AAD for every row
static uint32_t s_encFails = 0;

// Sanitized for FAT32 path safety -- the client folder name comes from an
// operator-typed engagement field, not a fixed set of values.
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
  wlogClose();                       // never leak a previously open handle
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

  s_aad = engagementHeaderLine();    // fixed AAD for every row in this file
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
      for (size_t i = 0; i < outLen; i++) s_file.printf("%02X", outBuf[i]);
      s_file.println();
      return;
    }
    // Armed but the encrypt failed -- NEVER write the row in the clear.
    // Drop it, leave a non-sensitive marker, count it for the caller's UI.
    s_encFails++;
    Serial.printf("[wlog] encrypt FAILED (#%lu), row len=%u -- dropped\n",
                  (unsigned long)s_encFails, (unsigned)len);
    s_file.println("ENC_FAIL");
    return;
  }

  s_file.println(row);               // genuinely not armed -- plaintext is expected
}

void wlogFlush()        { if (s_file) s_file.flush(); }
void wlogClose()        { if (s_file) s_file.close(); }   // File::close() nulls the impl -> wlogIsOpen() false
bool wlogIsOpen()       { return (bool)s_file; }
uint32_t wlogEncFails() { return s_encFails; }
