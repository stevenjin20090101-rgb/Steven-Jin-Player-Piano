// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The Bluetooth lock's cryptography (docs/BLE_LOCK.md, "Protocol"), as pure
// functions over libsodium: no Arduino, NimBLE, NVS or randomness here, so the
// same file compiles on the Mac (tools/lock-check proves it against the app
// side written in Python) and on the ESP32 (src/ble_lock.cpp uses it).
//
//   claim:  shared = X25519(piano_secret, app_public)   (all-zero → refused)
//           K = SHA-256("steven-piano-lock-v1" ‖ shared ‖ app_public ‖ piano_public)
//   auth:   HMAC-SHA256(K, "steven-piano-auth-v1" ‖ nonce[16])
//
// Every secret on the stack is wiped with sodium_memzero() before returning.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace lockcrypto {

static const size_t kKeyBytes   = 32;   // K, and every X25519 key
static const size_t kNonceBytes = 16;
static const size_t kMacBytes   = 32;

// True for the X25519 public keys whose shared secret is all zero whatever
// the secret: libsodium's list of small-order points (0, 1, the two of order
// 8, p - 1, p, p + 1), the top bit ignored as X25519 ignores it. Constant
// time. Lets the piano refuse such a key before spending any X25519 time.
bool isSmallOrder(const uint8_t publicKey[32]);

// The piano's side of a claim. `pianoSecret` is 32 fresh random bytes (X25519
// clamps them). Writes the piano's public key and K. False — and both outputs
// zeroed — when the app key is small-order or the shared secret is all zero.
bool claimDerive(const uint8_t pianoSecret[32], const uint8_t appPublic[32],
                 uint8_t pianoPublic[32], uint8_t key[32]);

// HMAC-SHA256(key, "steven-piano-auth-v1" ‖ nonce).
void authMac(const uint8_t key[32], const uint8_t nonce[16], uint8_t mac[32]);

// True if `mac` is authMac(k, nonce) for any of the `count` keys. Every key
// is tried and each comparison is constant-time, whatever matches.
bool authMatches(const uint8_t (*keys)[32], size_t count, const uint8_t nonce[16],
                 const uint8_t mac[32]);

// Lowercase hex of `n` bytes into `out` (2n characters and a NUL).
void toHex(const uint8_t *in, size_t n, char *out);

// Exactly 2n hex digits (either case) and nothing else → `n` bytes; else false.
bool fromHex(const char *in, uint8_t *out, size_t n);

}  // namespace lockcrypto
