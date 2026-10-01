#!/usr/bin/env python3
# ============================================================================
#  Player Piano - ESP32-S3 self-playing acoustic piano
#  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
#  Original author & creator: Steven Jin.
#  Licensed under the MIT License (see LICENSE). This copyright and attribution
#  notice MUST be preserved in all copies or substantial portions of the work.
#  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
# ============================================================================
# The Bluetooth settings console (docs/BLE_SETTINGS.md). Connects to the piano,
# switches on the Nordic UART TX notifications, sends console lines (given as
# arguments or read from stdin), and prints the reply lines with millisecond
# timestamps, dropping the "> command" echo.
#
# --acceptance runs BLE_SETTINGS §7 as far as one client can and prints PASS or
# FAIL per step, exiting 0 only when every step passes. It restores every
# setting it changes and never sends a command that moves a solenoid (the one
# `fire` it sends is refused over Bluetooth by the firmware, which is the point
# of step 4).
#
#   .venv/bin/python console_client.py dump
#   .venv/bin/python console_client.py            # interactive, reads stdin
#   .venv/bin/python console_client.py --acceptance

from __future__ import annotations

import argparse
import asyncio
import sys
import time
from typing import List, Optional

import piano_ble as pb

# The tunables and facts in BLE_SETTINGS §4 order (facts carry a leading '!').
EXPECTED_DUMP_NAMES = [
    "!proto", "!fw", "!ota", "!ble", "!boards", "!i2cfails", "!pedalboard", "!uptime",
    "leds", "ledmode", "ledbright", "ledcount", "ledoffset", "ledscale", "ledtail",
    "ledreverse", "reactcolor", "ledglow", "velbright", "decay", "rainspeed", "dimsecs",
    "dimfloor", "keyviz", "fullpower", "volume", "velcurve", "velmult", "min", "minblack",
    "max", "humanvel", "humantime", "burstgap", "burstboost", "minstrike", "isostrike",
    "isogap", "gap", "hold", "restrike", "softrelease", "releasepwm", "releasems", "freq",
    "keyforce_white", "keyforce_black", "pedalon", "pedalhalf", "pedalup", "pedaldown",
]


class ConsoleSession:
    """A connected console: subscribes to TX, sends lines, gathers reply lines."""

    def __init__(self, client, stamp: pb.Stamp, echo: bool = True) -> None:
        self.client = client
        self.stamp = stamp
        self.echo = echo
        self._assembler = pb.ConsoleLineAssembler()
        self._queue: "asyncio.Queue[tuple]" = asyncio.Queue()
        self._subscribed = False

    def _on_tx(self, _char, data: bytearray) -> None:
        now = self.stamp.ms()
        self._assembler.feed(bytes(data), lambda s: self._queue.put_nowait((now, s)))

    async def subscribe(self) -> None:
        await self.client.start_notify(pb.NUS_TX_UUID, self._on_tx)
        self._subscribed = True

    async def unsubscribe(self) -> None:
        if self._subscribed:
            try:
                await self.client.stop_notify(pb.NUS_TX_UUID)
            except Exception:
                pass
            self._subscribed = False

    async def send(self, text: str) -> None:
        payload = (text + "\n").encode("utf-8")
        if len(payload) > 80:
            raise ValueError("a console line is at most 79 characters + newline")
        await self.client.write_gatt_char(pb.NUS_RX_UUID, payload, response=False)

    async def collect(self, quiet: float = 0.6, overall: float = 3.0,
                      until: Optional[str] = None, printed: bool = True) -> List[str]:
        """Gather reply lines until `until` is seen, or `quiet` seconds pass with
        no new line, or `overall` seconds total. Prints each as it arrives."""
        lines: List[str] = []
        start = time.monotonic()
        while True:
            remaining = overall - (time.monotonic() - start)
            if remaining <= 0:
                break
            try:
                t_ms, line = await asyncio.wait_for(self._queue.get(), min(quiet, remaining))
            except asyncio.TimeoutError:
                break
            lines.append(line)
            if printed:
                print(f"[{t_ms:9.1f} ms]  {line}")
            if until is not None and line.strip() == until:
                break
        return lines


def value_of(lines: List[str], name: str) -> Optional[str]:
    """Pull `name=value` out of reply lines (handles the '!' on facts)."""
    for line in lines:
        s = line.strip()
        if s.startswith(name + "="):
            return s.split("=", 1)[1]
    return None


# ---------------------------------------------------------------------------
# Acceptance (BLE_SETTINGS §7).
# ---------------------------------------------------------------------------


