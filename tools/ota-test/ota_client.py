#!/usr/bin/env python3
# ============================================================================
#  Player Piano - ESP32-S3 self-playing acoustic piano
#  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
#  Original author & creator: Steven Jin.
#  Licensed under the MIT License (see LICENSE). This copyright and attribution
#  notice MUST be preserved in all copies or substantial portions of the work.
#  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
# ============================================================================
# The Bluetooth firmware-update test client (docs/BLE_OTA.md §13). Finds
# "Steven Piano", subscribes to Control, writes BEGIN with response, streams
# Data a window at a time waiting for each ACK, sends END, waits for OK or ERR,
# and after OK waits for the link to drop, reconnects, and re-reads the version.
# Every notification is printed as hex with a millisecond timestamp and a
# decoded meaning; throughput is printed at the end.
#
# The fault flags of §13 (flip a byte / a SHA bit / a signature bit, abort,
# disconnect, stall, a second BEGIN, a wrong first sequence, a claimed size, a
# BEGIN too soon after a restart, MIDI notes before BEGIN) build the frames the
# acceptance table expects. --expect names the outcome so the process exits 0
# only on that outcome; the README's table uses it.
#
# NO TOOL SOUNDS A KEY except --notes-then-begin, which deliberately plays one
# note on the MIDI characteristic to test the 500 ms quiet rule (ERR 1): "a key
# will sound". Everything else is silent by contract.
#
#   .venv/bin/python ota_client.py --image fw.bin --version 2.0.1 --expect ok
#   .venv/bin/python ota_client.py --image fw.bin --flip-image-byte --expect err:5
# See the README's table for the full run-to-command mapping.

from __future__ import annotations

import argparse
import asyncio
import hashlib
import os
import sys
import time
from dataclasses import dataclass, field
from typing import List, Optional

import piano_ble as pb

DEFAULT_KEY = "~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem"

# Positions the fault flags flip. Deterministic, so a run is reproducible and
# the self-test can check them. The image byte is NOT the magic byte 0 (which
# would give ERR 8, image invalid), but a middle byte, so a flip is a hash
# mismatch (ERR 5) as §13 run 1 expects.
FLIP_SHA_POS = 0
FLIP_SIG_POS = 0


# ---------------------------------------------------------------------------
# Pure planning: build the frames a run sends. No radio, so the self-test can
# check every fault builds the right frame.
# ---------------------------------------------------------------------------


@dataclass
class Plan:
    image: bytes               # the bytes actually transmitted (possibly tampered)
    begin: bytes               # the 118-byte BEGIN frame
    sha256: bytes              # the SHA field placed in BEGIN
    sig: bytes                 # the signature field placed in BEGIN
    version: str
    window: int
    claimed_size: int          # the size field placed in BEGIN
    first_seq: int
    begin_twice: bool
    abort_after: Optional[int]
    disconnect_after: Optional[int]
    stall_after: Optional[int]
    notes_then_begin_ms: Optional[int]
    begin_within_ms: Optional[int]
    expect: Optional[str]
    # untampered references, for diffing / display
    base_image: bytes
    base_sha256: bytes
    base_sig: bytes
    flipped_image_pos: Optional[int] = None
    notes: List[str] = field(default_factory=list)


