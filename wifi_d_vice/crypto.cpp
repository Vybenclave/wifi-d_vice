#include "crypto.h"
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <string.h>
#include <esp_random.h>

static uint8_t sessionKey[32];
static bool hasKey = false;

// PBKDF2-HMAC-SHA256, dkLen == 32 (exactly one output block), hand-rolled
// so it can yield a progress % as it runs. Reuses one HMAC context and
// mbedtls_md_hmac_reset() between rounds -- the SHA256 is HW-accelerated on
// the ESP32, so this is about as fast as mbedtls' own pbkdf2.
void cryptoSetPassphraseEx(const String &passphrase, const String &salt,
                           uint32_t iters, void (*progress)(int pct)) {
  if (iters < 1) iters = 1;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1 /* HMAC */);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char *)passphrase.c_str(), passphrase.length());

  uint8_t U[32], T[32];
  const uint8_t blk1[4] = {0, 0, 0, 1};
  mbedtls_md_hmac_update(&ctx, (const unsigned char *)salt.c_str(), salt.length());
  mbedtls_md_hmac_update(&ctx, blk1, 4);
  mbedtls_md_hmac_finish(&ctx, U);
  memcpy(T, U, 32);

  int lastPct = 0;
  for (uint32_t i = 1; i < iters; i++) {
    mbedtls_md_hmac_reset(&ctx);
    mbedtls_md_hmac_update(&ctx, U, 32);
    mbedtls_md_hmac_finish(&ctx, U);
    for (int j = 0; j < 32; j++) T[j] ^= U[j];
    if (progress) {
      int pct = (int)((uint64_t)i * 100 / iters);
      if (pct != lastPct) { lastPct = pct; progress(pct); }
    }
  }
  mbedtls_md_free(&ctx);
  memcpy(sessionKey, T, 32);
  hasKey = true;
  if (progress) progress(100);
}

void cryptoSetPassphrase(const String &passphrase, const String &salt) {
  cryptoSetPassphraseEx(passphrase, salt, CRYPTO_KDF_ITERS, nullptr);
}

bool cryptoHasKey() { return hasKey; }

void cryptoClearKey() {
  memset(sessionKey, 0, sizeof(sessionKey));
  hasKey = false;
}

bool cryptoEncryptRecord(const uint8_t *plaintext, size_t len,
                          const uint8_t *aad, size_t aadLen,
                          uint8_t *out, size_t outCap, size_t *outLen) {
  if (!hasKey) return false;
  const size_t nonceLen = 12, tagLen = 16;
  if (outCap < len + nonceLen + tagLen) return false;

  uint8_t nonce[12];
  for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)esp_random();

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, sessionKey, 256);
  if (rc != 0) { Serial.printf("[crypto] gcm_setkey rc=-0x%04X\n", -rc); mbedtls_gcm_free(&ctx); return false; }

  uint8_t tag[16];
  rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, len, nonce, nonceLen,
                                  aad, aadLen, plaintext, out + nonceLen, tagLen, tag);
  mbedtls_gcm_free(&ctx);
  if (rc != 0) { Serial.printf("[crypto] gcm_crypt_and_tag rc=-0x%04X len=%u aadLen=%u\n", -rc, (unsigned)len, (unsigned)aadLen); return false; }

  memcpy(out, nonce, nonceLen);
  memcpy(out + nonceLen + len, tag, tagLen);
  *outLen = nonceLen + len + tagLen;
  return true;
}

bool cryptoDecryptRecord(const uint8_t *in, size_t inLen,
                          const uint8_t *aad, size_t aadLen,
                          uint8_t *out, size_t outCap, size_t *outLen) {
  if (!hasKey) return false;
  const size_t nonceLen = 12, tagLen = 16;
  if (inLen < nonceLen + tagLen) return false;
  size_t ctLen = inLen - nonceLen - tagLen;
  if (outCap < ctLen) return false;

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, sessionKey, 256);
  if (rc != 0) { mbedtls_gcm_free(&ctx); return false; }

  rc = mbedtls_gcm_auth_decrypt(&ctx, ctLen,
                                 in, nonceLen,
                                 aad, aadLen,
                                 in + nonceLen + ctLen, tagLen,   // tag
                                 in + nonceLen,                   // ciphertext
                                 out);
  mbedtls_gcm_free(&ctx);
  if (rc != 0) return false;
  *outLen = ctLen;
  return true;
}
