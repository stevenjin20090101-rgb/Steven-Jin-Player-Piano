# Converting the trunk from I²C to RS-485 — plan

**Status: plan, 2026-10-21.** Steven's scope: keep the seven PCA9685 power boards; add a small **node
board** to each that drives the board locally over a few centimetres of I²C and talks to the controller
over RS-485. First the cut-off relay (`docs/SAFETY_CUTOFF.md`); then this, as firmware 3.0.0 plus a node
firmware.

## Why

I²C was never meant for metres of cable. Over the Cat7 trunk it has given EMI errors (`i2cfails`), boards
dropping off the bus, and — with nothing able to release a coil over a dead bus — the near fire. RS-485 is
a differential, terminated, multi-drop bus built for exactly this run: tens of metres at 1 Mbit/s with
noise rejection, and every frame checksummed. Moving the I²C onto the power board itself makes every write
local and verifiable, and a node that stops hearing the controller can switch its own outputs off.

## What the node does

```
RJ45 in ──┬── RS-485 A/B ──► transceiver ──► ESP32-C6 ──► I²C (on-board, cm) ──► PCA9685 ──► MOSFETs ──► coils
          └── RJ45 out (daisy chain)              │
XT60 in ──┬── 24 V ──► buck 5 V / 3.3 V ──────────┤
          └── XT60 out                            ├──► OE of the PCA9685  ◄── heartbeat detector (as the relay's)
                                                  └──► DS18B20 on the board, 24 V sense
```

- Receives frames addressed to it, writes the PCA9685, answers with an ACK that carries its status
  (channels on, board temperature, 24 V present). The controller's "a channel is off only after a confirmed
  write" rule survives: the ACK **is** the confirmation, and it is local I²C with its own ACK underneath.
- **Node time-out:** no valid frame from the controller for 500 ms → the node writes all-off and drives
  the PCA9685's **OE high** (every output off in hardware). A cut trunk, a dead controller or a stuck master
  all look the same to the node, and all end the same way.
- **Node hardware watchdog:** the node keeps OE low only through the same charge-pump detector as the
  24 V relay, fed by a heartbeat the node toggles from its main loop. A hung node drops OE by itself.
- **Local hold ceiling:** the node refuses to keep any channel on longer than 4 s (the controller's own
  ceiling is 2 s by default): a second, independent copy of rule 3 of `SAFETY.md`.
- **Address:** a 3-bit DIP switch (1–7 for the power boards, 8 for the pedal node). The pedal node of
  `PEDAL_NODE_PCB.md` becomes an RS-485 node of the same design with a different payload.
- **Temperature:** the DS18B20 the trunk was meant to carry sits on the node; its reading rides in every
  ACK, so the controller (and the app's System page) finally see per-board temperature, and the node can
  refuse to energize above a limit — the measured half of the duty budget planned for 2.1.0.

## Trunk

| RJ45 pin | Signal |
|---|---|
| 1, 2 | RS-485 A, B (one twisted pair) |
| 3, 6 | GND (its own pair) |
| 4, 5 | spare pair (reserved: a second bus or a hard all-off line) |
| 7, 8 | GND |

Shielded Cat7, shield bonded at the controller end only. **Termination:** 120 Ω at the controller and at
the last node (a jumper on every node, fitted on the last one). **Fail-safe bias** (pull A up, B down, ≈ 680 Ω)
at the controller only, so an idle bus reads as idle, not as noise. **No power on the RJ45**, as before:
24 V stays on the XT60 chain, each node makes its own 5 V / 3.3 V.

Controller side: an **isolated** RS-485 transceiver (ISO1500 / ADM2582E class, half-duplex, 3.3 V) in
place of today's I²C isolator, on a spare UART of the ESP32-S3 (two GPIOs + DE/RE). The existing
isolated-board revision's trunk connector stays; only the signals on it change.

## Protocol (half-duplex, controller is the only master)

`1 Mbit/s`, 8N1. Frame: `[0xA5][addr][seq][cmd][len][payload…][CRC16-CCITT]`. The node answers within
200 µs with `[0x5A][addr][seq][status][len][payload…][CRC16]`; no answer in 2 ms = retry once, then the
board is "offline" (as a missing I²C ACK is today).

| cmd | payload | use |
|---|---|---|
| `SET` | channel, pwm (12-bit; 4096 = full on) | one strike / release / cushion write |
| `SETN` | n × (channel, pwm) | a chord to one board in one frame |
| `ALLOFF` | — | the board's all-off (also as a broadcast to addr 0xFF, no reply; then polled) |
| `CFG` | PWM frequency, hold ceiling | at boot and on `freq` |
| `PING` | — | the 100 ms keep-alive to each node; the reply is the node's status |
| `STATUS` | — | channels on, temperature, 24 V, OE state, uptime, firmware version |
| `BOOT` | — | node reboot (bench) |

Status byte in every reply: OK · OE forced off · over temperature · 24 V missing · I²C to the PCA9685 failed.
A node that reports OE forced off is treated by the controller as "all channels off, board offline" until
it is re-enabled by `CFG`.

Throughput: a 10-note chord across boards is ≤ 10 frames of ~12 bytes ≈ 1.2 ms including replies — below
today's I²C time for the same chord at 400 kHz.

## Controller firmware (3.0.0)

- `power_boards.cpp` keeps its state machine, watchdogs and `coilsVerifiedOff()` untouched; only the
  `PCA9685` driver underneath (`setPWM`, `allOff`, `isConnected`, `begin`) gets an RS-485 implementation
  (`Rs485Node`) with the same contract. `tools/dispatch-sim` already replaces that driver with a fake, so
  every scenario keeps proving the ceiling on the new transport.
- Keep-alive: `PING` to each node from the chip-health cadence (every 100 ms, not every 5 s), so the
  node's 500 ms time-out never trips in normal use.
- New facts for the app: `!boardtemp=…`, `!board24v=…`, per board, from the status replies.
- The 24 V cut-off relay stays as the outermost layer.

## Node firmware

Its own small PlatformIO project (`firmware/node/`, ESP32-C6, Arduino): the frame parser, the PCA9685
driver (the existing `pca9685.cpp` reused as is), the time-out, the OE heartbeat, the DS18B20, and a
USB console for the bench. The node needs no over-the-air updates (it reboots from the controller's
`BOOT` only); flashing is by USB at the bench.

## Rollout

1. 吟晚风: the relay board first (`SAFETY_CUTOFF.md`), then one node board. Steven: a bench with one power
   board on the node, the controller on RS-485, the dispatch simulator green, then a full `sweep`.
2. Six more nodes; convert the piano in one visit (each board gains a node; the trunk is re-pinned; the
   controller gets the RS-485 isolator). The pedal node follows the same design.
3. Firmware 3.0.0 published only when every node is in; until then 2.x stays on I²C.

## Parts, per node (indicative)

ESP32-C6 module · THVD1450 (or MAX3485, 3.3 V, half-duplex) · 120 Ω + jumper · 2 × RJ45 shielded ·
2 × XT60 · Murata OKI-78SR-5 (24 V → 5 V, as the pedal node) + 3.3 V LDO · DS18B20 · 3-position DIP ·
the OE heartbeat detector (2 diodes, 2 capacitors, 1 resistor, 1 logic-level MOSFET) · TVS on 24 V.
