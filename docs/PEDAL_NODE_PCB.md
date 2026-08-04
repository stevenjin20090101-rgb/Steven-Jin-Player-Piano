# Pedal Node — PCB Specification

Design brief for the **sustain-pedal controller board**: a small node that hangs
off the existing daisy chain, receives pedal position over I²C, and drives an
MKS SERVO42C closed-loop stepper that presses the piano's sustain pedal.

Matches the house style of the rest of the system: **RJ45** for the signal
trunk, **XT60** for 24 V power, both daisy-chained through.

> Part of the Player Piano project. © 2026 Steven Jin — see [`../LICENSE`](../LICENSE).

---

## 1. What this board does

```
RJ45 in ──┬── I²C (SDA/SCL) ──► ESP32-C6 (I²C slave)
          └── RJ45 out (daisy chain)
                                      │ UART or STEP/DIR
XT60 in ──┬── 24 V ──► buck ──► 5 V ──┤
          └── XT60 out (daisy chain)  ├──► MKS SERVO42C ──► T8 screw ──► pedal
                                      └──◄ optical endstop (optional)
```

The master board sends one byte of pedal position (0 = fully up, 127 = fully
down). Everything else — homing, calibration, motion profiling, and the
comms-timeout failsafe — happens locally on this board.

---

## 2. Connectors

| Ref | Part | Purpose |
|---|---|---|
| J1 | RJ45 shielded, PCB mount | Signal trunk **in** |
| J2 | RJ45 shielded, PCB mount | Signal trunk **out** (daisy chain) |
| J3 | XT60 (male/female per house convention) | 24 V **in** |
| J4 | XT60 | 24 V **out** (daisy chain) |
| J5 | 4-pin JST-XH or screw terminal | To SERVO42C: UART or STEP/DIR + GND |
| J6 | 3-pin JST-XH | Optical endstop (VCC / GND / OUT) — **optional, fit anyway** |
| J7 | 2-pin | Spare / status LED / future second pedal |

### RJ45 pin allocation

Signal only — **no power on the RJ45.** Power comes in on XT60.

| Pin | Signal | Pair |
|---|---|---|
| 1 | SDA | 1 |
| 2 | GND (signal return) | 1 |
| 3 | SCL | 2 |
| 6 | GND (signal return) | 2 |
| 4, 5 | reserved / OneWire | 3 |
| 7, 8 | GND | 4 |

Each I²C line is paired with its own ground return — this is what keeps the
trunk clean over several metres of Cat7.

> ⚠️ **This must match the existing trunk allocation.** The current trunk carries
> I²C + 5 V + OneWire; confirm the mapping against the master-board schematic
> before committing. If the existing trunk already uses pins 4/5 for 5 V, either
> keep that convention and leave the 5 V unloaded here, or re-map the whole chain.

---

## 3. Power chain

```
XT60 24 V ──► F1 fuse ──► D1 reverse-polarity ──► TVS ──┬──► SERVO42C VIN (+ bulk cap)
                                                         └──► U1 buck ──► 5 V ──► ESP32-C6
```

| Ref | Part | Spec |
|---|---|---|
| F1 | Fuse | 2 A slow-blow (motor inrush) |
| D1 | Schottky SS34, or P-FET | Reverse-polarity protection |
| TVS1 | SMBJ30A / P6KE30A | Clamps inductive transients from the shared solenoid rail |
| C1 | 470–1000 µF, **35 V** electrolytic | Bulk, physically **next to the SERVO42C power terminal** |
| C2 | 100 µF, 50 V electrolytic | Buck input |
| U1 | **Murata OKI-78SR-5/1.5-W36-C** (7–36 V in, 5 V, 1.5 A) | Fixed output, encapsulated |
| C3 | 22 µF low-ESR + 100 nF | Buck output, near the C6 |

**Why 36 V input rating and not 28 V:** a nominal 24 V supply idles at 25–26 V,
and this rail also switches 84 solenoids. Headroom is not optional here.

**Why fixed output, not an adjustable module:** a trimpot that gets bumped or
drifts sends 24 V into the ESP32 and kills it instantly. If cost forces an
LM2596 module instead, set it, verify with a meter, then seal the pot.

**Do not fit a 5 V rail on the RJ45.** Powering the node from local 24 V draws
~115 mA over the cable instead of ~500 mA, so voltage drop down the chain is far
smaller.

---

## 4. Grounding — read this before routing

This is the part that decides whether the board works reliably or glitches every
time the stepper accelerates.

**The problem:** the stepper draws amps in fast pulses. If that return current
flows through the same copper the I²C uses as its voltage reference, SDA/SCL
shift relative to ground and the bus corrupts — the failure mode that already
cost this project a board revision.

**The rule:**

> Motor return current must go out the **XT60** and must never flow through the
> **RJ45** ground conductors.

**Implementation:**

1. **Two ground pours**, not one:
   - `PGND` — power ground: XT60 return, SERVO42C return, C1 bulk cap, buck input
   - `AGND` — signal ground: ESP32-C6, I²C, endstop, buck output, RJ45 pins 2/6/7/8
2. **Join them at exactly one star point**, at the **buck's output ground pad**.
   A single wide short link, or a 0 Ω jumper so it can be cut for debugging.
