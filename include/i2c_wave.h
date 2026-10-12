// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Bench signal test for the power-board I²C trunk: a clean square wave on SCL,
// SDA or both, so the lines can be scoped after the isolator and down the
// Cat7 trunk without any traffic on the bus (the `i2cwave` console command).
//
// SAFETY. The bus is taken down for the whole test, so no release could reach
// a coil while it runs: it starts only with every coil verified off and the
// dispatch layer's energize inhibit set, and ends with the bus brought back up
// exactly as at boot, three ALLCALL all-offs, every board re-probed and the
// inhibit cleared. USB console only (not in the Bluetooth allow-list).
#pragma once
#include <Arduino.h>

// `which` is "scl", "sda" or "both"; `hz` 10..2,000,000; `seconds` 1..30.
// Prints what it does to `out`. False when refused or when the wave could not
// be started (the bus is restored either way).
bool i2c_wave_run(const char *which, uint32_t hz, uint32_t seconds, Print &out);
