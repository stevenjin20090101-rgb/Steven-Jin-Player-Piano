// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Bench signal test for the power-board I²C trunk (the `i2cwave` console
// command): a clean square wave on SCL, SDA or both, or real address probes to
// every power board, so the lines can be scoped after the isolator and down
// the Cat7 trunk. It runs until `i2cwave stop` (or `off` / `panic`), or for
// the seconds given; the piano's screen and `status` say while it is on.
//
// SAFETY. During a square-wave test the bus is down, so no release could
// reach a coil: the test starts only with every coil verified off and the
// dispatch layer's energize inhibit set, notes are dropped at the input and
// energize commands refused while it runs, and it ends with the bus brought
// back up as at boot, three ALLCALL all-offs, every board re-probed and the
// inhibit cleared. loop() skips the safety ticks while it is on (they would
// have nothing to do and no bus to do it on). USB console only.
#pragma once
#include <Arduino.h>

// `which`: "scl", "sda", "both" (square wave at `hz`, 10..2,000,000) or "bus"
// (real probes, `hz` ignored). `seconds` 0 = until stopped (10 min cap).
// Prints what it does to `out`; false when refused.
bool i2c_wave_start(const char *which, uint32_t hz, uint32_t seconds, Print &out);

// Ends a running test (bus restored, boards re-probed, everything off); a
// no-op otherwise. `off`, `panic` and a firmware update call it first.
void i2c_wave_stop(Print &out);

// loop(), every pass: the probes of the `bus` mode, the time limit, a
// heartbeat line every 10 s.
void i2c_wave_tick();

bool i2c_wave_active();

// "Signal test: SCL 400 kHz, 0:12" for the screen; false when not running.
bool i2c_wave_status_text(char *buf, size_t len);

// One indented `status` line.
void i2c_wave_print_status(Print &out);
