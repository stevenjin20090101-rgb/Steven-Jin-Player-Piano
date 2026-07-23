# Player Piano — Project Overview

A visual guide to how the self-playing piano works, end to end. For build/flash
steps see the [README](../README.md); for the fire-safety design see
[SAFETY.md](SAFETY.md).

> **Made by Steven Jin.** Firmware, LED engine, control software, and
> integration. See [PROVENANCE.md](../PROVENANCE.md) for cryptographic
> authorship verification.

---

## What it is

A real acoustic upright piano that plays itself. 84 solenoids press the actual
keys, driven by an ESP32-S3 with a touchscreen. You can feed it music three
ways — stream **Bluetooth MIDI** from an iPad (Synthesia, GarageBand), tap the
**on-screen keyboard**, or drop a **MIDI file** into the web control panel. A
73-LED strip lights up in sync with the keys.

---

## System architecture

```mermaid
flowchart LR
    iPad["iPad / phone<br/>(Synthesia, GarageBand)"] -->|BLE MIDI| ESP
    Touch["On-screen<br/>touch keyboard"] --> ESP
    GUI["Web GUI<br/>(USB Web Serial)"] -->|note / MIDI file| ESP
    subgraph ESP["ESP32-S3 (LilyGo T4-S3)"]
        direction TB
        Dispatch["Note dispatch<br/>+ fire-safety watchdogs"]
        LEDs["LED engine<br/>(reactive / palettes)"]
    end
    Dispatch -->|"I²C (opto-isolated trunk)"| Boards
    LEDs -->|"WS2812 (GPIO41,<br/>through isolator)"| Strip["73-LED strip"]
    subgraph Boards["7 × PCA9685 power boards (0x40–0x46)"]
        direction TB
        PCA["PCA9685 PWM"] --> MOS["MOSFETs"] --> Sol["84 solenoids"]
    end
    Sol -->|press| Keys["Real piano keys C1–B7"]
```

- **Master board** hosts the T4-S3. It runs two *separate* I²C buses: the
  module-internal one (touch + power management) and a **dedicated,
  opto-isolated** bus for the power boards — so solenoid switching noise
  physically cannot reach the display.
- Each **power board** is one PCA9685 chip driving 12 MOSFET + solenoid
  channels — one chromatic octave. Address set by a DIP switch.

---

## What happens when a note plays

```mermaid
sequenceDiagram
    participant Src as MIDI / touch
    participant Q as Event queue
    participant D as Dispatch (core 1)
    participant PCA as PCA9685
    participant LED as LED strip
    Src->>Q: NoteOn(note, velocity)
    Q->>D: drain (capped per loop)
    D->>D: velocity → PWM force<br/>(or full-power)
    D->>PCA: I²C setPWM(channel, force)
    PCA-->>D: ACK (verified)
    D->>LED: light note's LED (same instant)
    Note over D,PCA: watchdog arms s_onAt<br/>(coil must release ≤ 4 s)
    Src->>Q: NoteOff(note)
    Q->>D: drain
    D->>D: min-strike? isolated-note boost?
    D->>PCA: setPWM(channel, 0) — verified
    D->>LED: begin fade (or hold if key still down)
```

The LED is lit **at the exact instant the solenoid fires** — not from the
display loop — so lights and sound never drift apart.

---

## Making solenoids musical

Solenoids are basically on/off; these features turn them into an instrument:

| Feature | What it solves |
|---|---|
| Velocity → PWM force | Soft *pp*, swelling *ff* dynamics |
| **Isolated-note strike boost** | A lone staccato note is held longer so it actually sounds; notes inside a run stay short and fast |
| Retrigger gap | Rapid same-key repeats get micro-spaced so the plunger resets |
| Min-strike duration | Ultra-short notes are stretched enough to strike |
| Soft release | Cushion current on release so keys don't clank |
| Auto re-strike | Optional tremolo so held notes keep singing |
| **Feel presets** | 🎬 *Cinematic* (soft/expressive) and ⚡ *Snappy* (fast/dense) set everything at once |

---

## Lighting engine

The 73-LED strip maps the played key range onto physical LEDs, live-calibratable
so each lit LED sits under its key.

```mermaid
flowchart LR
    Note["MIDI note<br/>C1–B7"] --> Map["note → LED<br/>offset · scale · reverse"]
    Map --> Tail{"past last<br/>solenoid key?"}
    Tail -->|yes| Dark["stay dark<br/>(keys-end trim)"]
    Tail -->|no| Pal["palette + glow +<br/>velocity brightness"]
    Pal --> Hold{"key held?"}
    Hold -->|yes| Sustain["stay lit"]
    Hold -->|no| Fade["fade out"]
```

- **8 palettes:** rainbow, solid, velocity, fire, ocean, forest, lava, party.
- **Calibration:** two-point — light the lowest key's LED and trim *Offset*,
  then the highest and trim *Scale*. Everything between lines up.
- **Keys-end trim:** LEDs past the last solenoid key stay dark, so *lit = plays*.
- **Sustain:** held keys keep their LED lit; it fades on release.

---

## Reliability & fire-safety (summary)

A stuck-on solenoid is a real fire risk, so protection is layered — see
[SAFETY.md](SAFETY.md) for the full design.

```mermaid
flowchart TD
    A["Coil energized"] --> B{"released<br/>normally?"}
    B -->|yes, verified I²C| OK["off"]
    B -->|write failed| R["retry at 100 Hz"] --> B
    A --> W["hold watchdog ≤ 4 s"] --> OK
    A --> C["crash / reboot"] --> E["boot: ALLCALL FULL_OFF<br/>FIRST, before display"] --> OK
    A --> H["firmware hang"] --> WD["8 s hardware watchdog<br/>→ reboot → all-off"] --> OK
    A --> HUM["last resort:<br/>pull 24 V XT60"] --> OK
```

Every release is **verified** (a coil is only "off" after a confirmed I²C ACK),
no coil can stay energized past ~4 s through any path, and a crash releases
everything within ~150 ms of reboot — before the display even initializes.

---

*Player Piano — © 2026 Steven Jin. Licensed under the MIT License.*
