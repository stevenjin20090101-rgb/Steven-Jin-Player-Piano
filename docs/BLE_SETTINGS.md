# Bluetooth settings channel — spec for the firmware

> **Implemented in firmware 2.0.0 (2026-10-01)**, with corrections found while building it
> against the linked sources. They are made in place below:
> §1 the NUS service is created *before* `MIDI.begin()`; §2 four lines per loop pass in the
> console's own buffer; §3 replies are mirrored only to the Bluetooth client that asked, sent
> with the checked low-level notify; §4 `!fw` is the version string and `get` takes names with
> or without `!`; §5 the allow-list is default-deny and refusals use fixed words; §8 is fixed.
> Code: `src/ble_console.cpp`, `src/console_out.cpp`, `handleLine()` in `src/main.cpp`.

**Purpose.** Let the "Steven Piano" Android app (`Player Piano/android/`) read and adjust
the piano's lighting, feel, and pedal settings over Bluetooth, using the *existing* serial
console commands. Today those commands are reachable only over USB serial and the Web
Serial GUI. This document is the contract between the firmware and the app; the app side
is being built against it now (`android/BUILD_SPEC.md › Piano settings over Bluetooth`).

Written by the app-side session on 2026-09-24 from a full read of `src/main.cpp`
(`handleLine`, lines ~705–1195), `src/settings.cpp`, `gui/piano-control.html`, and the
NimBLE 1.4.3 / BLE-MIDI 2.2 sources actually linked. Nothing here changes what any
command does; it only adds a second way in and a machine-readable way out.

## 1. Transport: Nordic UART Service next to BLE-MIDI

Add the standard Nordic UART Service (NUS) to the same NimBLE server that BLE-MIDI uses.

| Item | Value |
|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX characteristic (app → piano) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`, properties WRITE + WRITE_NR |
| TX characteristic (piano → app) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`, property NOTIFY (CCCD 0x2902) |
| Security | none, same as BLE-MIDI (no bonding, no encryption) |
| Advertising | **unchanged.** The MIDI UUID + flags + appearance already fill 25 of 31 bytes; the app finds NUS by service discovery after connecting on the MIDI UUID |

Implementation notes (NimBLE 1.4.3, the version that is linked):

- **Create the NUS service before `MIDI.begin()`, not after** (corrected 2026-10-01; this
  note used to say "right after"). `MIDI.begin()` ends with `advertising->start()`, which
  runs the single GATT server start (`NimBLEAdvertising.cpp:428`). A service created after
  that runs `serviceChanged()` → `resetGATT()`, which calls `start()` on every service —
  including the new one, which has no characteristics yet — and the real `start()` later
  reuses that empty definition, so the service ends up registered twice, the first time
  empty. `setup()` therefore does: `NimBLEDevice::init(name); NimBLEDevice::setMTU(255);
  srv = NimBLEDevice::createServer();` → create and `start()` NUS on `srv` → then
  `MIDI.begin()` (its `init()` is then a no-op, `createServer()` returns the same server,
  and its `advertising->start()` runs the one GATT start). After that, the firmware checks
  that RX and TX have real attribute handles and logs one `[ble]` line. MIDI's own handles
  move once with 2.0.0; clients rediscover on each connection.
- Keep the library's `MyServerCallbacks` as the server callback (there is one slot; the
  firmware's connect/disconnect handling depends on it): never call `srv->setCallbacks()`.
  Use *characteristic* callbacks for NUS RX and for the TX CCCD subscribe.
- The connect/disconnect stop logic stays exactly as it is: any disconnect still runs the
  full all-off. NUS shares the single connection with MIDI; the app opens one link.

## 2. Threading: never parse on the Bluetooth task

`onWrite` for NUS RX runs on the NimBLE host task (core 0). It must **only append the
received bytes to a FreeRTOS byte queue** (≥ 512 bytes; drop with a counter when full,
never block). As built: a 1 KB stream buffer; each write goes in whole or not at all, as
one frame tagged with the connection that wrote it. The main `loop()` drains that queue
into the console's **own** 80-byte line buffer (never the serial reader's `s_lineBuf`;
end on `\n` or `\r`, blank lines skipped, 79 characters kept of a longer line as on USB)
and calls `handleLine(line, SRC_BLE)` on core 1, as serial input does, **at most 4 lines
per pass**; a line whose command is `testmin`, `testmax` or `panic` (they block for
150–200 ms) ends that pass. Solenoid dispatch, I²C, LEDs and the safety watchdogs stay on
the loop. Bluetooth lines count as activity for the screen's idle dimmer, like serial
lines.

