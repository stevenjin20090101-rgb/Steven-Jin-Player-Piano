// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// A command line around the REAL src/lock_crypto.cpp (the firmware's claim and
// auth derivation), compiled on the Mac with the device's language settings.
// check.py plays the app's side in Python and compares. Every argument and
// result is hex.
//
//   lockcheck claim <piano secret> <app public>    pianoPublic=… and key=…, or "refused"
//   lockcheck mac <key> <nonce>                    mac=…
//   lockcheck match <nonce> <mac> [key …]          "match" or "nomatch"
//   lockcheck fromhex <text> <bytes>               "ok=<hex>" or "bad"
//   lockcheck smallorder <public key>              "yes" or "no"
#include "lock_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool arg(const char *text, uint8_t *out, size_t n) {
    if (!lockcrypto::fromHex(text, out, n)) {
        fprintf(stderr, "bad hex argument (want %zu bytes): %s\n", n, text);
        return false;
    }
    return true;
}

static void print(const char *name, const uint8_t *b, size_t n) {
    char h[2 * 64 + 1];
    lockcrypto::toHex(b, n, h);
    printf("%s=%s\n", name, h);
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "claim")) {
        uint8_t sk[32], app[32], pub[32], key[32];
        if (!arg(argv[2], sk, 32) || !arg(argv[3], app, 32)) return 2;
        if (!lockcrypto::claimDerive(sk, app, pub, key)) {
            uint8_t z = 0;
            for (int i = 0; i < 32; i++) z |= pub[i] | key[i];
            printf("refused%s\n", z ? " (outputs NOT zeroed)" : "");
            return 0;
        }
        print("pianoPublic", pub, 32);
        print("key", key, 32);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "mac")) {
        uint8_t key[32], nonce[16], mac[32];
        if (!arg(argv[2], key, 32) || !arg(argv[3], nonce, 16)) return 2;
        lockcrypto::authMac(key, nonce, mac);
        print("mac", mac, 32);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "match")) {
        uint8_t nonce[16], mac[32], keys[8][32];
        if (!arg(argv[2], nonce, 16) || !arg(argv[3], mac, 32)) return 2;
        size_t count = 0;
        for (int i = 4; i < argc && count < 8; i++, count++) {
            if (!arg(argv[i], keys[count], 32)) return 2;
        }
        printf("%s\n", lockcrypto::authMatches(keys, count, nonce, mac) ? "match" : "nomatch");
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "fromhex")) {
        const size_t n = (size_t)atoi(argv[3]);
        uint8_t out[64];
        if (n > sizeof out) return 2;
        if (lockcrypto::fromHex(argv[2], out, n)) print("ok", out, n);
        else printf("bad\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "smallorder")) {
        uint8_t pub[32];
        if (!arg(argv[2], pub, 32)) return 2;
        printf("%s\n", lockcrypto::isSmallOrder(pub) ? "yes" : "no");
        return 0;
    }
    fprintf(stderr, "usage: lockcheck claim|mac|match|fromhex|smallorder …\n");
    return 2;
}
