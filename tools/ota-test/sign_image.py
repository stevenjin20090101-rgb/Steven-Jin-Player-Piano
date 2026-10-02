#!/usr/bin/env python3
# ============================================================================
#  Player Piano - ESP32-S3 self-playing acoustic piano
#  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
#  Original author & creator: Steven Jin.
#  Licensed under the MIT License (see LICENSE). This copyright and attribution
#  notice MUST be preserved in all copies or substantial portions of the work.
#  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
# ============================================================================
# Sign a firmware image for the Bluetooth update (docs/BLE_OTA.md §10): print
# its size, SHA-256 and the base64 Ed25519 signature of the 32 digest bytes,
# then verify that signature against the repository's public key
# (provenance/author_ed25519_public.pem). This is the §10 one-liner as a tool.
#
# The private key is read from its path only; it is never printed, logged,
# copied or written anywhere. A signed test image is NOT a release: it carries
# a working signature only if you sign with the real key, and the .bin/.sig it
# produces are git-ignored.
#
#   PIANO_SIGNING_KEY=~/key.pem python3 sign_image.py firmware-2.1.0.bin
#   python3 sign_image.py --out firmware-2.1.0.sig firmware-2.1.0.bin
#
# Default key path: $PIANO_SIGNING_KEY, else ~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem.

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# tools/ota-test/ -> repository root -> provenance/
PUBKEY_PATH = os.path.normpath(os.path.join(HERE, "..", "..", "provenance", "author_ed25519_public.pem"))
DEFAULT_KEY = "~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem"


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Sign a firmware image (Ed25519 over its SHA-256) and verify "
        "the signature against provenance/author_ed25519_public.pem.",
    )
    p.add_argument("image", help="the firmware .bin to sign")
    p.add_argument(
        "--key",
        default=os.environ.get("PIANO_SIGNING_KEY", DEFAULT_KEY),
        help="the Ed25519 private key PEM (default: $PIANO_SIGNING_KEY or "
        f"{DEFAULT_KEY}); read only, never printed",
    )
    p.add_argument(
        "--pubkey",
        default=PUBKEY_PATH,
        help="the public key PEM to verify against (default: the repo's "
        "provenance/author_ed25519_public.pem)",
    )
    p.add_argument(
        "--out",
        help="also write the base64 signature to this file (git-ignored *.sig)",
    )
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        from cryptography.hazmat.primitives.serialization import (
            load_pem_private_key,
            load_pem_public_key,
        )
        from cryptography.exceptions import InvalidSignature
    except ImportError:
        print("ERROR: this tool needs 'cryptography' "
              "(pip install -r requirements.txt into .venv)", file=sys.stderr)
        return 2

    if not os.path.exists(args.image):
        print(f"ERROR: image not found: {args.image}", file=sys.stderr)
        return 2
    with open(args.image, "rb") as f:
        image = f.read()
    digest = hashlib.sha256(image).digest()

    key_path = os.path.expanduser(args.key)
    if not os.path.exists(key_path):
        # Never echo the key; its path is fine to show.
        print(f"ERROR: signing key not found at {key_path}", file=sys.stderr)
        print("Set PIANO_SIGNING_KEY to the Ed25519 private key PEM.", file=sys.stderr)
        return 2
    try:
        with open(key_path, "rb") as f:
            private_key = load_pem_private_key(f.read(), password=None)
        signature = private_key.sign(digest)  # Ed25519 over the 32 digest bytes
    except Exception as exc:
        # Deliberately generic: do not surface any key material in the message.
        print(f"ERROR: could not sign with the key at {key_path}: "
              f"{type(exc).__name__}", file=sys.stderr)
        return 2
    del private_key  # drop the key object promptly

    sig_b64 = base64.b64encode(signature).decode()

    # Verify against the repository's public key, like the §10 one-liner.
    verified = False
    verify_note = ""
    if not os.path.exists(args.pubkey):
        verify_note = f"(public key not found at {args.pubkey}; not verified)"
    else:
        try:
            with open(args.pubkey, "rb") as f:
                public_key = load_pem_public_key(f.read())
            public_key.verify(signature, digest)
            verified = True
        except InvalidSignature:
            verify_note = "(signature does NOT verify against the public key)"
        except Exception as exc:
            verify_note = f"({type(exc).__name__} while verifying)"

    print(f"image        {args.image}")
    print(f"size         {len(image)} bytes")
    print(f"sha256       {digest.hex()}")
    print(f"sig (base64) {sig_b64}")
    if verified:
        print("verify       OK — signature verifies against "
              "provenance/author_ed25519_public.pem")
    else:
        print(f"verify       FAILED {verify_note}")

    if args.out:
        with open(args.out, "w") as f:
            f.write(sig_b64 + "\n")
        print(f"wrote        {args.out}")

    if not verified:
        # A signature that does not verify against the repo key is only expected
        # when signing a test image with a throwaway key; make the non-match loud
        # but do not fail the process, so test images can still be produced.
        print("\nnote: the signature did not verify against the repository's "
              "public key.\nThat is expected for a test image signed with a key "
              "other than the author key;\nthe real piano will reject it (ERR 6).",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
