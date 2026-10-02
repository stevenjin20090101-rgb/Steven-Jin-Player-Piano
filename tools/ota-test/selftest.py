#!/usr/bin/env python3
# ============================================================================
#  Player Piano - ESP32-S3 self-playing acoustic piano
#  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
#  Original author & creator: Steven Jin.
#  Licensed under the MIT License (see LICENSE). This copyright and attribution
#  notice MUST be preserved in all copies or substantial portions of the work.
#  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
# ============================================================================
# Self-test for the bench tools: runs with NO radio and NO piano, and exits 0
# only if everything matches docs/BLE_OTA.md. It checks the pure parts of the
# tools against the spec's own numbers so a mistake is caught here, long before
# the bench:
#   * the 118 BEGIN bytes of §5's worked example, reproduced exactly;
#   * its SHA-256 and (with cryptography) its Ed25519 signature;
#   * READY / ACK / OK / ERR / VERIFYING / ABORTED parsing against §5's hex;
#   * Data frames 0, 1 and 3964, and the 3,965-frame / 232-byte-last count;
#   * the §10 example manifest's signature (with cryptography);
#   * BLE-MIDI framing of one NoteOn;
#   * console line assembly across split notifications (a multi-byte UTF-8
#     character split in two, a \r\n ending, the "> " echo dropped);
#   * each fault flag of ota_client building the frame it should.
#
# Without the 'cryptography' module the two signature checks are skipped (and
# said so, with how to install it); everything else still runs.
#
#   .venv/bin/python selftest.py

from __future__ import annotations

import base64
import hashlib
import sys

import piano_ble as pb
import ota_client

# docs/BLE_OTA.md §5 / §10 worked example.
IMAGE_SIZE = 991_232
VERSION = "2.1.0"
WINDOW = 16
SHA256_HEX = "6d872b9645d71995871a346d906e64c1b4ea4d92dd7f88cccea75f2792b212c6"
SIG_B64 = "J3gBHESXwswpxAhbRmiZDS5Y8YbLk16zxgRBJvCaeZO7OOxPPGPmBjuBkoh0v3ytKS/w9PeTDJNBjoe4yi3mCg=="

# RFC 8032 §7.1 TEST 1 (a published test vector; its secret key is in the RFC).
RFC_SECRET = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
RFC_PUBLIC = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")

# §5's BEGIN, byte for byte (the worked example's hex dump).
BEGIN_DOC_HEX = """
01 00 20 0F 00 6D 87 2B 96 45 D7 19 95 87 1A 34
6D 90 6E 64 C1 B4 EA 4D 92 DD 7F 88 CC CE A7 5F
27 92 B2 12 C6 27 78 01 1C 44 97 C2 CC 29 C4 08
5B 46 68 99 0D 2E 58 F1 86 CB 93 5E B3 C6 04 41
26 F0 9A 79 93 BB 38 EC 4F 3C 63 E6 06 3B 81 92
88 74 BF 7C AD 29 2F F0 F4 F7 93 0C 93 41 8E 87
B8 CA 2D E6 0A 32 2E 31 2E 30 00 00 00 00 00 00
00 00 00 00 00 10
"""


def hexbytes(text: str) -> bytes:
    return bytes(int(x, 16) for x in text.split())


def popcount(data: bytes) -> int:
    return sum(bin(b).count("1") for b in data)


def xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


# ---------------------------------------------------------------------------
# A tiny test harness: each test returns (ok, detail) or raises.
# ---------------------------------------------------------------------------


class Runner:
    def __init__(self) -> None:
        self.failures = 0
        self.skipped = 0

    def check(self, name: str, fn) -> None:
        try:
            ok, detail = fn()
        except Exception as exc:  # a thrown test is a failure
            ok, detail = False, f"raised {type(exc).__name__}: {exc}"
        mark = "ok  " if ok else "FAIL"
        if not ok:
            self.failures += 1
        print(f"  [{mark}] {name}: {detail}")

    def skip(self, name: str, why: str) -> None:
        self.skipped += 1
        print(f"  [skip] {name}: {why}")


# Build the worked-example image and frames once.
def worked_image() -> bytes:
    return bytes(((0xE9 + 7 * i) & 0xFF) for i in range(IMAGE_SIZE))


IMAGE = worked_image()
DIGEST = hashlib.sha256(IMAGE).digest()
SIG = base64.b64decode(SIG_B64)
BEGIN_DOC = hexbytes(BEGIN_DOC_HEX)


# ---- pure tests (stdlib only) ----------------------------------------------


def t_image_sha():
    ok = DIGEST.hex() == SHA256_HEX
    return ok, f"SHA-256 of the generated image {'=' if ok else '!='} the document's"