## 3. Output: a tee that mirrors console replies to NUS TX

`handleLine()` and the functions it calls print replies straight to `Serial`. Add a
`Print` subclass (a tee) whose `write()` goes to `Serial` **and** into a TX ring buffer
(8 KB; `help` is ~5.2 KB). Route every reply that a command produces through it: the
`  …` reply lines in `handleLine`, `help`, `status`, the `[settings]` and `[pedal]` lines
printed by the functions it calls, and the `> line` echo (the app ignores lines starting
with `> `). Heartbeats, boot banners and debug chatter stay on serial only.

**Replies go to the asker** (corrected 2026-10-01). The tee copies into the ring only the
output produced while a line that arrived over NUS is being handled, and only when the
connection that wrote it is subscribed to TX (a mirror flag around that `handleLine` call).
USB commands are never mirrored — the web GUI's MIDI player streams `note` lines, and the
app would take someone else's `dump` for its own. NUS commands still print to Serial as
well. With no subscriber nothing is queued. The ring holds one asker's reply at a time.
As built, `[vol]` (from `set_master_volume()`) and `[test]` (from the sweep, refused over
NUS anyway) stay Serial-only: they come from `power_boards.cpp`, which the dispatch
simulator compiles without the tee. The app re-reads `fullpower` after a `volume` write.
When a reply does not fit in the ring, it is cut at a line boundary (counted in `status`).

