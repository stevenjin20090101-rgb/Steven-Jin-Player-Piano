// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Routes MIDI NoteOn/Off to whichever PCA9685 owns that key on the I²C
// trunk. Each board handles one chromatic octave; PCA9685 channels 0..11
// are wired in chromatic order to the solenoids.
//
// IMPORTANT: the channel-to-key wiring must match 吟晚风's PCB layout —
// see kKeyToChannel[] in power_boards.cpp and adjust if needed.

#pragma once
#include "pca9685.h"

struct PowerBoard {
    PCA9685 chip;
    uint8_t midi_start;  // MIDI note number of the LOWEST key on this board
    uint8_t key_count;   // number of keys this board owns (≤ 12, usually 12)
    bool    connected;   // set by initPowerBoards() via I²C ack test
};

extern PowerBoard powerBoards[];
extern const int  kNumPowerBoards;

void initPowerBoards();                                 // scan + configure
bool dispatchNoteOn(uint8_t midi_note, uint8_t velocity);
bool dispatchNoteOff(uint8_t midi_note);
void allKeysOff();                                      // panic / safety
void testSweepAllBoards();                              // fire each ch once on boot

// Single-transaction broadcast that forces every PCA9685 channel on every
// chip to FULL_OFF via the ALLCALL address (0x70). Faster than iterating
// boards individually and immune to per-address Wire glitches. Use this
// whenever you need a hard "release everything right now" — at end of
// sweep, on BLE disconnect, or via the "broadcast" serial command.
// Returns true only if BOTH ALLCALL transactions were ACKed — callers must
// not clear coil-tracking state on a false return (the coils may still be
// energized behind a hung bus).
bool broadcastAllOff();

// Tunable strike PWM values — defaults from config.h, can be changed at
// runtime via the serial console commands ("pwm", "min", "max", "velmult").
extern uint16_t g_sweepStrikePWM;
extern uint16_t g_minStrikePWM;
extern uint16_t g_maxStrikePWM;

// Velocity multiplier — applied to incoming MIDI velocity BEFORE the
// min/max PWM mapping. Boost weak MIDI files: velmult 1.5 turns a
// velocity-64 note into a velocity-96 one (capped at 127). Useful when
// a song's encoded velocities are too low for the solenoids to swing
// with enough force (Rush E typical).
extern float    g_velocityMult;

// Full-power mode: when true, every NoteOn drives the channel FULL_ON
// (constant HIGH, max strike force, NO PWM modulation = NO audible coil
// hum). Loses velocity sensitivity. Toggle via "fullpower 0|1".
extern bool     g_fullPowerMode;

// Current PCA9685 PWM carrier frequency. Use setAllBoardsPWMFreq() to
// change it across every connected chip.
extern uint16_t g_pwmFreqHz;

void setAllBoardsPWMFreq(uint16_t hz);

// Per-key force multiplier — indexed by MIDI note (0..127). Applied to
// the final PWM value after velocity mapping. Default 1.0 = no change.
// Range 0.0..2.0. Used by the on-screen tuning page to compensate for
// solenoids that need more or less force than the global setting.
// Stored in NVS as a 128-float blob.
extern float g_keyForceMult[128];

// Minimum gap (ms) between a NoteOff and the next NoteOn on the SAME
// MIDI key. Defers the new NoteOn if too soon — pendingFireMs queue is
// drained by tickPendingFires(). 0 = disabled (raw passthrough).
extern uint32_t g_minRetriggerGapMs;

// Minimum time (ms) a solenoid must be energized after a NoteOn before
// we honor the corresponding NoteOff. Forces every strike to be at
// least this long so very short MIDI notes still produce audible hits.
extern uint32_t g_minStrikeMs;

