# Fire-Safety Architecture

This document explains the layered protections that keep a solenoid from ever
being left energized ("stuck on"). A stuck coil at 24 V drives its MOSFET to
~80 °C within 5 minutes and is a genuine fire risk on a wooden piano in a
public space. **Read this before modifying anything in the dispatch path**
(`src/power_boards.cpp`, `src/pca9685.cpp`).

## The core invariant: verified release

> A channel is only ever considered OFF after an I²C off-write is
> **confirmed acknowledged**. An unverified "off" is treated as still-ON.

Why: the dominant real-world failure is a *silently failed* I²C write —
solenoid switching noise corrupts the bus exactly when the piano is busiest.
Early firmware cleared its bookkeeping after *sending* an off, so one glitched
write disarmed every safety net while the coil stayed energized. Every release
now flows through `tryChannelOff()`:

- **Success** → channel state cleared (`s_onAt = 0`).
- **Failure** → channel flagged `s_needOff`; the watchdog re-attempts the off
  **every 10 ms** until one is confirmed.

`PCA9685::setPWM/allOff/begin` all return write-success for this reason —
never ignore those return values.

## The layers (independent; any one alone prevents a fire)

| # | Layer | Trigger | Bound |
|---|---|---|---|
| 1 | Normal NoteOff dispatch | MIDI/touch release | immediate |
| 2 | `s_needOff` fast retry | any failed off-write | retries at 100 Hz |
| 3 | Hold watchdog (`g_maxHoldMs`) | any channel energized too long (orphaned NoteOn, dropped BLE packet) | 2 s default, **hard-clamped ≤ 4 s** via `set_max_hold_ms()` — every config path (serial, GUI, NVS load) goes through this clamp |
| 4 | ALLCALL escalation | off-writes keep failing (4 consecutive passes) | bus-wide FULL_OFF broadcast to address 0x70 (every PCA9685 listens, even mis-addressed ones) + forced bus recovery if the broadcast itself fails |
| 5 | Hardware task watchdog | firmware hang > 8 s | reboot → boot sequence broadcasts FULL_OFF ×3 before anything else runs |
| 6 | Crash-loop safe mode | 3 crash-reboots in a row (panic/WDT/brownout only — not power cycles) | forces Full-Power mode (no PWM → no EMI → no crash source) |
| 7 | MIDI panic (CC 120/121/123) & BLE-disconnect stop | app stop button / connection loss | full release, executed on the dispatch core with the event queue flushed so no queued note can re-fire after the stop |
| 8 | Human | EMERGENCY STOP button (GUI) / `panic` / pulling the 24 V XT60 | immediate |

## Rules the code enforces (and you must preserve)

1. **Never clear state on an unverified write.** Includes broadcasts: an
   ALLCALL "success" is a bus-level ACK — it proves nothing about a chip
   that has dropped off the bus, so blanket clears skip offline boards.
2. **Timers that energize are always cleared on any stop** (`allKeysOff`
   wipes pending fires, deferred releases, cushions, and re-strike holds) —
   otherwise a queued strike could re-energize a coil *after* an emergency stop.
3. **Every energize arms the watchdog** (`s_onAt` timestamped) — including
   manual/test paths (`fire`, `rawnote`). Channels with no watchdog slot
   (unwired PCA outputs 7–10) **refuse to energize at all**.
4. **Feature timers may not extend the thermal budget.** Example: auto
   re-strike re-arms `s_onAt` on every hit, which would blind layer 3 — so it
   enforces the same `g_maxHoldMs` bound itself from the *first* press
   (`heldSinceMs`, deliberately never reset by re-strikes or repeated NoteOns).
5. **Offline boards stay armed.** A board that stops ACKing while a coil is
   believed on keeps its flags; the 5 s health tick force-recovers the bus,
   attempts a per-chip verified all-off, and only a **verified** all-off or a
   verified re-init (`begin()`, which writes FULL_OFF *before* waking the
   chip and checks it landed) clears that board's state.
6. **The safety ticks run before note dispatch** at the top of `loop()`,
   and the per-loop dispatch drain is capped — a flood of incoming notes can
   never starve the release watchdog.
7. **Cushion current counts as ON.** Soft-release's holding current is
   watchdog-covered like a full strike.

## Why Full-Power mode is the installation default

PWM velocity mode switches every active coil at the PCA9685 carrier
frequency; the resulting back-EMF/EMI is what corrupted the I²C bus and
crashed early hardware revisions under dense songs. Full-Power mode drives
plain DC — no switching, no EMI, no hum — and with fixed force the rest of
the musicality comes from the timing features. Velocity/PWM mode is safe to
use on the opto-isolated board revision, but if you ever see `i2cFails`
climbing in `status` during play, that is the EMI storm meter: switch to
Full-Power and investigate the hardware (flyback diodes, supply filtering,
isolation) before trusting PWM again.

## If you modify dispatch code

Re-verify at minimum:
- every new `setPWM(..., 0)` call site checks the return or routes through
  `tryChannelOff`;
- every new energize path stamps `s_onAt` (or refuses, like unwired channels);
- every new deferred/queued action is cleared in `allKeysOff()`;
- nothing new can hold a coil past `g_maxHoldMs` from first energize;
- then run a full `sweep` on hardware and confirm zero stuck keys, and watch
  `status` → `i2cFails` stays flat during a dense test song.