async def acceptance(stamp: pb.Stamp, scan_timeout: float) -> int:
    results: List[tuple] = []  # (step, passed, detail)

    def record(step: str, passed: bool, detail: str) -> None:
        results.append((step, passed, detail))
        print(f"\n{'PASS' if passed else 'FAIL'}  {step}: {detail}")

    # Connect once for steps 2-5 and 7; step 6 reconnects itself.
    try:
        client, device, features = await pb.connect_piano(timeout=scan_timeout)
    except (pb.PianoNotFound, pb.BluetoothPermission) as exc:
        print(f"ERROR: {exc}")
        return 3
    print(f"Connected to {device.name or device.address}  ({features.summary()})")
    if not features.console:
        print("ERROR: this piano has no Nordic UART console (6E400001-...).")
        await client.disconnect()
        return 3

    try:
        session = ConsoleSession(client, stamp)
        await session.subscribe()

        # --- Step 2: dump whole, ends 'end', timing, !proto=1, names in order ---
        print("\n--- Step 2: dump ---")
        t0 = time.monotonic()
        await session.send("dump")
        lines = await session.collect(quiet=1.0, overall=5.0, until="end")
        elapsed_ms = (time.monotonic() - t0) * 1000.0
        ends = any(l.strip() == "end" for l in lines)
        proto = value_of(lines, "!proto") == "1"
        names = [l.strip().split("=", 1)[0] for l in lines if "=" in l and not l.startswith(" ")]
        in_order = _is_ordered_subsequence(EXPECTED_DUMP_NAMES, names)
        missing = [n for n in EXPECTED_DUMP_NAMES if n not in names]
        ok = ends and proto and in_order and not missing and elapsed_ms < 2000.0
        detail = (f"{len(lines)} lines in {elapsed_ms:.0f} ms "
                  f"(target 300 ms, app limit 2000 ms); ends-with-end={ends}; "
                  f"!proto=1={proto}; names-in-order={in_order}")
        if missing:
            detail += f"; MISSING {missing}"
        if elapsed_ms >= 300.0 and elapsed_ms < 2000.0:
            detail += "; note: over the 300 ms target but within the 2 s limit"
        record("Step 2 (dump)", ok, detail)

        # --- Step 3: ledbright write, get, restore ---
        print("\n--- Step 3: ledbright round trip ---")
        await session.send("get ledbright")
        original = value_of(await session.collect(quiet=0.6, overall=2.0), "ledbright")
        if original is None:
            record("Step 3 (ledbright)", False, "could not read the original ledbright")
        else:
            test_value = "40" if original != "40" else "50"
            await session.send(f"ledbright {test_value}")
            await session.collect(quiet=0.6, overall=2.0)
            await session.send("get ledbright")
            got = value_of(await session.collect(quiet=0.6, overall=2.0), "ledbright")
            # Restore.
            await session.send(f"ledbright {original}")
            await session.collect(quiet=0.6, overall=2.0)
            await session.send("get ledbright")
            restored = value_of(await session.collect(quiet=0.6, overall=2.0), "ledbright")
            ok = got == test_value and restored == original
            record("Step 3 (ledbright)", ok,
                   f"original={original}, wrote {test_value} -> {got}, "
                   f"restored -> {restored}")

        # --- Step 4: fire refused over Bluetooth ---
        print("\n--- Step 4: fire refused ---")
        await session.send("fire 0 0 4095")
        lines = await session.collect(quiet=0.6, overall=2.0)
        refused = any("refused" in l.lower() for l in lines)
        record("Step 4 (fire refused)", refused,
               "a reply contains 'refused'" if refused
               else "no 'refused' reply (nothing should have fired regardless)")

        # --- Step 5: 50 read-only lines in one burst, each answered ---
        # Sent as 50 rapid write-without-response lines (a single >MTU write
        # cannot be carried without response on macOS); the firmware's 1 KB RX
        # stream buffer holds the burst and answers every line.
        print("\n--- Step 5: 50-line burst ---")
        for _ in range(50):
            await session.send("get !proto")
        lines = await session.collect(quiet=1.5, overall=8.0, printed=False)
        answers = sum(1 for l in lines if l.strip() == "!proto=1")
        ok = answers >= 50
        record("Step 5 (50-line burst)", ok, f"{answers}/50 answered")

        # --- Step 7: notifications off, status produces nothing ---
        print("\n--- Step 7: status with notifications off ---")
        await session.unsubscribe()
        await asyncio.sleep(0.3)
        # Drain anything already queued so we only judge what arrives next.
        while not session._queue.empty():
            session._queue.get_nowait()
        await session.send("status")
        quiet_lines = await session.collect(quiet=1.5, overall=2.0, printed=False)
        ok = len(quiet_lines) == 0
        record("Step 7 (quiet when unsubscribed)", ok,
               "nothing queued" if ok else f"{len(quiet_lines)} lines leaked")
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

    # --- Step 6: disconnect mid-dump, reconnect, clean dump ---
    print("\n--- Step 6: disconnect mid-dump, reconnect ---")
    try:
        ok = await _step6_reconnect(stamp, scan_timeout)
        record("Step 6 (mid-dump reconnect)", ok,
               "a clean dump after a mid-dump disconnect" if ok
               else "the dump after reconnect was not clean")
    except (pb.PianoNotFound, pb.BluetoothPermission) as exc:
        record("Step 6 (mid-dump reconnect)", False, f"reconnect failed: {exc}")

    passed = sum(1 for _, p, _ in results if p)
    print(f"\n{'='*60}\nAcceptance: {passed}/{len(results)} steps passed.")
    for step, p, _ in results:
        print(f"  {'PASS' if p else 'FAIL'}  {step}")
    print("Note: step 1 (MIDI still plays while discovering services) is a bench "
          "check with the iPad; it cannot be judged from one client.")
    return 0 if passed == len(results) else 1


