# ============================================================================
#  Player Piano - ESP32-S3 self-playing acoustic piano
#  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
#  Original author & creator: Steven Jin.
#  Licensed under the MIT License (see LICENSE). This copyright and attribution
#  notice MUST be preserved in all copies or substantial portions of the work.
#  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
# ============================================================================
"""The app's side of the Bluetooth lock (docs/BLE_LOCK.md) in Python, against the
firmware's own derivation (src/lock_crypto.cpp, built as ./lockcheck).

    make check            (runs: ~/.platformio/penv/bin/python check.py ./lockcheck)

Checks: RFC 7748's X25519 vector through the claim; 200 random claims (piano
public key and K); the auth HMAC over random nonces; matching against 1..4 keys
and refusing a wrong MAC; small-order app keys refused; strict hex. Then prints
the fixed vector the firmware's USB `lock selftest` checks on the device.
"""
import hashlib
import hmac
import os
import subprocess
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

CLAIM_LABEL = b"steven-piano-lock-v1"
AUTH_LABEL = b"steven-piano-auth-v1"

# RFC 7748 section 6.1: Alice plays the app, Bob the piano.
ALICE_SK = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
ALICE_PK = bytes.fromhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
BOB_SK = bytes.fromhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
BOB_PK = bytes.fromhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
RFC_SHARED = bytes.fromhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
SELFTEST_NONCE = bytes(range(16))

# Small-order and non-canonical points whose X25519 result is all zero.
SMALL_ORDER = [
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0100000000000000000000000000000000000000000000000000000000000000",
    "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
    "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
]

FAILS = []


def expect(ok, what):
    if not ok:
        FAILS.append(what)
        print("  FAIL", what)


def run(binary, *args):
    out = subprocess.run([binary, *args], capture_output=True, text=True, check=True).stdout
    return out.strip()


def fields(text):
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def pub_of(sk):
    return X25519PrivateKey.from_private_bytes(sk).public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def app_key(app_sk, piano_pk):
    """The app's K: X25519(app secret, piano public), then the same SHA-256 the piano takes."""
    shared = X25519PrivateKey.from_private_bytes(app_sk).exchange(X25519PublicKey.from_public_bytes(piano_pk))
    return hashlib.sha256(CLAIM_LABEL + shared + pub_of(app_sk) + piano_pk).digest()


def app_mac(key, nonce):
    return hmac.new(key, AUTH_LABEL + nonce, hashlib.sha256).digest()


