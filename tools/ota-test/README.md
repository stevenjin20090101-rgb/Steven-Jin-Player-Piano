# ota-test — Mac-side bench tools for firmware 2.0.0

These are the tools the owner and the bench session use to prove the "Steven
Piano" firmware (ESP32-S3, BLE peripheral named **Steven Piano**) matches its
two Bluetooth contracts:

- **`docs/BLE_OTA.md`** — the signed firmware-update service (§13 is the
  acceptance table these tools run).
- **`docs/BLE_SETTINGS.md`** — the Nordic UART settings console (§7 is its
  acceptance list).

Everything that can be checked **without a radio** is checked by `selftest.py`,
so the tools are proven correct against the spec's own numbers before any piano
is attached. The spec corrections in `~/.claude/plans/humming-prancing-finch.md`
apply: replies come only to the client that asked; the console default-denies
with a fixed refusal string; and BLE_OTA §13's two "another central" checks are
replaced by "MIDI notes then BEGIN".

## The tools

| File | What it is for |
|---|---|
| `piano_ble.py` | Shared, importable library: the UUIDs, pure frame builders/parsers (BEGIN/END/ABORT/Data, READY/ACK/VERIFYING/OK/ABORTED/ERR), BLE-MIDI framing, console line assembly, and the scan/connect helpers. No side effects on import; the radio is only touched when you call `find_piano()`/`connect_piano()`. |
| `ota_client.py` | BLE_OTA §13's update test client: BEGIN with response, windowed Data, ACK checks, END, OK/ERR, reconnect-and-re-read after OK, throughput, and every §13 fault flag. `--expect` makes it exit 0 only on the named outcome. |
| `console_client.py` | The BLE_SETTINGS console: send lines (arguments or stdin), print replies with timestamps, drop the `> ` echo. `--acceptance` runs BLE_SETTINGS §7 and prints PASS/FAIL per step. Never sends a command that moves a solenoid. |
| `sign_image.py` | Sign a test image (Ed25519 over its SHA-256, BLE_OTA §10) and verify the signature against the repo's public key. The private key is read by path only and never printed. |
| `selftest.py` | No radio, no piano: reproduces §5's worked example byte for byte, checks parsing, BLE-MIDI/console framing, the §10 manifest signature, and every fault flag. Exit 0 only if all match. |

## One-time setup

```bash
python3 -m venv tools/ota-test/.venv
tools/ota-test/.venv/bin/pip install -r tools/ota-test/requirements.txt
```

Run the tools with that interpreter, e.g.:

```bash
tools/ota-test/.venv/bin/python tools/ota-test/selftest.py
tools/ota-test/.venv/bin/python tools/ota-test/console_client.py --acceptance
```

`selftest.py`'s pure checks run without any third-party module; only the two
Ed25519 signature checks need `cryptography` (it says so and skips them if it is
missing).

## macOS Bluetooth permission

On macOS the app that launches Python — **Terminal** (or iTerm, or VS Code's
terminal) — must be allowed to use Bluetooth under **System Settings › Privacy &
Security › Bluetooth**. If it is not, the first scan or connect fails
immediately with a message like *"Bluetooth access is denied … Check macOS
privacy settings"* (bleak raises `BleakBluetoothNotAvailableError`); the tools
print that and exit. The fix is to tick the terminal app in that list and run
again. A scan that simply finds nothing prints a different message (see below).

## After a USB flash: toggle Bluetooth

The piano does **not** advertise while another device is connected to it, so a
scan can time out for that reason alone (disconnect the phone/iPad/other Mac
first). Separately, right after a USB flash macOS often caches a **stale GATT
service list**, so the update or console service looks missing even though it is
there. **Turn the Mac's Bluetooth off and on** (or "Forget" the device) to clear
that cache, then retry.

## Signing a test image

```bash
export PIANO_SIGNING_KEY=~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem   # the default
tools/ota-test/.venv/bin/python tools/ota-test/sign_image.py --out test.sig firmware-2.0.1.bin
```

It prints the size, SHA-256 and base64 signature, and whether the signature
verifies against `provenance/author_ed25519_public.pem`. **Never commit a test
image or its signature** — `*.bin` and `*.sig` are git-ignored. A test image
signed with the real key still must not be published through a release; sign it,
flash it over Bluetooth with `ota_client.py`, and throw it away.

## Acceptance: BLE_OTA §13

Build a signed test image first (a 2.0.1 built locally, never published). Pass
its path as `--image`; sign with `--sig <base64>` or let the client sign it with
`$PIANO_SIGNING_KEY`. Each command prints every notification and ends with
`Outcome: …` and a `PASS`/`FAIL` line; with `--expect` the process exits 0 only
on that outcome.

