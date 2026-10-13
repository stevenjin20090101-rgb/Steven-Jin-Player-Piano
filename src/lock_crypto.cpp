// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The Bluetooth lock's cryptography — see include/lock_crypto.h. Plain C++11
// on libsodium (the SDK's 1.0.12 on the ESP32; tools/lock-check's stand-in on
// the Mac) and <string.h> only.
#include "lock_crypto.h"
#include <string.h>
#include <sodium.h>

namespace lockcrypto {
namespace {

// The labels exactly as docs/BLE_LOCK.md writes them, without a NUL.
const char   kClaimLabel[] = "steven-piano-lock-v1";
const char   kAuthLabel[]  = "steven-piano-auth-v1";
const size_t kClaimLabelBytes = sizeof kClaimLabel - 1;   // 20
const size_t kAuthLabelBytes  = sizeof kAuthLabel - 1;    // 20

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// libsodium's small-order list (crypto_scalarmult/curve25519/ref10, from
// 1.0.16; the SDK's 1.0.12 only refuses the all-zero result afterwards).
const uint8_t kSmallOrder[7][32] = {
    { 0x00 },                                                                     // 0 (order 4)
    { 0x01 },                                                                     // 1 (order 1)
    { 0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
      0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00 },   // order 8
    { 0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1, 0x55, 0x9c, 0x83, 0xef, 0x5b,
      0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c, 0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57 },   // order 8
    { 0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },   // p - 1
    { 0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },   // p
    { 0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },   // p + 1
};

}  // namespace

bool isSmallOrder(const uint8_t publicKey[32]) {
    uint8_t hit = 0;
    for (size_t j = 0; j < 7; j++) {
        uint8_t diff = 0;
        for (size_t i = 0; i < 31; i++) diff |= (uint8_t)(publicKey[i] ^ kSmallOrder[j][i]);
        diff |= (uint8_t)((publicKey[31] & 0x7f) ^ kSmallOrder[j][31]);
        hit |= (uint8_t)(diff == 0);
    }
    return hit != 0;
}

bool claimDerive(const uint8_t pianoSecret[32], const uint8_t appPublic[32],
                 uint8_t pianoPublic[32], uint8_t key[32]) {
    if (isSmallOrder(appPublic)) {
        memset(pianoPublic, 0, 32);
        memset(key, 0, 32);
        return false;
    }
    uint8_t shared[32];
    crypto_scalarmult_base(pianoPublic, pianoSecret);
    const int rc = crypto_scalarmult(shared, pianoSecret, appPublic);   // -1 on an all-zero result
    uint8_t any = 0;
    for (size_t i = 0; i < sizeof shared; i++) any |= shared[i];        // and checked here too
    if (rc != 0 || any == 0) {
        sodium_memzero(shared, sizeof shared);
        memset(pianoPublic, 0, 32);
        memset(key, 0, 32);
        return false;
    }
    // K = SHA-256(label ‖ shared ‖ app_public ‖ piano_public), 116 bytes in.
    uint8_t in[kClaimLabelBytes + 32 + 32 + 32];
    memcpy(in, kClaimLabel, kClaimLabelBytes);
    memcpy(in + kClaimLabelBytes, shared, 32);
    memcpy(in + kClaimLabelBytes + 32, appPublic, 32);
    memcpy(in + kClaimLabelBytes + 64, pianoPublic, 32);
    crypto_hash_sha256(key, in, sizeof in);
    sodium_memzero(in, sizeof in);
    sodium_memzero(shared, sizeof shared);
    return true;
}

void authMac(const uint8_t key[32], const uint8_t nonce[16], uint8_t mac[32]) {
    uint8_t msg[kAuthLabelBytes + 16];
    memcpy(msg, kAuthLabel, kAuthLabelBytes);
    memcpy(msg + kAuthLabelBytes, nonce, 16);
    crypto_auth_hmacsha256(mac, msg, sizeof msg, key);   // a 32-byte key, as HMAC uses it
}

bool authMatches(const uint8_t (*keys)[32], size_t count, const uint8_t nonce[16],
                 const uint8_t mac[32]) {
    uint8_t ok = 0;
    uint8_t expected[32];
    for (size_t k = 0; k < count; k++) {
        authMac(keys[k], nonce, expected);
        ok |= (uint8_t)(sodium_memcmp(expected, mac, sizeof expected) == 0);   // constant-time
    }
    sodium_memzero(expected, sizeof expected);
    return ok != 0;
}

void toHex(const uint8_t *in, size_t n, char *out) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = kHex[in[i] >> 4];
        out[2 * i + 1] = kHex[in[i] & 15];
    }
    out[2 * n] = '\0';
}

bool fromHex(const char *in, uint8_t *out, size_t n) {
    if (in == NULL || strlen(in) != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        const int hi = hexValue(in[2 * i]), lo = hexValue(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

}  // namespace lockcrypto
