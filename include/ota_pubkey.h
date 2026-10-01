// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The Ed25519 public key every firmware update must be signed with
// (docs/BLE_OTA.md section 8): the author key of provenance/author_ed25519_public.pem,
// fingerprint eab16a502f679465 (the first 16 hex digits of the SHA-256 of these
// 32 bytes, as PROVENANCE.md states). A BEGIN or an image whose signature does
// not verify against it is refused (ERR 6) and never activated.
//
// tools/publish-firmware.sh refuses to release unless these bytes are the PEM's.
// Regenerate them, from firmware/:
// ~/.platformio/penv/bin/python -c 'from cryptography.hazmat.primitives.serialization import load_pem_public_key as L, Encoding as E, PublicFormat as F; r=L(open("provenance/author_ed25519_public.pem","rb").read()).public_bytes(E.Raw,F.Raw); print(", ".join("0x%02x" % b for b in r))'
#pragma once
#include <stdint.h>

static const uint8_t kOtaPublicKey[32] = {
    0x94, 0xa0, 0xfc, 0x32, 0x52, 0x03, 0xab, 0x51, 0xdc, 0x28, 0xfb, 0xff, 0xc6, 0xc9, 0xce, 0x0f,
    0x9c, 0xda, 0x8f, 0x37, 0x60, 0xdc, 0xc0, 0x07, 0xb5, 0x6a, 0x4b, 0x65, 0xf8, 0x8f, 0x03, 0x19,
};
