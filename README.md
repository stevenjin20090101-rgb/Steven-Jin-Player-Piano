# Player Piano — ESP32-S3 Self-Playing Piano

Firmware and control software for a **self-playing acoustic upright piano**: 84 solenoids press the real keys, driven from an ESP32-S3 with a touchscreen, controlled by **Bluetooth MIDI** (play any song from Synthesia/GarageBand on an iPad), an **on-screen touch keyboard**, or a **web-based control panel** that can also play AI-generated MIDI files.

Built as a permanent interactive installation. **Made by Steven Jin** (firmware, integration, install); power-board electronics designed with a hardware collaborator.

📊 **[Visual overview with diagrams →](docs/OVERVIEW.md)** — architecture, note flow, lighting, and safety, explained with pictures.

---

## How it works

```
 iPad (Synthesia)          LilyGo T4-S3 (ESP32-S3 + 2.41" AMOLED touch)
      │  BLE MIDI               │
      ▼                         │  I²C (opto-isolated trunk over shielded cable)
 ┌───────────┐   notes    ┌─────┴──────┐     ┌──────────────────────────────┐
 │ BLE stack │ ─────────► │  dispatch  │ ──► │ 7 × PCA9685 power boards      │
 └───────────┘            │  + safety  │     │ (0x40–0x46, one octave each)  │
 Touchscreen piano ─────► │  watchdogs │     │  → MOSFETs → 24 V solenoids   │
 Web GUI (USB serial) ──► └────────────┘     │  → 84 piano keys (C1–B7)      │
                                             └──────────────────────────────┘
```

- Each **power board** carries one PCA9685 16-channel PWM chip driving 12 MOSFET+solenoid channels (one chromatic octave). Board address is set by a 6-position DIP switch.
- The **master board** hosts the T4-S3 module. Two separate I²C buses: the module-internal one (touch + PMU) and a **dedicated, opto-isolated bus** for the power boards — solenoid switching noise physically cannot reach the display/touch bus.
- A solenoid only sounds when it **strikes** — all timing features below exist to make real music physically playable on solenoids.

## Hardware

