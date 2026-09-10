#pragma once
#include <Arduino.h>
// AES-256-GCM record encryption, keyed from the Engagement Page passphrase
// via PBKDF2-SHA256. The derived key lives in RAM only -- cryptoClearKey()
// (called on disarm) zeroes it; nothing sensitive is ever written to flash.

// PBKDF2 round count. Dropped from 100k -> 40k: still a solid stretch for
// an operator-chosen passphrase on a device whose only local attack
// surface is a pulled SD card, and ~2.5x faster to derive. The count in
// use is stored alongside each engagement so old cards still unlock.
#define CRYPTO_KDF_ITERS 40000

void cryptoSetPassphrase(const String &passphrase, const String &salt);
// Same, but with an explicit round count and an optional progress hook
// (called with 0..100 as the derivation grinds).
void cryptoSetPassphraseEx(const String &passphrase, const String &salt,
                           uint32_t iters, void (*progress)(int pct) = nullptr);
bool cryptoHasKey();
void cryptoClearKey();

// Encrypts `len` bytes with a fresh random 12-byte nonce. Output layout is
// [12-byte nonce][ciphertext, same length as plaintext][16-byte tag] --
// outCap must be at least len + 28. `aad` (e.g. the engagement header) is
// authenticated but not encrypted, so a log's provenance can be checked
// without decrypting its body.
bool cryptoEncryptRecord(const uint8_t *plaintext, size_t len,
                          const uint8_t *aad, size_t aadLen,
                          uint8_t *out, size_t outCap, size_t *outLen);

// Inverse of cryptoEncryptRecord: `in` is [12-byte nonce][ciphertext][16-byte
// tag]. Returns false if there's no key, the buffer is too small, or the GCM
// tag doesn't authenticate (wrong key / tampered data) -- the tag check is
// what makes this usable as a passphrase verifier.
bool cryptoDecryptRecord(const uint8_t *in, size_t inLen,
                          const uint8_t *aad, size_t aadLen,
                          uint8_t *out, size_t outCap, size_t *outLen);
