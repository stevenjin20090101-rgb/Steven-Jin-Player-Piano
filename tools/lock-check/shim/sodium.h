// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Host stand-in for the six libsodium calls src/lock_crypto.cpp makes, with
// libsodium's signatures and return conventions, implemented on OpenSSL's
// libcrypto (sodium_shim.cpp). Used only by tools/lock-check on the Mac, where
// libsodium is not installed; the firmware links the SDK's real libsodium.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// X25519 (RFC 7748, scalar clamped). crypto_scalarmult returns -1 when the
// result is all zero (a small-order point), as libsodium does; 0 otherwise.
int  crypto_scalarmult_base(unsigned char *q, const unsigned char *n);
int  crypto_scalarmult(unsigned char *q, const unsigned char *n, const unsigned char *p);
int  crypto_hash_sha256(unsigned char *out, const unsigned char *in, unsigned long long inlen);
int  crypto_auth_hmacsha256(unsigned char *out, const unsigned char *in, unsigned long long inlen,
                            const unsigned char *k);   // k: 32 bytes
int  sodium_memcmp(const void *b1, const void *b2, size_t len);   // 0 equal, -1 not; constant time
void sodium_memzero(void *pnt, size_t len);

#ifdef __cplusplus
}
#endif