def main(binary):
    # RFC 7748 through the claim: the piano's public key and the shared secret.
    expect(pub_of(BOB_SK) == BOB_PK and pub_of(ALICE_SK) == ALICE_PK, "Python reproduces RFC 7748's public keys")
    f = fields(run(binary, "claim", BOB_SK.hex(), ALICE_PK.hex()))
    expect(f.get("pianoPublic") == BOB_PK.hex(), "claim: the piano's public key is RFC 7748's (Bob)")
    rfc_key = hashlib.sha256(CLAIM_LABEL + RFC_SHARED + ALICE_PK + BOB_PK).digest()
    expect(f.get("key") == rfc_key.hex(), "claim: K = SHA-256(label ‖ RFC shared ‖ app pub ‖ piano pub)")
    expect(app_key(ALICE_SK, BOB_PK) == rfc_key, "the app side derives the same K")

    # Random claims and auths.
    n_claims = 200
    for i in range(n_claims):
        app_sk, piano_sk, nonce = os.urandom(32), os.urandom(32), os.urandom(16)
        f = fields(run(binary, "claim", piano_sk.hex(), pub_of(app_sk).hex()))
        piano_pk = bytes.fromhex(f["pianoPublic"])
        expect(piano_pk == pub_of(piano_sk), f"claim {i}: piano public key")
        key = app_key(app_sk, piano_pk)
        expect(f["key"] == key.hex(), f"claim {i}: K")
        mac = app_mac(key, nonce)
        expect(fields(run(binary, "mac", key.hex(), nonce.hex()))["mac"] == mac.hex(), f"auth {i}: HMAC")
        if i < 40:
            others = [os.urandom(32).hex() for _ in range(i % 4)]
            at = i % (len(others) + 1)
            keys = others[:at] + [key.hex()] + others[at:]
            expect(run(binary, "match", nonce.hex(), mac.hex(), *keys) == "match", f"auth {i}: key {at + 1} of {len(keys)} matches")
            bad = bytearray(mac)
            bad[i % 32] ^= 1 << (i % 8)
            expect(run(binary, "match", nonce.hex(), bytes(bad).hex(), *keys) == "nomatch", f"auth {i}: a flipped bit is refused")
            expect(run(binary, "match", os.urandom(16).hex(), mac.hex(), *keys) == "nomatch", f"auth {i}: another nonce is refused")
    expect(run(binary, "match", SELFTEST_NONCE.hex(), "00" * 32) == "nomatch", "no keys: nothing matches")

    # Small-order app keys: an all-zero shared secret, refused with zeroed outputs —
    # and recognised by the list before any X25519 runs (with the top bit set too).
    for p in SMALL_ORDER:
        expect(run(binary, "claim", os.urandom(32).hex(), p) == "refused", f"small-order app key {p[:16]}…: refused")
        high = bytearray(bytes.fromhex(p))
        high[31] |= 0x80
        expect(run(binary, "smallorder", p) == "yes", f"small-order list holds {p[:16]}…")
        expect(run(binary, "smallorder", bytes(high).hex()) == "yes", f"small-order list holds {p[:16]}… with the top bit set")
        expect(run(binary, "claim", os.urandom(32).hex(), bytes(high).hex()) == "refused", f"{p[:16]}… with the top bit: refused")
        try:
            X25519PrivateKey.generate().exchange(X25519PublicKey.from_public_bytes(bytes.fromhex(p)))
            python_refuses = False
        except Exception:
            python_refuses = True
        expect(python_refuses, f"(Python refuses {p[:16]}… too)")
    for _ in range(50):
        expect(run(binary, "smallorder", pub_of(os.urandom(32)).hex()) == "no", "a real public key is not small-order")
    expect(run(binary, "smallorder", ALICE_PK.hex()) == "no", "RFC 7748's Alice is not small-order")

    # Strict hex: exactly 2n digits, either case.
    expect(run(binary, "fromhex", "ab" * 32, "32") == "ok=" + "ab" * 32, "64 hex digits: ok")
    expect(run(binary, "fromhex", "AB" * 32, "32") == "ok=" + "ab" * 32, "upper case: same bytes")
    for bad in ["ab" * 31 + "a", "ab" * 32 + "a", "ab" * 31 + "ag", "ab" * 31 + " a", ""]:
        expect(run(binary, "fromhex", bad, "32") == "bad", f"refused: {bad[-4:]!r} ({len(bad)} chars)")

    # The vector the device checks with `lock selftest` (USB).
    st_key = rfc_key
    st_mac = app_mac(st_key, SELFTEST_NONCE)
    print("device selftest vector (src/ble_lock.cpp, `lock selftest`):")
    print("  piano secret  ", BOB_SK.hex())
    print("  app public    ", ALICE_PK.hex())
    print("  piano public  ", BOB_PK.hex())
    print("  K             ", st_key.hex())
    print("  nonce         ", SELFTEST_NONCE.hex())
    print("  auth          ", st_mac.hex())
    print(f"  bench: `auth {st_mac.hex()}` is what an app holding K would send for !nonce={SELFTEST_NONCE.hex()}")

    if FAILS:
        print(f"LOCK CHECK: FAILED ({len(FAILS)} checks)")
        return 1
    print(f"LOCK CHECK: OK — RFC 7748 claim, {n_claims} random claims + auths, 40 x 3 match cases, "
          f"{len(SMALL_ORDER)} small-order keys (x2 top bit) listed and refused, strict hex")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "./lockcheck"))
