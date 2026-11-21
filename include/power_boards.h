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

// ---- Board address overrides (2.1.6) ----------------------------------------
// A board whose DIP switch can't reach its address (the new piano's C6, board
// 5: its A3 switch never closes, so it answers at 0x4D) is told where it
// answers. g_boardAddr[i] is 0 for the default (0x40 + i), else the saved
// address; initPowerBoards() applies it before the board's first begin(), so
// the controller never speaks to the wrong chip. Everything else — the hold
// watchdog, the health re-probe, !boards, the verified OFF — works by board
// index and follows the move. All Call (0x70) reaches every board anyway.
// Stored in NVS ("boardAddr", 8 bytes); `reset` writes it back, like the lock.
extern uint8_t g_boardAddr[8];
inline uint8_t board_default_addr(int board) { return (uint8_t)(0x40 + board); }
// Whether `board` may answer at `addr`; on false, *why says why (a short phrase).
bool board_addr_allowed(int board, uint8_t addr, const char **why);
// Move a board live: only with every coil verified off, nothing queued and no
// firmware update; the board is begun afresh at its new address (all OFF, the
// PWM frequency). False (and *why) when refused; true means the address took,
// whether or not a chip answers there (powerBoards[board].connected says).
bool board_set_address(int board, uint8_t addr, const char **why);
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

// ---- Firmware-update hooks (docs/BLE_OTA.md §7, docs/SAFETY.md rule 9) ----
//
// Energize inhibit. While set, the dispatch layer itself refuses EVERY
// energize, at the places a coil is driven: the strike (NoteOn, deferred fire,
// re-strike), manualFire/manualFireNote with pwm != 0, and the
// sweep; dispatchNoteOn returns false before it touches any timer or the held
// flag. Releases are NEVER inhibited: NoteOff, the s_needOff retry, the hold
// watchdog, allKeysOff(), broadcastAllOff() and tickChipHealth() run as usual.
// Setting it releases nothing by itself — the caller runs allKeysOff() next.
void set_energize_inhibit(bool on);
bool energize_inhibited();

// True only when every coil is verified OFF and nothing is waiting to change
// that: every board, ONLINE OR OFFLINE, has all 12 channels confirmed off
// (no s_onAt, no s_needOff), and no key has a pending fire, pending release,
// held note or queued repeat. Read-only — clears nothing (rule 1);
// an offline board's believed-on coil keeps it false (rule 5).
bool coilsVerifiedOff();

// millis() of the most recent energize attempt — stamped on every call of
// armOnAt(), whether or not the hold anchor moved. 0 = nothing energized since
// boot. The update's quiet rule: no session within 500 ms of it.
uint32_t lastEnergizeMs();

// Diagnostics (docs/BLE_DIAG.md), read-only: solenoid channels believed
// energized right now, and how many times the hold watchdog has had to let a
// key go since boot (counted once per trip).
int      coilsEnergizedCount();
uint32_t watchdogTrips();

// Tunable strike PWM values — defaults from config.h, can be changed at
// runtime via the serial console commands ("pwm", "min", "max", "velmult").
extern uint16_t g_sweepStrikePWM;
extern uint16_t g_minStrikePWM;        // white-key strike floor
// Black-key strike floor. Black keys need more force than white ones to sound.
// 0 = follow g_minStrikePWM (single-floor behaviour).
extern uint16_t g_minStrikePWMBlack;
// True when this MIDI note is a black key (C#, D#, F#, G#, A#).
bool note_is_black(uint8_t midi_note);
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
// Range 0.0..4.0. Used by the on-screen tuning page to compensate for
// solenoids that need more or less force than the global setting.
// Stored in NVS as a 128-float blob.
extern float g_keyForceMult[128];

// Distinct loudness steps (2.1.5, Steven 2026-11-18: "distinct notes of soft
// and hard"). With Full Power off and velsteps N (2..8), every NoteOn snaps to
// one of N fixed strike forces: no random scatter (humanvel is ignored), the
// master volume and velmult only move a note to a lower band, and a fast
// flurry lifts a note at most one step. A key with a calibration row (the
// app's listening calibration, `steptable`) strikes at its own measured PWMs
// (2..8 of them, rising; per-key force is not applied on top); any other key
// spreads the N steps over its min..max floor with the velocity curve.
// velsteps 0 is the smooth curve as before 2.1.5. Stored in NVS: "velSteps"
// and the "stepTbl" blob (128 rows of 18 bytes).
#define VEL_STEPS_MAX 8
struct StepRow {
    uint16_t pwm[VEL_STEPS_MAX];   // rising PWMs, 1..4095; pwm[0..n-1] used
    uint8_t  n;                    // 0 = not calibrated, else 2..8
    uint8_t  pad;
};
extern uint8_t  g_velSteps;            // 0 = smooth, else 2..8 bands
extern StepRow  g_stepTable[128];
extern uint32_t g_stepTableGen;        // bumped on every change (auto-save sees it)
uint8_t step_keys_calibrated();        // keys with a calibration row

// One calibration strike (2.1.5): this key at a raw PWM (1..4096) for `ms`,
// then the verified OFF, through manualFireNote() — the hold watchdog, the
// update inhibit and the verified-OFF retry all apply. Refused (false, nothing
// written) when the key is energized or has any queued strike, release or
// held note (a calibration never cancels a pending release), or no board
// answers for it.
bool calStrike(uint8_t midi_note, uint16_t pwm, uint32_t ms);

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
// Expression shaping. velCurve bends the MIDI-velocity -> strike-force map
// (>1 expands the soft end, the main cure for "sounds robotic"); the humanize
// values add small random scatter to force and timing so repeated notes and
// chords aren't machine-identical.
// Burst boost: notes arriving within g_burstGapMs of the previous strike are
// pushed g_burstBoostPct toward max force, because a short note has too little
// time for a soft strike to reach the string at all.
extern uint32_t g_burstGapMs;
extern uint8_t  g_burstBoostPct;

extern float   g_velCurve;      // 0.4 .. 3.0, 1.0 = linear
extern uint8_t g_humanizeVel;   // 0..30, +/- velocity units
extern uint8_t g_humanizeMs;    // 0..40 ms of note-on scatter

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

// Drains deferred re-triggers + their auto-releases. Call once per loop
// iteration alongside tickAutoRelease().
void tickPendingFires();

// Board health on two cadences. Every 5 s, probes each online PCA9685:
// a chip that answers a second probe is re-inited with begin() (it glitched,
// e.g. browned out from EMI / supply ripple under PWM switching); one that
// misses both is marked offline without a begin(). Every 1 s, re-probes the
// offline boards — their 24 V rail sits on a smart plug that is off whenever
// nothing plays (docs/SMART_PLUG.md) — hammers a verified all-off at any that
// still has a coil believed on, and brings back those that answer. Logs the
// transitions to Serial. Call from the main loop.
void tickChipHealth();

// Manually fire a single channel on a single board, bypassing all MIDI
// dispatch logic. Used by the serial "fire" command. The auto-release
// watchdog still applies — channel goes off after MAX_HOLD_MS regardless.
bool manualFire(uint8_t board_idx, uint8_t channel, uint16_t pwm);

// Fire a MIDI note at a RAW PWM value (0..4095, or 4096 = full-on),
// bypassing the velocity curve, per-key force, min-strike —
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
