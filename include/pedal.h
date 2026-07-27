// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Sustain (damper) pedal — servo actuator.
//
// MIDI CC64 drives a hobby servo that presses the piano's sustain pedal, so
// held/pedalled passages ring and blend the way the music intends.
//
// WHY A PEDAL AND NOT "just hold the keys down": a held key does keep its
// damper up, but a pedalled passage lasts 10-30 s while the solenoid thermal
// ceiling is 4 s per coil — and it would hold many coils at once. ONE pedal
// actuator lifts EVERY damper with a single device. It is the only thermally
// safe way to get sustain.
//
// HARDWARE: a DEDICATED PCA9685 at PEDAL_PCA_ADDR on the existing opto-isolated
// I²C trunk, running at PEDAL_SERVO_HZ (50 Hz servo rate). It must be its own
// chip because PCA9685 PWM frequency is PER-CHIP and the solenoid boards run at
// ~1111 Hz. setAllBoardsPWMFreq() only walks powerBoards[], so it never
// disturbs this chip.
//
// The hardware is OPTIONAL: if the chip doesn't ACK, every call below is a
// no-op and the piano runs exactly as before (no I²C traffic, no errors).
// ---------------------------------------------------------------------------

// Probe + configure the pedal chip. Safe to call when no pedal is installed.
void pedal_init();

// Apply the latest CC64 value (0..127). Call from loop() on the dispatch core
// — never from a BLE callback. Rate-limited and change-gated internally, so
// calling it every iteration is cheap.
void pedal_tick(uint8_t cc64Value);

// Safety: lift the pedal now (all-off / panic / BLE disconnect). Without this a
// stop would leave the dampers up and the piano ringing.
void pedal_release();

// True once the pedal chip has ACKed.
bool pedal_present();

// One-line state for the `status` command.
void pedal_print_status();

// Drive the servo straight to a raw PCA9685 count, for finding the UP/DOWN
// endpoints during setup. Creep up on the limits — a servo stalled against the
// pedal stop draws hard and will cook itself.
void pedal_test_counts(uint16_t counts);

// --- Runtime-tunable, persisted in NVS -------------------------------------
extern bool     g_pedalEnabled;     // master enable (default off until wired)
extern uint16_t g_pedalUpCounts;    // PCA9685 counts, pedal fully UP
extern uint16_t g_pedalDownCounts;  // PCA9685 counts, pedal fully DOWN
extern bool     g_pedalHalf;        // true = continuous half-pedalling
