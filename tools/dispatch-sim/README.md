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

Limits: one note (C4 / board 3 / channel 0), a perfectly regular tick, and a
fault model that can only fail OFF writes. It exercises the state machine, not
the I²C layer.