def t_begin_bytes():
    begin = pb.begin_frame(IMAGE_SIZE, DIGEST, SIG, VERSION, WINDOW)
    ok = len(begin) == 118 and begin == BEGIN_DOC
    return ok, ("118 bytes reproduce §5's BEGIN exactly" if ok
                else f"built BEGIN differs from the document ({pb.hexs(begin[:8])}...)")


def t_data0():
    f = pb.data_frame(0, IMAGE[0:250])
    ok = (len(f) == 252 and f[:8] == hexbytes("00 00 E9 F0 F7 FE 05 0C")
          and f[-2:] == hexbytes("B1 B8"))
    return ok, f"Data 0 = {pb.hexs(f[:8])} ... {pb.hexs(f[-2:])} ({len(f)} B)"


def t_data1():
    f = pb.data_frame(1, IMAGE[250:500])
    ok = f[:4] == hexbytes("01 00 BF C6")
    return ok, f"Data 1 = {pb.hexs(f[:4])} ..."


def t_data_last():
    off = 3964 * 250
    f = pb.data_frame(3964, IMAGE[off:IMAGE_SIZE])
    ok = (len(f) == 234 and f[:8] == hexbytes("7C 0F 91 98 9F A6 AD B4")
          and f[-2:] == hexbytes("DB E2"))
    return ok, f"Data 3964 = {pb.hexs(f[:8])} ... {pb.hexs(f[-2:])} ({len(f)} B)"


def t_frame_count():
    frames = (IMAGE_SIZE + 249) // 250
    last = IMAGE_SIZE - (frames - 1) * 250
    ok = frames == 3965 and last == 232
    return ok, f"{frames} frames, last payload {last} bytes"


def t_parse():
    cases = {
        "81 FA 00 10": ("READY", dict(max_chunk=250, window=16)),
        "85 A0 0F 00 00": ("ACK", dict(bytes_received=4000)),
        "85 60 13 0F 00": ("ACK", dict(bytes_received=988000)),
        "85 00 20 0F 00": ("ACK", dict(bytes_received=IMAGE_SIZE)),
        "82": ("VERIFYING", {}),
        "83 DC 05": ("OK", dict(restart_ms=1500)),
        "84": ("ABORTED", {}),
        "E1 05": ("ERR", dict(code=5)),
        "E1 06": ("ERR", dict(code=6)),
    }
    for hexstr, (name, fields) in cases.items():
        ev = pb.parse_control(hexbytes(hexstr))
        if ev.malformed or ev.name != name:
            return False, f"{hexstr} parsed as {ev.name} (malformed={ev.malformed})"
        for key, want in fields.items():
            if getattr(ev, key) != want:
                return False, f"{hexstr}: {key}={getattr(ev, key)}, want {want}"
    # Error names.
    if pb.ERROR_NAMES[5] != "hash mismatch" or pb.ERROR_NAMES[6] != "bad signature":
        return False, "error names wrong"
    # A known opcode of the wrong length is malformed.
    if not pb.parse_control(hexbytes("85 00")).malformed:
        return False, "a short ACK was not flagged malformed"
    return True, "READY/ACK/VERIFYING/OK/ABORTED/ERR all parse to §5's values"


def t_midi():
    f = pb.midi_note_on(60, 100)
    ok = f == hexbytes("80 80 90 3C 64")
    return ok, f"NoteOn(60,100) = {pb.hexs(f)}"


def t_console_assembly():
    emitted = []
    a = pb.ConsoleLineAssembler()
    # "> dump" echo (dropped), then "  pedal — up\r\n" with the em-dash
    # (E2 80 94) split across two notifications, then "!proto=1\n".
    a.feed(b"> dump\n  pedal \xe2", emitted.append)
    a.feed(b"\x80\x94 up\r\n!proto=1\n", emitted.append)
    ok = emitted == ["  pedal — up", "!proto=1"]
    return ok, f"emitted {emitted!r}"


# ---- fault-frame tests (stdlib only; use ota_client.build_plan) -------------


def base_plan(**kw):
    return ota_client.build_plan(IMAGE, DIGEST, SIG, version=VERSION, window=WINDOW, **kw)


def t_clean_plan():
    p = base_plan()
    ok = (p.begin == BEGIN_DOC and p.sig == SIG and p.sha256 == DIGEST
          and p.image == IMAGE)
    return ok, "no faults: BEGIN == the worked example, nothing tampered"


def t_flip_sig():
    p = base_plan(flip_sig_bit=True)
    d = popcount(xor(p.sig, SIG))
    ok = d == 1 and len(p.sig) == 64 and p.sha256 == DIGEST
    return ok, f"--flip-sig-bit changes exactly {d} bit of the 64"


def t_flip_sha():
    p = base_plan(flip_sha_bit=True)
    d = popcount(xor(p.sha256, DIGEST))
    ok = d == 1 and p.sig == SIG
    return ok, f"--flip-sha-bit changes exactly {d} bit of the 32"


