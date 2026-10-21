# 24 V cut-off — the controller holds the solenoid supply on, and anything else drops it

**Status: design, 2026-10-21 — hardware for 吟晚风, firmware 2.2.0 to follow once the board exists.**
Steven's decision after a stuck coil was found hot with the piano idle: a cut-off relay first, the
RS-485 trunk (`docs/RS485_TRUNK.md`) after.

**Interim, in use first (2026-10-21):** a Bluetooth smart plug under the tablet's control does this job
until the relay board exists — `docs/SMART_PLUG.md`. The rule below is the same; only who holds the rail on
differs (the tablet instead of the controller).

## What happened, and why firmware alone cannot fix it

A key was found held down with its coil hot and nothing playing. The controller had been dark or
rebooting, and at one point **unpowered while the 24 V rail stayed on**. The PCA9685 on each power
board keeps its last register state for as long as *it* is powered: a coil that was on when the
controller died, hung, or lost its bus stays on until someone pulls the plug. Every software layer in
`SAFETY.md` runs on the controller and talks over the trunk — none of them can act when the controller
is dead or the trunk is gone. That is the hole. The only thing that closes it is a supply that needs a
**live, healthy controller** to stay on.

## The rule

> The 24 V solenoid rail is on **only while the controller keeps sending a heartbeat**. A static level —
> stuck high or stuck low — must not hold it on. No heartbeat for ~0.5 s, for any reason, cuts the rail.

Consequences, all wanted:

| Situation | Today | With the cut-off |
|---|---|---|
| Controller unpowered, 24 V on | coils stay as they were | 24 V off (no heartbeat ever) |
| Controller crashed / hung (task watchdog, panic) | coils stay on until the reboot's all-off | 24 V off within 0.5 s |
| Trunk or isolator dead, coil believed on | OFF retries fail for ever | firmware stops the heartbeat → 24 V off |
| Board off the bus with a coil believed on | flagged, never released | same |
| Boot, before the first all-off | PCA9685s drive whatever they held | 24 V off until the firmware is up |
| Firmware update over Bluetooth | coils verified off, inhibit set | 24 V off for the whole session as well |

## Hardware (one small board, between the 24 V supply and the XT60 chain)

```
24 V PSU ──► F1 ──► RELAY K1 (NO contact, ≥ 20 A) ──► XT60 out to the power-board chain
                        │
   controller GPIO ──► C1 ──┤►│ D1 ──┬──► gate of Q1 (N-MOS, logic level) ──► K1 coil (24 V) ──► 24 V
   (heartbeat 100 Hz)      │►│ D2  C2 ‖ R1 (≈ 0.47 s)          └── D3 flyback across the coil
                           ▼ GND
```

- **Heartbeat detector:** C1 (100 nF) couples the GPIO's edges; D1/D2 (1N4148) rectify them into C2
  (10 µF) ‖ R1 (47 kΩ): each edge tops C2 up, R1 drains it in about half a second. Q1 conducts only while
  C2 is charged, so the relay holds **only while edges keep coming**. A GPIO stuck high, stuck low, or
  floating cannot hold it. (This is the classic charge-pump / missing-pulse detector; a 74HC123 retriggerable
  monostable with a 0.5 s period is an equally good alternative.)
- **Relay:** 24 V coil, contacts rated for the solenoid rail (the whole chain; size for the PSU's limit,
  ≥ 20 A), DC-rated. D3 flyback across the coil. An automotive 40 A relay is fine; a DC solid-state relay
  needs its own flyback handling and is not required.
- **Isolation:** the heartbeat GPIO comes through the same kind of digital isolator the trunk uses
  (ISO7721-class), so the relay side shares no ground with the controller.
- **Indicator:** an LED across the relay coil ("24 V enabled"), visible on the board.
- **Manual bypass: none.** A bypass switch would recreate the fault. For bench work the firmware's `power on`
  / `power off` commands drive the heartbeat.
- **Fuse F1** before the relay, as today. The existing XT60 daisy chain is unchanged downstream.
- **Grounds:** the relay board sits on the PSU side; its only link to the controller is the isolated
  heartbeat line. Keep PGND/AGND discipline as in `PEDAL_NODE_PCB.md`.
- **Controller pin:** one free header GPIO (proposal: GPIO 16 — 吟晚风 to confirm against the T4-S3; not 18,
  which is tied to the display's TE line). Firmware name `POWER_HB_PIN`.

## Firmware (2.2.0): what the heartbeat means

- **The heartbeat is software, never a hardware PWM.** It is toggled from the safety tick at the top of
  `loop()` (100 Hz): if `loop()` stops, so does the heartbeat. That is the point.
- **It runs only while all of these hold:**
  1. the boot all-off has been sent and the power boards probed;
  2. no OFF write has been failing for more than 1,000 ms (`s_needOff` retries, `power_boards.cpp`);
  3. no board that is off the bus still has a coil believed on for more than 2,000 ms;
  4. no firmware update session is active;
  5. no `power off` / `panic` has latched it off;
  6. the energize path is not inhibited for a bench test (`i2cwave`).
- **Dropping it is logged and shown:** `[power] 24 V cut: <reason>`; the screen reads `24 V OFF • <reason>`
  in amber; `!power=on|off|latched` in `dump`, with `!powerdrops` since boot.
- **Coming back:** when the reason clears (the bus answers again, the board re-probes OK), the firmware
  waits 10 s, sends the all-off sequence, then restarts the heartbeat. Cutting the 24 V also resets the
  PCA9685s (they are powered from that rail), so they come back with every output off; `initPowerBoards()`
  configures them again, as the health tick does today.
- **Latching:** 3 drops within an hour → `latched`: the rail stays off until `power on` from the USB console
  or the app (a trusted connection, `BLE_LOCK.md`). Unattended, a persistent fault must not cycle the rail.
- **`panic`** latches it off as well as releasing everything. `power off` is the explicit bench kill.
- **Nothing else changes.** Every layer of `SAFETY.md` stays; the cut-off is the layer below them all.

## Bench checks, with 吟晚风

1. Heartbeat on → relay closes, LED on; GPIO held high → relay opens within 0.6 s; held low → same;
   controller unplugged → same.
2. `power off` → rail off at once; `power on` → all-off sequence, then rail on after 10 s.
3. Pull the trunk's RJ45 while a note is held: `[power] 24 V cut: OFF write failing`, rail off within
   ~1.5 s, the coil releases (it has no supply). Reconnect: boards re-probe, rail back after 10 s.
4. Reset the controller (RST) while a note is held: rail off within 0.5 s, back after the boot sequence.
5. Three pulls in an hour → `latched`; `power on` clears it.