def build_plan(
    image: bytes,
    sha256: bytes,
    sig: bytes,
    *,
    version: str,
    window: int = 16,
    flip_image_byte: bool = False,
    flip_sha_bit: bool = False,
    flip_sig_bit: bool = False,
    claim_size: Optional[int] = None,
    first_seq: int = 0,
    begin_twice: bool = False,
    abort_after: Optional[int] = None,
    disconnect_after: Optional[int] = None,
    stall_after: Optional[int] = None,
    notes_then_begin_ms: Optional[int] = None,
    begin_within_ms: Optional[int] = None,
    expect: Optional[str] = None,
) -> Plan:
    """Apply the §13 fault flags to a (image, sha256, sig) and return the Plan.
    Pure: builds the BEGIN frame and the transmitted image; sends nothing."""
    base_image = bytes(image)
    base_sha = bytes(sha256)
    base_sig = bytes(sig)

    out_image = bytearray(base_image)
    flipped_pos = None
    if flip_image_byte:
        # A middle byte, never the 0xE9 magic at index 0.
        flipped_pos = len(out_image) // 2 if len(out_image) > 1 else 0
        out_image[flipped_pos] ^= 0x01

    out_sha = bytearray(base_sha)
    if flip_sha_bit:
        out_sha[FLIP_SHA_POS] ^= 0x01

    out_sig = bytearray(base_sig)
    if flip_sig_bit:
        out_sig[FLIP_SIG_POS] ^= 0x01

    claimed = claim_size if claim_size is not None else len(base_image)
    begin = pb.begin_frame(claimed, bytes(out_sha), bytes(out_sig), version, window)

    notes: List[str] = []
    if flip_image_byte:
        notes.append(f"flipped image byte at offset {flipped_pos} (expect ERR 5 after END)")
    if flip_sha_bit:
        notes.append("flipped one bit of the SHA field (expect ERR 6 at BEGIN)")
    if flip_sig_bit:
        notes.append("flipped one bit of the signature (expect ERR 6 at BEGIN)")
    if claim_size is not None:
        notes.append(f"BEGIN claims size {claimed} (image is {len(base_image)} bytes)")
    if first_seq:
        notes.append(f"first Data seq is {first_seq}, not 0 (expect ERR 4)")
    if begin_twice:
        notes.append("a second BEGIN follows the first (expect ERR 3)")

    return Plan(
        image=bytes(out_image),
        begin=begin,
        sha256=bytes(out_sha),
        sig=bytes(out_sig),
        version=version,
        window=window,
        claimed_size=claimed,
        first_seq=first_seq,
        begin_twice=begin_twice,
        abort_after=abort_after,
        disconnect_after=disconnect_after,
        stall_after=stall_after,
        notes_then_begin_ms=notes_then_begin_ms,
        begin_within_ms=begin_within_ms,
        expect=expect,
        base_image=base_image,
        base_sha256=base_sha,
        base_sig=base_sig,
        flipped_image_pos=flipped_pos,
        notes=notes,
    )


def sign_digest(digest: bytes, key_path: str) -> bytes:
    """Ed25519-sign the 32 digest bytes with the key at key_path (never printed)."""
    from cryptography.hazmat.primitives.serialization import load_pem_private_key

    with open(os.path.expanduser(key_path), "rb") as f:
        key = load_pem_private_key(f.read(), password=None)
    sig = key.sign(digest)
    del key
    return sig


def load_image_and_signature(args) -> tuple:
    """Read the image, compute its SHA-256, and resolve the signature from
    --sig or by signing with the key. Returns (image, sha256, sig)."""
    with open(args.image, "rb") as f:
        image = f.read()
    if not image:
        raise SystemExit("ota_client: the image is empty")
    if image[0] != 0xE9 and not args.force:
        raise SystemExit(
            "ota_client: the image's first byte is "
            f"0x{image[0]:02X}, not 0xE9 (not an ESP32 app image). "
            "Pass --force to send it anyway."
        )
    digest = hashlib.sha256(image).digest()
    if args.sig:
        sig = pb.b64decode(args.sig)
        if len(sig) != 64:
            raise SystemExit("ota_client: --sig is not 64 bytes of base64")
    else:
        key_path = args.key
        if not os.path.exists(os.path.expanduser(key_path)):
            raise SystemExit(
                f"ota_client: no --sig given and no signing key at {key_path}. "
                "Set PIANO_SIGNING_KEY or pass --sig."
            )
        sig = sign_digest(digest, key_path)
    return image, digest, sig


# ---------------------------------------------------------------------------
# The radio run.
# ---------------------------------------------------------------------------