3. **Never** let the RJ45 ground pins connect directly to `PGND` — they land on
   `AGND` only, and reach `PGND` through the star point.
4. Keep the SERVO42C's high-current loop (XT60 → C1 → motor terminals → back)
   physically **tight and short**. Loop area is what radiates.
5. RJ45 shield: tie to chassis/`PGND` through a **1 MΩ ‖ 10 nF** network, not
   directly — avoids a shield ground loop down the daisy chain.

**Yes, all grounds are ultimately common** — a single voltage reference is
required for I²C and step signals to mean anything. The point is *where* they
become common: one deliberate star point, not a shared pour that motor current
happens to flow through.

**No additional isolator is needed on this board.** The trunk is already on the
field side of the master's ISO7721 barrier, so this node inherits that isolation.
Adding a second barrier here would create a third ground domain and require an
isolated supply for it. Handle the stepper's noise with layout and filtering
instead. *(Leave an ISO1540 footprint unpopulated if you want the option later.)*

---

## 5. ESP32-C6 interface

Suggested pin map — **confirm against the specific module and avoid strapping
pins (GPIO4, 5, 8, 9, 15 on the C6)**:

| Function | GPIO | Notes |
|---|---|---|
| I²C SDA | 6 | slave mode, to RJ45 pin 1 |
| I²C SCL | 7 | slave mode, to RJ45 pin 3 |
| UART TX → SERVO42C RX | 10 | UART1 — keep UART0 free for debug |
| UART RX ← SERVO42C TX | 11 | |
| STEP | 18 | alternative to UART mode |
| DIR | 19 | |
| ENABLE | 20 | **pull-down** so the motor is de-energized while the C6 boots |
| Endstop input | 21 | internal pull-up |
| Status LED | 2 | |

**I²C pull-ups:** 4.7 kΩ to 3.3 V, fit as DNP-optional. The master's pull-ups
serve the whole trunk; adding a set at every node over-loads the bus. Populate
only if this is the last node and the bus needs termination help.

### ⚠️ Level shifting

- **ESP32-C6 GPIOs are not 5 V tolerant.** Fit a 3-channel level shifter
  (e.g. TXS0108E) or per-line MOSFET shifters between the C6 and the SERVO42C
  **if** its inputs turn out to need 5 V. Many MKS boards have opto-isolated
  inputs that want ~5 V — verify in the manual before deciding to populate.
- **Endstop:** power it from **3.3 V**. If only a 5 V unit is available, power
  at 5 V and divide its output (10 kΩ / 20 kΩ) down to 3.3 V before the GPIO.

---

## 6. Homing strategy — support both

Fit the endstop connector **and** rely on the encoder. They do different jobs:

| Method | Job | Notes |
|---|---|---|
| **Optical endstop** (J6) | Absolute home reference at pedal-up | Precise, repeatable, immune to friction drift |
| **Position-error / stall** | Finds the pedal *contact point* during calibration; over-travel backstop | Encoder-based, so more reliable than back-EMF StallGuard |

Sensorless-only homing is viable — the SERVO42C's encoder detects a hard stop
directly. But it means driving the mechanism into its stop on every boot, at up
to ~500 N with a 2 mm lead, so home at **reduced current and low speed**.
Repeatability is a few steps rather than one, and it degrades as friction
changes over the years. For a permanent public install, the endstop is worth its
three wires.

**Hard mechanical stops at both ends of travel are mandatory** regardless. The
endstop is a sensor, not a barrier — it will not stop a runaway.

---

## 7. Firmware-side safety requirements

These are behaviours the board must physically permit:

1. **ENABLE pulled down by default** — a booting, crashed, or unpowered C6 leaves
   the motor de-energized. Combined with a non-self-locking screw, the pedal
   spring then returns the pedal up and the piano goes quiet.
2. **Comms-timeout failsafe** — if the C6 hears nothing from the master for ~2 s,
   it drives the pedal **up** on its own. Cable pulled, master crashed, trunk
   unplugged: the piano must never be left ringing.
3. **Watchdog timer** on the C6, same as the master board.

---

## 8. Layout notes

- Keep the motor loop (XT60 → C1 → J5 power) short and tight; minimise loop area.
- Route SDA/SCL as a pair, away from the motor loop and the buck's switch node.
- Keep the buck's switch node small — it is the loudest thing on the board.
- Bulk cap C1 goes at the SERVO42C connector, not at the XT60.
- Silkscreen: mark the XT60 polarity clearly, and label the star-ground jumper.
- Mounting: 4 × M3, with the board oriented so the XT60 leads don't cross the
  RJ45 run.

---

## 9. Open items to confirm before fabrication

1. **Existing trunk RJ45 pin allocation** (from the master-board schematic).
2. **SERVO42C control-input logic level** — 3.3 V direct, or 5 V needing a shifter.
3. **SERVO42C control mode** — UART position commands (preferred; no pulse train
   needed) vs STEP/DIR. Fit both headers so the choice can be made in firmware.
4. **Lead screw lead** — 2 mm (1-start) or 8 mm (4-start). Affects speed only;
   force is ample either way.
5. **I²C slave address** for this node (0x47 is free; power boards occupy
   0x40–0x46).

---

*Player Piano — © 2026 Steven Jin. Licensed under the MIT License.*