async def _step6_reconnect(stamp: pb.Stamp, scan_timeout: float) -> bool:
    client, _, features = await pb.connect_piano(timeout=scan_timeout)
    if not features.console:
        await client.disconnect()
        return False
    session = ConsoleSession(client, stamp)
    await session.subscribe()
    await session.send("dump")
    # Wait for the first line, then yank the link mid-dump.
    first = await session.collect(quiet=1.0, overall=2.0, printed=False)
    await client.disconnect()
    print(f"  disconnected after {len(first)} line(s) of the dump")
    await asyncio.sleep(1.0)

    # Reconnect and do a clean dump.
    client2, _, features2 = await pb.connect_piano(timeout=scan_timeout)
    try:
        if not features2.console:
            return False
        session2 = ConsoleSession(client2, stamp)
        await session2.subscribe()
        await session2.send("dump")
        lines = await session2.collect(quiet=1.0, overall=5.0, until="end", printed=False)
        ends = any(l.strip() == "end" for l in lines)
        has_proto = value_of(lines, "!proto") == "1"
        print(f"  reconnect dump: {len(lines)} lines, ends-with-end={ends}")
        return ends and has_proto
    finally:
        try:
            await client2.disconnect()
        except Exception:
            pass


def _is_ordered_subsequence(expected: List[str], got: List[str]) -> bool:
    """True when every name in `expected` appears in `got` in the same order."""
    it = iter(got)
    return all(name in it for name in expected)


# ---------------------------------------------------------------------------
# Plain send / interactive.
# ---------------------------------------------------------------------------


async def run_lines(stamp: pb.Stamp, scan_timeout: float, lines: List[str]) -> int:
    try:
        client, device, features = await pb.connect_piano(timeout=scan_timeout)
    except (pb.PianoNotFound, pb.BluetoothPermission) as exc:
        print(f"ERROR: {exc}")
        return 3
    print(f"Connected to {device.name or device.address}  ({features.summary()})")
    if not features.console:
        print("ERROR: this piano has no Nordic UART console (6E400001-...).")
        await client.disconnect()
        return 3
    try:
        session = ConsoleSession(client, stamp)
        await session.subscribe()
        if lines:
            for line in lines:
                print(f"> {line}")
                await session.send(line)
                await session.collect(quiet=0.8, overall=4.0)
        else:
            print("Interactive console. Type a command and press Enter; Ctrl-D to quit.")
            loop = asyncio.get_event_loop()
            while True:
                try:
                    line = await loop.run_in_executor(None, sys.stdin.readline)
                except (EOFError, KeyboardInterrupt):
                    break
                if not line:
                    break
                line = line.rstrip("\n")
                if not line:
                    continue
                await session.send(line)
                await session.collect(quiet=0.8, overall=4.0)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
    return 0


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Bluetooth settings console (docs/BLE_SETTINGS.md). "
        "Never sends a command that moves a solenoid.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("lines", nargs="*", help="console lines to send (else stdin)")
    p.add_argument("--acceptance", action="store_true",
                   help="run BLE_SETTINGS §7 and print PASS/FAIL per step")
    p.add_argument("--scan-timeout", type=float, default=12.0,
                   help="seconds to scan for the piano")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    stamp = pb.Stamp()
    try:
        if args.acceptance:
            return asyncio.run(acceptance(stamp, args.scan_timeout))
        return asyncio.run(run_lines(stamp, args.scan_timeout, args.lines))
    except KeyboardInterrupt:
        print("\ninterrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