The loop flushes the TX ring once per pass, near the end of `loop()`, as notifications to
the asking connection: chunks of at most `MTU − 3` bytes, at most 8 notifications per loop
pass, in order, lines ending in `\n` (a chunk may split a line; the app reassembles). It
uses the **checked low-level send** (corrected 2026-10-01): `ble_hs_mbuf_from_flat()`
then `ble_gattc_notify_custom()`, never `NimBLECharacteristic::notify()`, which in 1.4.3
neither checks the mbuf nor the return code. It stops for the pass while fewer than 5
mbufs are free (the stack's own ATT responses come from the same 12-block pool); any
failure keeps the bytes for the next pass. When the asker disconnects, unsubscribes or
comes back as a new subscription, the ring is discarded.

## 4. Two new commands: `dump` and `get`

Every reply today is free text written for people. The app needs one parseable form.

**`dump`** prints one line per tunable, `name=value`, then a final line `end`. The
`name` is the **command name that sets it**, so the app can write back with
`name value`. Values: integers as decimal; floats with two decimals; booleans `0`/`1`.
Read-only facts are prefixed `!`. Exact list and order:

```
!proto=1
!fw=<the version string, e.g. 2.0.0+a1b2c3d (BLE_OTA.md section 2)>
!ota=<none|pending|confirmed>      (the running image's update state, see BLE_OTA.md section 9: none = flashed over USB, pending = not yet confirmed, confirmed = self-test passed)
!ble=1
!boards=OK,OK,MISSING,OK,OK,OK,OK          (7 entries, MIDI 24-35 first)
!i2cfails=<n>
!pedalboard=absent|online
!uptime=<seconds>
!repeatms=<ms>                     (the shortest steady same-key repeat period the piano honours now, see below)
!temp=<°C, one decimal>             (diagnostics, docs/BLE_DIAG.md, from 2.0.1: the die temperature, read when asked; empty if the sensor gives nothing)
!heap=<bytes>                       (free heap now)
!heapmin=<bytes>                    (lowest free heap since boot)
!heapblock=<bytes>                  (largest free block)
!heapsize=<bytes>                   (the heap's whole size)
!reset=poweron|software|panic|watchdog|brownout|usb|other
!crashes=<n>                        (crash resets in a row, as counted at this boot; cleared after 30 s of healthy uptime)
!tasks=<n>                          (FreeRTOS tasks)
!stack=<bytes>                      (the loop task's stack never used)
!taskstack=loop:<bytes>,lvgl:<bytes>
!looprate=<passes per second>       (over the last whole 5 s; 0 until the first window has passed)
!loopms=<ms, one decimal>           (the longest loop pass in that window)
!rssi=<dBm>                         (the link's signal as the piano hears it; empty with no central connected)
!active=<n>                         (solenoid channels believed energized now)
!trips=<n>                          (hold-watchdog releases since boot)
!lockproto=1                        (the Bluetooth lock is present, docs/BLE_LOCK.md, from 2.1.0)
!lock=open|claimed|pairing
!auth=0|1                           (the connection that asked has proved a key; 1 on an open piano and over USB)
!nonce=<32 hex>                     (the asking connection's 16-byte nonce; 32 zeros over USB)
!guest=<seconds left>               (guest play; 0 = off)
!restartproto=1                     (the `restart` command exists, §5, from 2.1.1)
leds=0|1
ledmode=0..3
ledbright=0..255
ledcount=1..300
ledoffset=-300..300
ledscale=10..400
ledtail=0..255
ledreverse=0|1
reactcolor=0..7
ledglow=0..10
velbright=0|1
decay=1..40
rainspeed=1..40
dimsecs=0..3600
dimfloor=0..255
keyviz=0|1
fullpower=0|1
volume=0..100
velcurve=0.40..3.00
velmult=0.10..5.00
min=0..4095
minblack=0..4095
max=0..4095
humanvel=0..30
humantime=0..40
burstgap=0..600
burstboost=0..100
minstrike=0..500
isostrike=0..500
isogap=0..2000
gap=0..300
hold=50..4000
restrike=0|40..1000
softrelease=0|1
releasepwm=0..4095
releasems=0..200
freq=24..1526
keyforce_white=<mean multiplier of the white keys, 2 decimals>
keyforce_black=<mean multiplier of the black keys, 2 decimals>
pedalon=0|1
pedalhalf=0|1
pedalup=80..600
pedaldown=80..600
end
```

`minwhite` is the same variable as `min`; dump reports it once, as `min`.

`!repeatms` (added 2026-10-01, a compatible addition: the app keeps facts it does not know
and ignores them) is the shortest steady same-key repeat period the dispatch layer honours
with the current settings, `minstrike + gap + 2 × 10 ms` (each deferred release and deferred
strike runs on the 10 ms safety tick); it changes when `minstrike` or `gap` does. The first
repeat after a lone note can additionally wait up to `isostrike − minstrike`.

The diagnostics facts from `!temp` to `!trips` (added 2026-10-08 for 2.0.1, docs/BLE_DIAG.md) are compatible
additions of the same kind: read-only, computed when asked, listed after `!repeatms`. Two values can be empty
(`!temp` when the sensor gives nothing, `!rssi` with no central connected); a client shows an empty row.

The lock facts from `!lockproto` to `!guest` (added 2026-10-12 for 2.1.0, docs/BLE_LOCK.md) follow `!trips`.
`!auth` and `!nonce` belong to the connection that asked: the Bluetooth console tells the lock which
connection it is serving while it runs the line. The nonce is made on that connection's first ask and renewed
after every `auth` attempt; over USB `!auth=1` and the nonce reads 32 zeros. All five are readable by an
untrusted connection (`dump` and `get` stay open on a claimed piano), and change at once. From 2.1.0 the
console also hands `claim`, `auth` and `lock …` to the lock before the allow-list of §5, and on a claimed
piano an untrusted connection may run only `dump`, `get`, `status`, `help`, `off` and `broadcast`
(`  refused — not authorised (send auth first)` for anything else).

`!restartproto=1` (added 2026-10-21 for 2.1.1: the `restart` command exists) follows `!guest`, a compatible
addition of the same kind; the app sends `restart` only to a piano that lists it.

**`get <name>`** prints the single `name=value` line for any name in the list above
(including the `!` facts), or `  unknown setting` for anything else. The name may be given
with or without its `!`; a fact is always answered as `!name=value` (`get fw` and
`get !fw` both print `!fw=…`). The app uses `get`
after each write to show what the piano actually holds, and `dump` on connect and after
a preset.

`dump` and `get` are the only firmware output that starts a line at column 0 with
`name=`: the app reads every line matching `^!?[a-z][a-z0-9_]*=` as a value, so all other
replies stay indented or `[tagged]`. As built, one table in `main.cpp` drives both
commands in the order above; `keyforce_white` / `keyforce_black` are the mean multiplier
over the white / black keys of MIDI 24..107.

Both commands are plain console commands: they also work over USB serial and are
listed in `help`.

## 5. Allow-list over Bluetooth

**Default deny** (corrected 2026-10-01: the list below is what is *allowed*; everything
else is refused). Allowed over NUS: `name value` for every tunable in `dump` except
`keyforce_white` / `keyforce_black`; `minwhite` (the same variable as `min`); and the
commands `status`, `dump`, `get`, `help`, `?`, `off`, `broadcast`, `panic`, `save`,
`ledtest`, `testmin`, `testmax`, `note`, `restart` (from 2.1.1), and the presets `soft`,
`cinematic`, `expressive`, `snappy`. A line's command is its first word, compared exactly.

`restart` (docs/SMART_PLUG.md rule 10) makes the piano quiescent the way an update does —
queued notes dropped, every coil released through the verified all-off, pedal up, strip
dark, energizing inhibited — replies `  restarting` and resets the controller 200 ms later
(the next boot reads `!reset=software`); it is refused with `  refused — updating firmware`
while an update runs and `  refused — this firmware has not confirmed itself yet` while
`!ota=pending`, and on a claimed piano it needs a trusted connection (docs/BLE_LOCK.md).

Everything else — known or not — is refused over NUS with the reply
`  refused over Bluetooth — use the USB console` (after the `> line` echo), so a command
added to the firmware later stays bench-only until someone allows it. Today that refuses
`fire`, `rawnote`, `sweep`, `pwm`, `ramp`, `reset`, `blereset`, `pedaltest`, `keyforce`,
`keyforce_all`, `keyforce_white`, `keyforce_black`, `waterfall` and any unknown word.
(Bench and destructive commands stay at the bench; per-key force is not in scope for the
app yet.)

The check is in the NUS path before `handleLine`, and `handleLine(line, SRC_BLE)` checks
again; the serial path is unchanged.

**Reply wording is part of the contract** (added 2026-10-01). The app treats any reply line
containing `out of range`, `usage:`, `unknown` or `refused` (any case) as a refusal of its
last write. Every refusal of a setting write or of a command's arguments that is reachable
over NUS contains one of those words (2.0.0 fixed `ledmode` and `reactcolor`, which did
not), and no success reply contains them. Two runtime failures do not use them and so are
not shown as refusals by the app: `testmin`/`testmax` on a key whose power board is offline
(`  no board for that note …`) and a failed NVS open on `save` (`[settings] save FAILED …`).

## 6. Persistence and side effects (unchanged, but the app relies on them)

- The 30-second auto-save persists changes; the four presets save immediately. The app
  sends `save` when the user leaves its settings screen after changing something, so a
  piano powered off within 30 s still keeps the change.
- `volume` below 100 turns Full Power off; `panic` turns it on; `hold` clamps to
  50–4000. The app always re-reads after writing, so these side effects show up.
- `ledcount` changes the mapping live but the clock-out length only after a reboot; the
  reply already says so and the app shows it.

## 7. Acceptance (bench, with the app or `nRF Connect`)

1. Connect over MIDI as before; discover services: NUS is present; MIDI still plays.
2. Subscribe to TX, write `dump\n` to RX: the full list arrives, ending with `end`, in
   under 300 ms at MTU 255.
3. Write `ledbright 40\n`: serial and TX both show `  LED brightness = 40/255 (15%)`;
   `get ledbright` → `ledbright=40`.
4. Write `fire 0 0 4095\n` over NUS: refused; nothing fires. Same line over USB serial
   still works.
5. Send 50 lines in one write burst: no line lost, no watchdog trip, MIDI playback
   continues in time throughout.
6. Disconnect mid-`dump`: no crash; TX ring discarded; next connect starts clean.
7. Unsubscribe from TX and send `status`: serial still prints; nothing is queued.

## 8. Things found while reading, to fix in the same change

All fixed in 2.0.0 (and `ledtail` now accepts 0..255, the range `dump` reports, instead of
0..299 into a `uint8_t`; `decay`/`rainspeed` stay 1..40 and the AMOLED slider goes to 40).

- `testmax` behaves like `testmin`: `bool wantMax = (line[4] == 'a');` checks index 4,
  which is `m` in both words (`src/main.cpp` ~1041).
- `help` says `keyforce` takes 0.0..2.0; the code accepts 0..4.
- `decay` and `rainspeed` accept 1..40 over serial while the AMOLED slider and the
  `AppState` comments say 1..20; pick one.
- `pedalon 0` calls `pedal_release()` after clearing the flag, so the servo is not
  driven up; release first, then clear.
- `settings.h` says settings save on BLE disconnect; they do not (`main.cpp` ~1496).

## 9. Feature detection on the app side

The app treats the piano as "settings-capable" only if NUS is present **and** `dump`
returns `!proto=1 … end` within 2 s. Older firmware (no NUS, or `unknown command`)
keeps working exactly as before; the app simply hides its piano-settings section.
