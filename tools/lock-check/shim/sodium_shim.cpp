// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The libsodium stand-in of sodium.h, on OpenSSL 3's libcrypto. Host only.
#include "sodium.h"
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

extern "C" {

int crypto_scalarmult_base(unsigned char *q, const unsigned char *n) {
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, n, 32);
    size_t len = 32;
    const int ok = k != NULL && EVP_PKEY_get_raw_public_key(k, q, &len) == 1 && len == 32;
    EVP_PKEY_free(k);
    return ok ? 0 : -1;
}

int crypto_scalarmult(unsigned char *q, const unsigned char *n, const unsigned char *p) {
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, n, 32);
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, p, 32);
    EVP_PKEY_CTX *ctx = mine ? EVP_PKEY_CTX_new(mine, NULL) : NULL;
    size_t len = 32;
    int ok = ctx != NULL && peer != NULL && EVP_PKEY_derive_init(ctx) == 1 &&
             EVP_PKEY_derive_set_peer(ctx, peer) == 1 && EVP_PKEY_derive(ctx, q, &len) == 1 && len == 32;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    EVP_PKEY_free(mine);
    if (ok) {                                   // libsodium: an all-zero result is an error
        unsigned char d = 0;
        for (int i = 0; i < 32; i++) d |= q[i];
        ok = d != 0;
    }
    if (!ok) memset(q, 0, 32);                  // OpenSSL refuses all-zero itself; same answer
    return ok ? 0 : -1;
}

int crypto_hash_sha256(unsigned char *out, const unsigned char *in, unsigned long long inlen) {
    unsigned int len = 0;
    return EVP_Digest(in, (size_t)inlen, out, &len, EVP_sha256(), NULL) == 1 && len == 32 ? 0 : -1;
}

int crypto_auth_hmacsha256(unsigned char *out, const unsigned char *in, unsigned long long inlen,
                           const unsigned char *k) {
    unsigned int len = 0;
    return HMAC(EVP_sha256(), k, 32, in, (size_t)inlen, out, &len) != NULL && len == 32 ? 0 : -1;
}

int sodium_memcmp(const void *b1, const void *b2, size_t len) {
    return CRYPTO_memcmp(b1, b2, len) == 0 ? 0 : -1;
}

void sodium_memzero(void *pnt, size_t len) { OPENSSL_cleanse(pnt, len); }

}  // extern "C"
