# dispatch-sim — host-side test for the solenoid dispatch path

Compiles the real [`src/power_boards.cpp`](../../src/power_boards.cpp) on your
Mac/Linux box against stub Arduino/Wire/FreeRTOS headers, swaps the PCA9685
driver for simulated chips, and drives NoteOn/NoteOff streams through
`dispatchNoteOn` / `dispatchNoteOff` with a fake clock and the same ~100 Hz
safety-tick cadence as `loop()`. No hardware, no PlatformIO.

The number that matters is **`longest_energized`**: the longest stretch a coil
stayed on without a *verified* OFF write landing. It must never exceed
`g_maxHoldMs` (2 s default). See [`docs/SAFETY.md`](../../docs/SAFETY.md).

```bash
cd firmware/tools/dispatch-sim
make check                 # every scenario; non-zero exit on any violation
SIM_VERBOSE=1 ./sim grace  # timeline of one scenario (source events + coil transitions)
```

Scenarios (`sim.cpp`): the rapid same-key pattern that used to merge notes and
defeat the hold ceiling (`rapid`, `rapid_snappy`, `rapid_failoff`), the
musical figures it must still play (`grace`, `short_repeat`, `case1_gap`), the
ceiling under repeated `fire` commands (`manual_refire`), and plain regression
checks (`rearticulate`, `long_note`, `orphan`). Add a scenario whenever you
touch the dispatch path.

The firmware-update hooks (SAFETY.md rule 9, [`docs/BLE_OTA.md`](../../docs/BLE_OTA.md) §7)
have their own three:

- `inhibit` — five coils on board 3 energized, then `set_energize_inhibit(true)`
  and 2.5 s of NoteOns, `fire`, `rawnote`, held-note re-strike ticks and sweeps
  on every board. Every attempt must be refused with **zero non-zero PWM
  writes**, and each coil must still be released by the path that owns it: the
  hold watchdog (through a glitched OFF and its retry), the re-strike lift, a
  NoteOff (straight to OFF, no cushion), a deferred release, `allKeysOff()`.
- `verified_off` — strike, then `allKeysOff()` while every OFF write fails:
  `coilsVerifiedOff()` stays false until the watchdog's retry lands, and
  `lastEnergizeMs()` is the strike. Then a coil on a board that dropped
  offline: false until the health tick's verified per-chip all-off.
- `verified_off_pending` — every coil verified off, yet a deferred strike, a
  cushion the watchdog overtook, an OFF still owed, or a held note between
  re-strikes keeps it false until cleared.

And the bring-up sweep (SAFETY.md rule 3):

- `sweep` — a full `sweep` over all seven boards (84 keys, 19.3 s) while OFF
  writes fail twice: across two of board 2's releases in mid-sweep, and across
  the last two releases and the sweep's closing all-offs until 300 ms after it
  returns. Each strike must stamp `lastEnergizeMs()` (it goes through
  `armOnAt()`), the first two coils must be released by the watchdog the sweep
  runs between keys once the bus is back, the last two by the loop's 10 ms
  retry, none past the hold ceiling, and `coilsVerifiedOff()` must say false
  while any of them is on. The sweep keeps its timing: one strike per key,
  150 ms on, 80 ms off.

Every scenario also asks `coilsVerifiedOff()` on every simulated millisecond
(inside the sweep too, where no safety tick runs) and fails if it ever says
"all off" while a simulated channel is on.

Limits: a perfectly regular tick, chips that always answer (an offline board is
its `connected` flag cleared), and a fault model that can only fail OFF writes,
in up to two time windows (it also counts every non-zero write). The
original ten scenarios watch one note (C4 / board 3 / channel 0). It exercises
the state machine, not the I²C layer.