class NotifyLog:
    """Collects Control notifications with arrival timestamps and prints each
    once, as hex + decoded meaning, when it is consumed."""

    def __init__(self, stamp: pb.Stamp) -> None:
        self.stamp = stamp
        self.queue: "asyncio.Queue[tuple]" = asyncio.Queue()

    def callback(self, _char, data: bytearray) -> None:
        self.queue.put_nowait((self.stamp.ms(), bytes(data)))

    async def next(self, timeout: float, what: str) -> Optional[pb.OtaEvent]:
        try:
            t_ms, data = await asyncio.wait_for(self.queue.get(), timeout)
        except asyncio.TimeoutError:
            print(f"[{self.stamp.ms():9.1f} ms]  TIMEOUT after {timeout:.0f} s "
                  f"waiting for {what}")
            return None
        ev = pb.parse_control(data)
        print(f"[{t_ms:9.1f} ms]  {pb.hexs(data):<26}  {ev.decoded()}")
        return ev


def _cb_peripheral(client):
    try:
        return client._backend._peripheral  # CoreBluetooth CBPeripheral
    except Exception:
        return None


async def _wait_writable(peripheral) -> None:
    """Best-effort flow control for write-without-response on macOS: wait until
    CoreBluetooth can take another one. A no-op when the attribute is absent."""
    if peripheral is None:
        return
    try:
        for _ in range(5000):
            if peripheral.canSendWriteWithoutResponse():
                return
            await asyncio.sleep(0.001)
    except Exception:
        return


async def console_get(client, stamp: pb.Stamp, line: str, timeout: float = 2.0) -> List[str]:
    """One console round trip over NUS (used after OK to read !ota / !fw)."""
    lines: List[str] = []
    assembler = pb.ConsoleLineAssembler()
    done = asyncio.Event()

    def on_tx(_char, data: bytearray) -> None:
        assembler.feed(bytes(data), lambda s: (lines.append(s), done.set()))

    await client.start_notify(pb.NUS_TX_UUID, on_tx)
    await client.write_gatt_char(pb.NUS_RX_UUID, (line + "\n").encode(), response=False)
    try:
        await asyncio.wait_for(done.wait(), timeout)
    except asyncio.TimeoutError:
        pass
    await asyncio.sleep(0.2)  # let any trailing lines arrive
    try:
        await client.stop_notify(pb.NUS_TX_UUID)
    except Exception:
        pass
    for s in lines:
        print(f"[{stamp.ms():9.1f} ms]  console: {s}")
    return lines


def split_chunks(image: bytes, chunk: int) -> List[bytes]:
    return [image[i : i + chunk] for i in range(0, len(image), chunk)] or [b""]


async def run(args) -> int:
    stamp = pb.Stamp()
    image, digest, sig = load_image_and_signature(args)
    plan = build_plan(
        image, digest, sig,
        version=args.version,
        window=args.window,
        flip_image_byte=args.flip_image_byte,
        flip_sha_bit=args.flip_sha_bit,
        flip_sig_bit=args.flip_sig_bit,
        claim_size=args.claim_size,
        first_seq=args.first_seq,
        begin_twice=args.begin_twice,
        abort_after=args.abort_after,
        disconnect_after=args.disconnect_after,
        stall_after=args.stall_after,
        notes_then_begin_ms=args.notes_then_begin,
        begin_within_ms=args.begin_within_ms,
        expect=args.expect,
    )

    print(f"image {args.image}: {len(plan.base_image)} bytes, "
          f"sha256 {plan.base_sha256.hex()}")
    print(f"BEGIN: version {plan.version}, claimed size {plan.claimed_size}, "
          f"window {plan.window}")
    for note in plan.notes:
        print(f"  fault: {note}")
    if plan.expect:
        print(f"  expecting outcome: {plan.expect}")

    try:
        client, device, features = await pb.connect_piano(timeout=args.scan_timeout)
    except (pb.PianoNotFound, pb.BluetoothPermission) as exc:
        print(f"\nERROR: {exc}")
        return 3
    print(f"\nConnected to {device.name or device.address}  ({features.summary()})")

    outcome = "unknown"
    try:
        if not features.update and not args.force:
            print("ERROR: this piano has no update service (7D0A0001-...). "
                  "Older firmware, or a stale service list — toggle Bluetooth.")
            return 3

        fw_before = await pb.read_firmware_revision(client)
        print(f"0x2A26 before: {fw_before!r}")

        outcome = await _transfer(client, plan, stamp, features)

        if outcome == "ok" and not args.no_reconnect:
            outcome = await _after_ok(client, plan, stamp, args)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

    print(f"\nOutcome: {outcome}")
    if plan.expect:
        ok = outcome == plan.expect
        print(f"{'PASS' if ok else 'FAIL'}: expected {plan.expect}, got {outcome}")
        return 0 if ok else 1
    # No --expect: a clean OK is success; anything else is reported non-zero.
    return 0 if outcome == "ok" else 1