| # | Run | Command (`P=tools/ota-test/.venv/bin/python`, `C=tools/ota-test`) | A pass looks like |
|---|---|---|---|
| 1 | one image byte flipped, original sha/sig | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --flip-image-byte --expect err:5` | READY, ACKs, `82`, then `E1 05`; `Outcome: err:5`, exit 0. Bench: serial logs hash mismatch; old version after reboot. |
| 2 | one bit of `sig` flipped | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --flip-sig-bit --expect err:6` | `E1 06` right after BEGIN, no READY; `Outcome: err:6`, exit 0. |
| 3 | ABORT after 20 windows | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --abort-after 20 --expect aborted` | ACKs through window 20, then `84`; `Outcome: aborted`, exit 0. Bench: next note plays; LEDs/status back. |
| 4 | disconnect after 20 windows | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --disconnect-after 20 --expect lost` | link dropped after 20 ACKs; `Outcome: lost`, exit 0. Bench: serial "update abandoned"; reconnect shows `0x2A26` unchanged; a fresh good update then succeeds. |
| 5 | stall 11 s after 5 windows | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --stall-after 5 --expect err:9` | silence, then `E1 09`; `Outcome: err:9`, exit 0. |
| 6a | a second BEGIN | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --begin-twice --expect err:3` | `E1 03`; exit 0. |
| 6b | Data seq 5 first | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --first-seq 5 --expect err:4` | `E1 04`; exit 0. |
| 6c | size 7,000,000 | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --claim-size 7000000 --expect err:2` | `E1 02` at BEGIN; exit 0. |
| 7 | a good update | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --expect ok` | `83 DC 05`, link drops, back ≤ 60 s, `0x2A26 after` = new version, `get !ota` → `pending`; exit 0. Bench: serial `running app1 state PENDING_VERIFY`, ~30 s later `confirmed`, screen `Updated to 2.0.1`. |
| 8 | good update, then cut power 10 s after the new image boots | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --expect ok` then **pull the controller's power by hand** | the update reports `ok`; after the power cut the **old** version boots (bootloader is silent — judge by the banner); screen `Update to 2.0.1 rolled back`. The tool cannot cut power; that step is by hand. |
| 9 | BEGIN within 30 s of run 7's restart | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --begin-within-ms 5000 --expect err:3` | a good update, then after reconnect a BEGIN 5 s later gets `E1 03` (running image still PENDING_VERIFY); exit 0. |

**ERR 10 (crash-loop safe mode)** needs three crash resets within 30 s of boot;
test it once with a temporary debug build or skip it (BLE_OTA §13). No tool can
force it.

**Bench checklist (SAFETY.md), the parts a tool drives** — the "another central"
checks are replaced per the plan by:

| Check | Command | A pass looks like |
|---|---|---|
| MIDI notes then BEGIN under 500 ms (**a key will sound**) | `$P $C/ota_client.py --image fw.bin --version 2.0.1 --notes-then-begin 200 --expect err:1` | one note sounds, then `E1 01` (not quiet); exit 0. |
| `fire` over USB, then BEGIN within 0.5 s | run `fire 3 0 2000` on the USB serial console, then `$P $C/ota_client.py --image fw.bin --version 2.0.1 --expect err:1` | the coil is released (verified), then `E1 01`. |
| touch-key taps during a transfer | start run 7, tap keys by hand during Data | nothing sounds; the transfer finishes `ok`. |

## Acceptance: BLE_SETTINGS §7

```bash
tools/ota-test/.venv/bin/python tools/ota-test/console_client.py --acceptance
```

Runs steps 2–7 from one client and prints PASS/FAIL per step, exit 0 only if all
pass. It restores `ledbright` and never moves a solenoid.

| Step | What `--acceptance` does | A pass looks like |
|---|---|---|
| 1 | *(bench, not scriptable)* connect over MIDI, confirm the iPad still plays while services are discovered | done by hand with the iPad; `console_client.py dump` shows the console is present. |
| 2 | `dump`, timed | the full list arrives, ends with `end`, within the 2 s app limit (300 ms is the target); `!proto=1` present; every §4 name present in order. PASS. |
| 3 | `get ledbright`, write a different value, `get`, restore | the written value reads back, then the original is restored. PASS. |
| 4 | `fire 0 0 4095` over NUS | a reply containing `refused`; nothing fires (the firmware default-denies it over Bluetooth). PASS. |
| 5 | 50 `get !proto` lines in one burst | all 50 answered. PASS. |
| 6 | disconnect mid-`dump`, reconnect, clean `dump` | the reconnect `dump` ends with `end`. PASS. |
| 7 | unsubscribe from TX, send `status` | nothing is queued. PASS. |

Plain use (not acceptance):

```bash
tools/ota-test/.venv/bin/python tools/ota-test/console_client.py dump
tools/ota-test/.venv/bin/python tools/ota-test/console_client.py             # interactive (stdin)
```

## What a person must still check by hand at the bench

The tools cannot see serial output, the AMOLED screen, the LED strip, the
coils, or a power cut. After a tool run, a person confirms: the serial banner
and state (`running app1 state PENDING_VERIFY`, later `confirmed`); the screen
texts (`Updating • NN %`, `Updated to 2.0.1`, `Update to 2.0.1 rolled back`);
that coils are released and `i2cFails` stays flat; that LEDs and the status line
return after any error or abort; the power-cut rollback (run 8); ERR 10; and
that the iPad keeps playing over BLE-MIDI while the console is in use (§7 step 1).