| Part | Role |
|---|---|
| LilyGo T4-S3 (ESP32-S3, 2.41" AMOLED touch) | Master controller + UI |
| 7 × custom power boards (PCA9685 @ 0x40–0x46) | One octave of solenoid drivers each |
| Logic-level MOSFETs (IRLZ44N class) | 24 V solenoid switching, flyback diodes per coil |
| 24 V / 40 A PSU (XT60 connectors) | Solenoid power (~0.5 A per active solenoid) |
| Shielded Cat7 trunk (RJ45) | I²C + 5 V + OneWire between boards |
| Opto-isolation + supply filtering on the I²C trunk | Kills the EMI path that crashed early revisions |
| WS2812B strip (BTF-LIGHTING, 5 V, 300 LED, IP65) | Optional lighting |

### Master-board pinout (`BOARD_REV` in `include/config.h`)

| Signal | `BOARD_REV 1` (current, isolated) | `BOARD_REV 0` (legacy shared-bus) |
|---|---|---|
| I²C SDA → power boards | **GPIO 42** (Wire1) | GPIO 6 (shared Wire) |
| I²C SCL → power boards | **GPIO 40** (Wire1) | GPIO 7 (shared Wire) |
| WS2812B data | **GPIO 41** | GPIO 41 |
| DS18B20 OneWire | GPIO 21 | GPIO 21 |

Set `BOARD_REV` to match the physical board — a mismatch produces "BLE works, piano silent" (all boards `MISSING`).

### Power-board addressing

DIP switch sets PCA9685 address bits A0–A2 (A3–A5 off). Boards are 0x40 (octave C1–B1) through 0x46 (C7–B7). **Note:** on the current PCB revision the DIP silkscreen is mirrored — verify with the I²C scanner tool when setting addresses.

## Firmware features

**Playing**
- **BLE MIDI peripheral** ("Steven Piano") — pair from inside any iOS MIDI app (Synthesia → Settings → Music Devices). Auto re-advertises on disconnect; the iPad reconnects by itself.
- **Touch piano** on the AMOLED with swipe-glissando, live key highlighting, and a last-notes log.
- **Web GUI** (`gui/piano-control.html`) over USB Web Serial — every tunable as a slider, a **MIDI-file player** (drop any `.mid`, e.g. AI-generated, and the piano plays it), a **strike tester** (sweep force × duration per key), and a big **EMERGENCY STOP**.
- **MIDI panic support** — CC 120/121/123 (All Sound/Notes Off) release everything instantly.

**Making solenoids musical**
- **Velocity → PWM force mapping** (min/max/multiplier/per-key trim), or **Full-Power mode** (every note max force, zero PWM hum — the reliable default).
- **Retrigger gap** — rapid same-key repeats get micro-spaced so the plunger can physically reset (fast trills all sound).
- **Min-strike duration** — ultra-short MIDI notes are stretched just enough to actually strike (no more "forgotten" notes).
- **Soft release** — brief cushion current on release so keys don't clank on the backstop.
- **Auto re-strike (tremolo sustain)** — optionally re-hits held notes every N ms so sustained notes stay audible next to busy passages.

**Reliability & self-healing** (see [`docs/SAFETY.md`](docs/SAFETY.md) for the full design)
- Verified-release invariant: a solenoid is only ever considered "off" after a **confirmed** I²C write; failed releases retry at 100 Hz and escalate to a bus-wide broadcast.
- Bounded hold: no coil can be energized longer than the clamped hold timeout (≤ 4 s) through **any** code path.
- Hardware watchdog (8 s) + boot-reason logging + crash-loop safe mode + triple all-off broadcast at every boot.
- **Hot-reconnect**: power boards that are missing/dropped are re-probed every 5 s and come back online automatically.
- I²C fault counter, per-transaction 5 ms timeout, rate-limited bus recovery — an EMI glitch storm degrades gracefully instead of freezing playback.
- All settings persist in NVS flash and survive power-off and re-flash.

**Comfort**
- Gradual idle dimming: screen fades down after 60 s of no notes/touch, wakes instantly on activity (`dimsecs`).
- Apple-style pull-down panel for brightness + full-power toggle on the touchscreen.

## Building & flashing

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
# from the repo root
pio run                 # build
pio run -t upload       # flash over USB-C
pio device monitor      # serial console @115200 (type 'help')
```

1. Check `include/config.h` → `BOARD_REV` matches your master board.
2. If upload can't connect: hold **BOOT**, tap **RST**, release **BOOT**, retry.
3. First boot: watch the serial log — all 7 boards should report `OK` and the I²C scan should list `0x40–0x46`.

## Serial console (or type into the web GUI)

| Command | Purpose |
|---|---|
| `help` / `status` | Command list / full current state (boards, tunables, i2c fault count) |
| `off` / `broadcast` / `panic` | Release everything (escalating levels; panic also forces Full-Power) |
| `note <midi> <vel>` | Play through the full dispatch path (vel 0 = release) |
| `rawnote <midi> <pwm>` | Strike-tester: raw force 0–4096, bypasses velocity mapping |
| `fire <board> <ch> <pwm>` | Drive one PCA9685 channel directly |
| `sweep` / `pwm <n>` | Walk every key once (bring-up test) / set sweep force |
| `min` `max` `velmult` `keyforce[_all/_white/_black]` | Velocity-curve shaping |
| `gap` `minstrike` `restrike` `softrelease` `releasepwm` `releasems` | Timing feel |
| `fullpower 0\|1` | Max-force no-PWM mode (recommended ON) |
| `freq <24..1526>` | PCA9685 PWM carrier (hum pitch) |
| `hold <ms>` | Safety auto-release timeout (hard-capped at 4000 ms) |
| `leds 0\|1` / `keyviz 0\|1` / `dimsecs <s>` | LED strip / key highlight / idle dim |
| `snappy` | One-shot lowest-latency preset (recommended for dense songs) |
| `save` / `reset` | Persist now / factory-wipe settings |

## Web GUI

1. Open `gui/piano-control.html` with VS Code **Live Server** (Web Serial needs localhost) in **Chrome/Edge**.
2. Click **Connect**, pick the `usbmodem` port. Sliders sync from the device.
3. Only one program can hold the USB port — disconnect the GUI before re-flashing.

**MIDI player:** drop a `.mid` file, press ▶. Notes outside C1–B7 fold in by octave; stop/pause always release every key. Works with any AI music tool that can produce MIDI.

## Tools

- [`tools/i2c-scanner`](tools/i2c-scanner) — standalone bus scanner; use when setting DIP addresses or debugging the trunk. Also broadcasts all-off at boot (frees any solenoid stuck by a previous crash).
- [`tools/led-test`](tools/led-test) — minimal WS2812B color-cycle on GPIO 18+41 simultaneously to validate strip wiring independent of everything else.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| BLE receives but piano silent, boards `MISSING` | Power-board side unpowered, trunk unplugged, or `BOARD_REV` mismatch. Boards auto-join within 5 s of power returning. |
| Crashes only in PWM mode under heavy songs | Solenoid switching EMI — use Full-Power mode; the permanent fix is the opto-isolated board + flyback diodes/filtering. |
| USB port not appearing on the computer | Use a known **data** cable, then BOOT+RST into download mode. |
| A key stuck pressed | Should self-release ≤ hold timeout (2 s default). If ever not: EMERGENCY STOP in the GUI / `panic` / pull the 24 V XT60. Persistent single-channel stick = that channel's driver hardware. |
| One key never plays | `status` → board `OK`? Then `rawnote <midi> 3000` — if silent, check that channel's wiring/MOSFET. |

## Repo layout

```
├── src/                  firmware (dispatch, safety, UI, settings, drivers)
├── include/              config.h (pins/BOARD_REV/tunables) + headers
├── gui/piano-control.html  web control panel + MIDI player (Web Serial)
├── tools/                standalone bring-up utilities (PlatformIO projects)
├── docs/SAFETY.md        fire-safety architecture — read before modifying dispatch
├── lv_conf.h             LVGL configuration
└── platformio.ini        build config (LilyGo T4-S3)
```

## Pedal node (in design)

A sustain-pedal actuator is being added: MIDI CC64 drives a closed-loop stepper
that presses the piano's damper pedal, so pedalled passages ring and blend.
Firmware support is already in (`src/pedal.cpp`); the hardware is optional and
absent = no-op. See [`docs/PEDAL_NODE_PCB.md`](docs/PEDAL_NODE_PCB.md) for the
board specification — RJ45 + XT60 daisy chain, power chain, and the grounding
rules that keep stepper current out of the I²C reference.

## Authorship & license

**Made by Steven Jin** — firmware, LED engine, control software, and integration.
Licensed under the [MIT License](LICENSE): you're welcome to learn from and build
on this, but the copyright and attribution notice must be preserved.

This project carries a layered authorship watermark so the work can't be passed
off as someone else's:

- **Copyright headers** in every source file, plus [`LICENSE`](LICENSE) and [`AUTHORS`](AUTHORS).
- **A watermark compiled into the firmware binary** — even a copied `.bin` reveals
  the origin: `strings firmware.bin | grep PPFW`.
- **A cryptographic Ed25519 signature** over the whole source tree —
  see [`PROVENANCE.md`](PROVENANCE.md) to verify that Steven Jin authored these
  exact files (`provenance/verify.py`).

## Safety notice

This machine switches real current into 84 coils bolted to a wooden instrument. A stuck-on solenoid heats its driver to ~80 °C within minutes. The firmware layers multiple independent protections (see [`docs/SAFETY.md`](docs/SAFETY.md)) — **keep them intact when modifying dispatch code**, keep the 24 V connector reachable as the ultimate kill switch, and re-run a full-key `sweep` after any hardware change.