async def _transfer(client, plan: Plan, stamp: pb.Stamp, features) -> str:
    log = NotifyLog(stamp)
    await client.start_notify(pb.UPDATE_CONTROL_UUID, log.callback)

    # Optional: play a note first, then BEGIN after a delay (docs/BLE_OTA.md §13,
    # plan correction 12). THIS SOUNDS A KEY.
    if plan.notes_then_begin_ms is not None:
        if not features.midi:
            print("ERROR: --notes-then-begin needs the MIDI characteristic")
            return "unknown"
        print(f"[{stamp.ms():9.1f} ms]  MIDI NoteOn/NoteOff (a key sounds), "
              f"then BEGIN in {plan.notes_then_begin_ms} ms")
        await client.write_gatt_char(pb.MIDI_CHAR_UUID, pb.midi_note_on(60, 64), response=False)
        await asyncio.sleep(0.05)
        await client.write_gatt_char(pb.MIDI_CHAR_UUID, pb.midi_note_off(60), response=False)
        await asyncio.sleep(plan.notes_then_begin_ms / 1000.0)

    print(f"[{stamp.ms():9.1f} ms]  -> BEGIN ({len(plan.begin)} bytes)")
    await client.write_gatt_char(pb.UPDATE_CONTROL_UUID, plan.begin, response=True)

    if plan.begin_twice:
        print(f"[{stamp.ms():9.1f} ms]  -> BEGIN again (second)")
        await client.write_gatt_char(pb.UPDATE_CONTROL_UUID, plan.begin, response=True)

    # Wait for the piano's first word: READY, or a BEGIN-time refusal.
    ev = await log.next(timeout=10.0, what="READY or an error after BEGIN")
    if ev is None:
        return "timeout:ready"
    if ev.op == pb.OP_ERR:
        return f"err:{ev.code}"
    if ev.op == pb.OP_ABORTED:
        return "aborted"
    if ev.op != pb.OP_READY:
        return "malformed"

    mc = min(ev.max_chunk or pb.MAX_CHUNK, pb.MAX_CHUNK)
    window = ev.window or plan.window
    if mc <= 0:
        print("ERROR: READY gave a zero maxChunk")
        return "malformed"
    chunks = split_chunks(plan.image, mc)
    print(f"[{stamp.ms():9.1f} ms]  READY accepted: maxChunk {mc}, window {window}, "
          f"{len(chunks)} Data frames")

    peripheral = _cb_peripheral(client)

    # A wrong first sequence: send one frame, expect ERR 4.
    if plan.first_seq:
        await _wait_writable(peripheral)
        await client.write_gatt_char(
            pb.UPDATE_DATA_UUID, pb.data_frame(plan.first_seq, chunks[0]), response=False)
        print(f"[{stamp.ms():9.1f} ms]  -> Data seq {plan.first_seq} (deliberately wrong)")
        ev = await log.next(timeout=10.0, what="ERR after a bad first sequence")
        if ev is None:
            return "timeout:ack"
        return _terminal(ev)

    transfer_start = time.monotonic()
    sent = 0
    windows_done = 0
    seq = 0
    i = 0
    while i < len(chunks):
        n = min(window, len(chunks) - i)
        for j in range(n):
            await _wait_writable(peripheral)
            await client.write_gatt_char(
                pb.UPDATE_DATA_UUID, pb.data_frame(seq, chunks[i + j]), response=False)
            seq = (seq + 1) & 0xFFFF
            sent += len(chunks[i + j])
        i += n

        ev = await log.next(timeout=10.0, what=f"ACK for byte {sent}")
        if ev is None:
            return "timeout:ack"
        if ev.op == pb.OP_ERR:
            return f"err:{ev.code}"
        if ev.op != pb.OP_ACK:
            return "malformed"
        if ev.bytes_received != sent:
            print(f"  WARNING: ACK says {ev.bytes_received} bytes, we sent {sent}")
        windows_done += 1

        # Faults keyed on completed windows.
        if plan.abort_after == windows_done:
            print(f"[{stamp.ms():9.1f} ms]  -> ABORT after {windows_done} windows")
            await client.write_gatt_char(pb.UPDATE_CONTROL_UUID, pb.abort_frame(), response=True)
            ev = await log.next(timeout=10.0, what="ABORTED")
            return _terminal(ev) if ev else "timeout:aborted"
        if plan.disconnect_after == windows_done:
            print(f"[{stamp.ms():9.1f} ms]  disconnecting after {windows_done} windows")
            await client.disconnect()
            return "lost"
        if plan.stall_after == windows_done:
            print(f"[{stamp.ms():9.1f} ms]  stalling 11 s after {windows_done} windows "
                  "(expect ERR 9)")
            ev = await log.next(timeout=20.0, what="ERR 9 after an 11 s stall")
            return _terminal(ev) if ev else "timeout:stall"

    elapsed = time.monotonic() - transfer_start
    rate = (sent / 1024.0 / elapsed) if elapsed > 0 else 0.0
    print(f"[{stamp.ms():9.1f} ms]  all {sent} bytes sent in {elapsed:.1f} s "
          f"({rate:.1f} kB/s)")

    print(f"[{stamp.ms():9.1f} ms]  -> END")
    await client.write_gatt_char(pb.UPDATE_CONTROL_UUID, pb.end_frame(), response=True)

    # VERIFYING, then OK or ERR, within 30 s.
    deadline = time.monotonic() + 30.0
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            print(f"[{stamp.ms():9.1f} ms]  TIMEOUT waiting for OK/ERR after END")
            return "timeout:verify"
        ev = await log.next(timeout=remaining, what="OK or ERR after END")
        if ev is None:
            return "timeout:verify"
        if ev.op in (pb.OP_OK, pb.OP_ERR, pb.OP_ABORTED):
            return _terminal(ev)
        # VERIFYING and anything else: keep waiting.


