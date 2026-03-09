// ============================================================================
//  crypto.h — AES-128 Encryption Wrapper for LoRaLink-AnyToAny
//  (c) 2026 Steven P Williams (spw1.com). All Rights Reserved.
// ============================================================================
#pragma once

#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include <Arduino.h>
#include <esp_random.h>
#include <stdint.h>
#include <string.h>

// Default key (Reference only - used if no key saved)
// "LoRaLinkDefault!" = 16 bytes
static const uint8_t DEFAULT_AES_KEY[16] = {'L', 'o', 'R', 'a', 'L', 'i',
                                            'n', 'k', 'D', 'e', 'f', 'a',
                                            'u', 'l', 't', '!'};

// Parse a 32-character hex string into 16 bytes. Returns true on success.
inline bool parseHexKey(const char *hex, uint8_t *out) {
  if (strlen(hex) != 32)
    return false;
  for (int i = 0; i < 16; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    uint8_t hiVal, loVal;
    if (hi >= '0' && hi <= '9')
      hiVal = hi - '0';
    else if (hi >= 'A' && hi <= 'F')
      hiVal = hi - 'A' + 10;
    else if (hi >= 'a' && hi <= 'f')
      hiVal = hi - 'a' + 10;
    else
      return false;
    if (lo >= '0' && lo <= '9')
      loVal = lo - '0';
    else if (lo >= 'A' && lo <= 'F')
      loVal = lo - 'A' + 10;
    else if (lo >= 'a' && lo <= 'f')
      loVal = lo - 'a' + 10;
    else
      return false;
    out[i] = (hiVal << 4) | loVal;
  }
  return true;
}

// Encrypted wire format: | IV[12] | Tag[16] | ciphertext[64] | = 92 bytes
#define ENCRYPTED_PAYLOAD_SIZE 64
#define GCM_IV_SIZE 12
#define GCM_TAG_SIZE 16

// General purpose GCM encrypt/decrypt for variable-sized data
inline void encryptData(const uint8_t *plain, size_t size, uint8_t *outBuf,
                        const uint8_t *key) {
  uint8_t iv[GCM_IV_SIZE];
  esp_fill_random(iv, GCM_IV_SIZE);
  memcpy(outBuf, iv, GCM_IV_SIZE);

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);

  mbedtls_gcm_crypt_and_tag(
      &gcm, MBEDTLS_GCM_ENCRYPT, size, iv, GCM_IV_SIZE, nullptr, 0, plain,
      outBuf + GCM_IV_SIZE + GCM_TAG_SIZE, GCM_TAG_SIZE, outBuf + GCM_IV_SIZE);

  mbedtls_gcm_free(&gcm);
}

inline bool decryptData(const uint8_t *inBuf, size_t payloadSize,
                        uint8_t *outPlain, const uint8_t *key) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);

  int ret = mbedtls_gcm_auth_decrypt(
      &gcm, payloadSize, inBuf, GCM_IV_SIZE, nullptr, 0, inBuf + GCM_IV_SIZE,
      GCM_TAG_SIZE, inBuf + GCM_IV_SIZE + GCM_TAG_SIZE, outPlain);

  mbedtls_gcm_free(&gcm);
  return (ret == 0);
}

// Encrypt a MessagePacket (64 bytes) into a 92-byte output buffer.
inline void encryptPacket(const void *plainPacket, uint8_t *outBuf,
                          const uint8_t *key) {
  encryptData((const uint8_t *)plainPacket, ENCRYPTED_PAYLOAD_SIZE, outBuf,
              key);
}

// Decrypt a 92-byte buffer into a 64-byte MessagePacket.
inline bool decryptPacket(const uint8_t *inBuf, void *plainPacket,
                          const uint8_t *key) {
  return decryptData(inBuf, ENCRYPTED_PAYLOAD_SIZE, (uint8_t *)plainPacket,
                     key);
}