def t_flip_image():
    p = base_plan(flip_image_byte=True)
    diff = [i for i in range(len(IMAGE)) if p.image[i] != IMAGE[i]]
    ok = (len(diff) == 1 and diff[0] != 0 and p.image[0] == 0xE9
          and p.sha256 == DIGEST and p.sig == SIG)
    where = diff[0] if diff else "none"
    return ok, (f"--flip-image-byte changes 1 byte (offset {where}), keeps the "
                "magic and BEGIN's sha/sig")


def t_claim_size():
    p = base_plan(claim_size=7_000_000)
    size_field = int.from_bytes(p.begin[1:5], "little")
    ok = size_field == 7_000_000 and p.image == IMAGE
    return ok, f"--claim-size puts {size_field} in BEGIN, leaves the image"


def t_first_seq():
    p = base_plan(first_seq=5)
    ok = p.first_seq == 5
    return ok, "--first-seq N recorded (the client sends seq N first)"


def t_begin_twice():
    p = base_plan(begin_twice=True)
    return p.begin_twice, "--begin-twice recorded"


def t_after_flags():
    p = base_plan(abort_after=20, disconnect_after=3, stall_after=5)
    ok = p.abort_after == 20 and p.disconnect_after == 3 and p.stall_after == 5
    return ok, "--abort-after / --disconnect-after / --stall-after recorded"


# ---- crypto tests (need 'cryptography') ------------------------------------


def t_sign_matches():
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    key = Ed25519PrivateKey.from_private_bytes(RFC_SECRET)
    sig = key.sign(DIGEST)
    ok = base64.b64encode(sig).decode() == SIG_B64
    return ok, ("signing the digest with RFC 8032 TEST 1's key reproduces §5's "
                "signature" if ok else "the signature does not match the document")


def t_manifest_sig():
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature

    pub = Ed25519PublicKey.from_public_bytes(RFC_PUBLIC)
    try:
        pub.verify(base64.b64decode(SIG_B64), bytes.fromhex(SHA256_HEX))
        return True, "§10's example manifest sig verifies against the RFC test key"
    except InvalidSignature:
        return False, "the manifest signature does not verify"


def t_rfc_pubkey():
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

    key = Ed25519PrivateKey.from_private_bytes(RFC_SECRET)
    raw = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    ok = raw == RFC_PUBLIC
    return ok, "RFC 8032 TEST 1 secret -> its published public key"


def main() -> int:
    if any(a in ("-h", "--help") for a in sys.argv[1:]):
        print("usage: selftest.py\n\n"
              "Run the ota-test self-test: no radio, no piano. Exits 0 only if "
              "every check matches docs/BLE_OTA.md. The two Ed25519 signature "
              "checks need the 'cryptography' module; the rest are stdlib only.")
        return 0
    print("ota-test self-test (no radio, no piano)\n")
    r = Runner()

    print("Frames and parsing (docs/BLE_OTA.md §5):")
    r.check("image SHA-256", t_image_sha)
    r.check("BEGIN 118 bytes", t_begin_bytes)
    r.check("Data frame 0", t_data0)
    r.check("Data frame 1", t_data1)
    r.check("Data frame 3964", t_data_last)
    r.check("frame count", t_frame_count)
    r.check("Control parsing", t_parse)

    print("\nBLE-MIDI and console (docs/BLE_OTA.md §13, BLE_SETTINGS.md §3):")
    r.check("BLE-MIDI NoteOn", t_midi)
    r.check("console line assembly", t_console_assembly)

    print("\nFault flags build the right frames (ota_client, docs/BLE_OTA.md §13):")
    r.check("no-fault plan", t_clean_plan)
    r.check("--flip-sig-bit", t_flip_sig)
    r.check("--flip-sha-bit", t_flip_sha)
    r.check("--flip-image-byte", t_flip_image)
    r.check("--claim-size", t_claim_size)
    r.check("--first-seq", t_first_seq)
    r.check("--begin-twice", t_begin_twice)
    r.check("--abort/disconnect/stall-after", t_after_flags)

    print("\nSignatures (docs/BLE_OTA.md §8, §10):")
    try:
        import cryptography  # noqa: F401

        r.check("RFC 8032 secret -> public", t_rfc_pubkey)
        r.check("sign digest == document", t_sign_matches)
        r.check("§10 manifest sig verifies", t_manifest_sig)
    except ImportError:
        r.skip("Ed25519 signature checks", "the 'cryptography' module is not "
               "installed (pip install -r requirements.txt into .venv)")

    print()
    if r.failures:
        print(f"FAILED: {r.failures} check(s) did not match the specification.")
        return 1
    if r.skipped:
        print(f"OK (pure checks passed), but {r.skipped} group(s) were skipped "
              "for a missing module — install cryptography to run them.")
        return 0
    print("OK: every check matches docs/BLE_OTA.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