def _terminal(ev: pb.OtaEvent) -> str:
    if ev.op == pb.OP_OK:
        return "ok"
    if ev.op == pb.OP_ABORTED:
        return "aborted"
    if ev.op == pb.OP_ERR:
        return f"err:{ev.code}"
    return "malformed"


async def _after_ok(client, plan: Plan, stamp: pb.Stamp, args) -> str:
    """After OK: wait for the link to drop, reconnect within 60 s, re-read the
    version, and (if the console is present) read !ota and !fw. Optionally send
    a BEGIN too soon after the restart (run 9)."""
    print(f"[{stamp.ms():9.1f} ms]  OK — waiting for the piano to restart "
          "and drop the link")
    try:
        await client.disconnect()
    except Exception:
        pass

    # Reconnect within 60 s.
    deadline = time.monotonic() + 60.0
    new_client = None
    while time.monotonic() < deadline:
        await asyncio.sleep(2.0)
        try:
            new_client, device, features = await pb.connect_piano(timeout=8.0)
            break
        except (pb.PianoNotFound, pb.BluetoothPermission):
            continue
    if new_client is None:
        print(f"[{stamp.ms():9.1f} ms]  the piano did not come back within 60 s")
        return "timeout:reconnect"

    try:
        print(f"[{stamp.ms():9.1f} ms]  reconnected ({features.summary()})")
        fw_after = await pb.read_firmware_revision(new_client)
        print(f"0x2A26 after:  {fw_after!r}")
        if features.console:
            await console_get(new_client, stamp, "get !ota")
            await console_get(new_client, stamp, "get !fw")

        if plan.begin_within_ms is not None:
            print(f"[{stamp.ms():9.1f} ms]  sending BEGIN {plan.begin_within_ms} ms "
                  "after the restart (expect ERR 3 while the image is pending)")
            await asyncio.sleep(plan.begin_within_ms / 1000.0)
            log = NotifyLog(stamp)
            await new_client.start_notify(pb.UPDATE_CONTROL_UUID, log.callback)
            await new_client.write_gatt_char(pb.UPDATE_CONTROL_UUID, plan.begin, response=True)
            ev = await log.next(timeout=10.0, what="the reply to a too-soon BEGIN")
            return _terminal(ev) if ev else "timeout:begin_within"
    finally:
        try:
            await new_client.disconnect()
        except Exception:
            pass
    return "ok"


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Bluetooth firmware-update test client (docs/BLE_OTA.md §13). "
        "No tool sounds a key except --notes-then-begin.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--image", help="the firmware .bin to send")
    p.add_argument("--version", default="2.0.1",
                   help="the version string placed in BEGIN")
    p.add_argument("--window", type=int, default=16, help="Data frames per ACK (1..32)")
    p.add_argument("--sig", help="base64 Ed25519 signature of the SHA-256 "
                   "(else the image is signed with the key)")
    p.add_argument("--key", default=os.environ.get("PIANO_SIGNING_KEY", DEFAULT_KEY),
                   help="the Ed25519 private key PEM (read only, never printed)")
    p.add_argument("--scan-timeout", type=float, default=12.0,
                   help="seconds to scan for the piano")
    p.add_argument("--force", action="store_true",
                   help="send even if the image's first byte is not 0xE9, or there "
                   "is no update service")
    p.add_argument("--no-reconnect", action="store_true",
                   help="do not reconnect and re-read the version after OK")

    # Fault injection (docs/BLE_OTA.md §13).
    g = p.add_argument_group("fault injection")
    g.add_argument("--flip-image-byte", action="store_true",
                   help="flip one transmitted image byte (expect ERR 5)")
    g.add_argument("--flip-sha-bit", action="store_true",
                   help="flip one bit of BEGIN's SHA field (expect ERR 6)")
    g.add_argument("--flip-sig-bit", action="store_true",
                   help="flip one bit of BEGIN's signature (expect ERR 6)")
    g.add_argument("--abort-after", type=int, metavar="N",
                   help="send ABORT after N windows (expect ABORTED)")
    g.add_argument("--disconnect-after", type=int, metavar="N",
                   help="drop the link after N windows (expect lost)")
    g.add_argument("--stall-after", type=int, metavar="N",
                   help="go silent for 11 s after N windows (expect ERR 9)")
    g.add_argument("--begin-twice", action="store_true",
                   help="send a second BEGIN (expect ERR 3)")
    g.add_argument("--first-seq", type=int, default=0, metavar="N",
                   help="start Data at sequence N instead of 0 (N>0 expects ERR 4)")
    g.add_argument("--claim-size", type=int, metavar="N",
                   help="put N in BEGIN's size field (N>slot expects ERR 2)")
    g.add_argument("--begin-within-ms", type=int, metavar="N",
                   help="after OK and the reconnect, send BEGIN N ms later (run 9: ERR 3)")
    g.add_argument("--notes-then-begin", type=int, metavar="MS",
                   help="play a note (A KEY SOUNDS), then BEGIN after MS ms "
                   "(under 500 expects ERR 1)")

    p.add_argument("--expect", metavar="OUTCOME",
                   help="required outcome for exit 0: ok | aborted | lost | err:N")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    if not args.image:
        print("ota_client: --image is required to run a transfer "
              "(use --help for the options)", file=sys.stderr)
        return 2
    if args.expect is not None:
        if args.expect not in ("ok", "aborted", "lost") and not (
            args.expect.startswith("err:") and args.expect[4:].isdigit()
        ):
            print("ota_client: --expect must be ok, aborted, lost or err:N",
                  file=sys.stderr)
            return 2
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\ninterrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
