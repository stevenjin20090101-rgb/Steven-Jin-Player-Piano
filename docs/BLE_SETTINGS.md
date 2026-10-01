# Bluetooth settings channel — spec for the firmware

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

- `NimBLEDevice::createServer()` / `getServer()` return the one shared server, so the
  NUS service can be created right after `MIDI.begin(...)` in `setup()`. Creating a
  service after the server has started triggers `serviceChanged()` → `resetGATT()`,
  which is harmless at boot (no client connected yet) but restarts advertising; do it
  before the firmware's own `advertising->start()` call.
- Keep the library's `MyServerCallbacks` as the server callback (there is one slot; the
  firmware's connect/disconnect handling depends on it). Use *characteristic* callbacks
  for NUS RX and for the TX CCCD subscribe.
- The connect/disconnect stop logic stays exactly as it is: any disconnect still runs the
  full all-off. NUS shares the single connection with MIDI; the app opens one link.

## 2. Threading: never parse on the Bluetooth task

`onWrite` for NUS RX runs on the NimBLE host task (core 0). It must **only append the
received bytes to a FreeRTOS byte queue** (≥ 512 bytes; drop with a counter when full,
never block). The main `loop()` drains that queue into the same line machinery
`pollSerial()` uses (`s_lineBuf[80]`, end on `\n` or `\r`, ≤ 8 lines per pass) and calls
`handleLine()` on core 1, exactly as serial input does. Solenoid dispatch, I²C, LEDs and
the safety watchdogs must stay on the loop.

## 3. Output: a tee that mirrors console replies to NUS TX

`handleLine()` and the functions it calls print replies straight to `Serial`. Add a
`Print` subclass (a tee) whose `write()` goes to `Serial` **and**, while an NUS client is
subscribed to TX, into a TX ring buffer (≥ 4 KB; `help` is ~4 KB). Route every reply
that a command produces through it: the `  …` reply lines in `handleLine`, the
`[settings]`, `[vol]`, `[pedal]` and `[test]` lines printed by the functions it calls,
and the `> line` echo (the app ignores lines starting with `> `). Heartbeats, boot
banners and debug chatter stay on serial only.

The loop flushes the TX ring as notifications: chunks of at most `MTU − 3` bytes, at
most 8 notifications per loop pass, in order, lines ending in `\n` (a chunk may split a
line; the app reassembles). When no client is subscribed, the ring is discarded.

## 4. Two new commands: `dump` and `get`

Every reply today is free text written for people. The app needs one parseable form.

**`dump`** prints one line per tunable, `name=value`, then a final line `end`. The
`name` is the **command name that sets it**, so the app can write back with
`name value`. Values: integers as decimal; floats with two decimals; booleans `0`/`1`.
Read-only facts are prefixed `!`. Exact list and order:

```
!proto=1
!fw=<git short hash or build date>
!ota=<none|pending|confirmed>      (the running image's update state, see BLE_OTA.md section 9: none = flashed over USB, pending = not yet confirmed, confirmed = self-test passed)
!ble=1
!boards=OK,OK,MISSING,OK,OK,OK,OK          (7 entries, MIDI 24-35 first)
!i2cfails=<n>
!pedalboard=absent|online
!uptime=<seconds>
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

**`get <name>`** prints the single `name=value` line for any name in the list above
(including the `!` facts), or `  unknown setting` for anything else. The app uses `get`
after each write to show what the piano actually holds, and `dump` on connect and after
a preset.

Both commands are plain console commands: they also work over USB serial and are
listed in `help`.

## 5. Allow-list over Bluetooth

Everything in `dump` can be set over NUS with `name value`, and these commands are
allowed as well: `status`, `dump`, `get`, `help`, `off`, `panic`, `save`, `ledtest`,
`testmin`, `testmax`, `note`, and the presets `soft`, `cinematic`, `expressive`,
`snappy`.

Refused over NUS with the reply `  refused over Bluetooth — use the USB console`:
`fire`, `rawnote`, `sweep`, `pwm`, `ramp`, `reset`, `blereset`, `pedaltest`, `keyforce`,
`keyforce_all`, `keyforce_white`, `keyforce_black`, `waterfall`. (Bench and destructive
commands stay at the bench; per-key force is not in scope for the app yet.)

Implement the check in the NUS path before `handleLine` (a `fromBle` flag); the serial
path is unchanged.

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