// Dynamic min-strike for ISOLATED short notes. A lone staccato note (one that
// arrives after > g_isoGapMs of silence) is stretched to at least g_isoStrikeMs
// — longer than the normal g_minStrikeMs — so it actually sounds, while notes
// inside a fast passage keep the shorter g_minStrikeMs and stay quick.
// g_isoStrikeMs <= g_minStrikeMs (or g_isoGapMs == 0) disables the boost.
extern uint32_t g_isoStrikeMs;
extern uint32_t g_isoGapMs;

// Master volume 0..100 — scales strike force down toward the audibility floor
// so the whole piano plays softer without touching min/max/velmult. Set it via
// set_master_volume(), which also turns Full Power off (see the .cpp).
extern uint8_t g_masterVolume;
void set_master_volume(uint8_t vol);

// Auto re-strike (tremolo sustain). While a note is held, re-hit it every
// g_restrikeMs (0 = off) so long notes stay audible instead of striking
// once and decaying. g_restrikeLiftMs is the key-lift gap between hits.
extern uint32_t g_restrikeMs;
extern uint32_t g_restrikeLiftMs;

// Re-strikes held notes on the auto-tremolo schedule. Call each loop
// alongside tickAutoRelease / tickPendingFires.
void tickRestrike();

// Soft release / anti-clank. See config.h for the mechanism.
extern bool     g_softRelease;   // master on/off
extern uint16_t g_releasePwm;    // cushion current (12-bit)
extern uint32_t g_releaseMs;     // cushion duration before cut to 0

// Drains deferred re-triggers + their auto-releases. Call once per loop
// iteration alongside tickAutoRelease().
void tickPendingFires();

// Probes every connected PCA9685 every few seconds. If a chip stops
// ack'ing (e.g. it browned out from EMI / power supply ripple caused by
// PWM switching of the solenoid coils), re-runs begin() to recover it.
// Logs everything to Serial. Call from the main loop.
void tickChipHealth();

// Manually fire a single channel on a single board, bypassing all MIDI
// dispatch logic. Used by the serial "fire" command. The auto-release
// watchdog still applies — channel goes off after MAX_HOLD_MS regardless.
bool manualFire(uint8_t board_idx, uint8_t channel, uint16_t pwm);

// Fire a MIDI note at a RAW PWM value (0..4095, or 4096 = full-on),
// bypassing the velocity curve, per-key force, soft-release, min-strike —
// everything. For the strike tester: lets you sweep force independently of
// duration. pwm==0 releases. The auto-release watchdog still applies.
bool manualFireNote(uint8_t midi_note, uint16_t pwm);

// Force-release any solenoid that's been held longer than g_maxHoldMs.
// Acts as a safety net for orphan NoteOn events (e.g., dropped BLE
// NoteOff packet) — NOT the primary release mechanism. NoteOff handling
// releases solenoids as soon as the message arrives.
void tickAutoRelease();

// Safety hold timeout. SHORT default (2 s) — stuck solenoid at 24V/0.5A
// reaches 80°C in 5 minutes per direct measurement. Capping at 2 s keeps
// thermal exposure to <8% of that. If you need longer sustained notes
// (organ-style), raise via "hold <ms>" or web-UI slider, but ~4 s is the
// practical safety ceiling. Note that real piano hammers strike and
// rebound, they don't hold — so >1 s sustain is unusual for piano voicing.
#ifndef DEFAULT_MAX_HOLD_MS
#define DEFAULT_MAX_HOLD_MS  2000
#endif
// Hard safety ceiling for g_maxHoldMs. The "hold" serial command, the web UI,
// and the NVS-loaded value are ALL clamped to this so no configuration path
// can extend the stuck-on window past the thermal-safe budget. Use
// set_max_hold_ms() as the single choke point.
#ifndef MAX_HOLD_CEILING_MS
#define MAX_HOLD_CEILING_MS  4000
#endif
extern uint32_t g_maxHoldMs;
void set_max_hold_ms(uint32_t ms);   // clamps to [50, MAX_HOLD_CEILING_MS]
