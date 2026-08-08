// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#include "power_boards.h"
#include "config.h"
#include <esp_task_wdt.h>

// Runtime PWM + timing tuning. All initialized from config.h defaults but
// mutable at runtime via the serial console.
uint16_t g_sweepStrikePWM = SWEEP_STRIKE_PWM;
uint16_t g_minStrikePWM   = MIN_STRIKE_PWM;
uint16_t g_maxStrikePWM   = MAX_STRIKE_PWM;
uint32_t g_maxHoldMs      = DEFAULT_MAX_HOLD_MS;
float    g_velocityMult   = 1.0f;
bool     g_fullPowerMode  = DEFAULT_FULL_POWER_MODE != 0;
uint16_t g_pwmFreqHz      = DEFAULT_PWM_FREQ_HZ;

// Per-key force multiplier. Default 1.0 = neutral. Settings module loads
// overrides from NVS at boot.
float g_keyForceMult[128] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

// Maps MIDI chromatic offset (0=C ... 11=B) → PCA9685 LED channel that
// drives that key's solenoid. From 吟晚风's ESP32S3_Schematic (U1, PCA9685):
//   White keys C..B are on LED0..LED6   (channels 0,1,2,3,4,5,6)
//   Black keys A#,G#,F#,D#,C# are on LED11..LED15 (channels 11,12,13,14,15)
//   LED7..LED10 are not wired (X marks on the schematic)
// Note the black-key channels are in REVERSE chromatic order on the chip.
static const uint8_t kKeyToChannel[12] = {
    0,    // 0: C   → LED0
    15,   // 1: C#  → LED15
    1,    // 2: D   → LED1
    14,   // 3: D#  → LED14
    2,    // 4: E   → LED2
    3,    // 5: F   → LED3
    13,   // 6: F#  → LED13
    4,    // 7: G   → LED4
    12,   // 8: G#  → LED12
    5,    // 9: A   → LED5
    11,   // 10: A# → LED11
    6,    // 11: B  → LED6
};

// 7-board coverage: 84 keys, C1 – B7 (MIDI 24..107). Each board owns one
// chromatic octave aligned to C. Bottom 3 piano keys (A0/A#0/B0) and top
// key (C8) are not in range — extend by adding more boards later if needed.
PowerBoard powerBoards[] = {
    {PCA9685(0x40),  24, 12, false},   // C1 – B1
    {PCA9685(0x41),  36, 12, false},   // C2 – B2
    {PCA9685(0x42),  48, 12, false},   // C3 – B3
    {PCA9685(0x43),  60, 12, false},   // C4 – B4 (middle C octave)
    {PCA9685(0x44),  72, 12, false},   // C5 – B5
    {PCA9685(0x45),  84, 12, false},   // C6 – B6
    {PCA9685(0x46),  96, 12, false},   // C7 – B7
};
const int kNumPowerBoards = sizeof(powerBoards) / sizeof(powerBoards[0]);

// millis() timestamp when each (board, semitone) was set ON. 0 means the
// channel is believed OFF. tickAutoRelease() force-releases any channel whose
// age exceeds g_maxHoldMs. CRITICAL INVARIANT: s_onAt is only cleared to 0
// after a CONFIRMED-SUCCESSFUL I²C OFF write — never on a write we didn't
// verify landed. Otherwise a failed OFF (bus glitch from solenoid EMI) would
// leave the coil energized while the watchdog thinks it's off → stuck-on fire.
static uint32_t s_onAt[8][12] = {{0}};   // [board_idx][semitone]

// Set when an OFF write for a channel FAILED. tickAutoRelease retries every
// tick until it succeeds, independent of the g_maxHoldMs timeout — so a
// glitched OFF is re-hammered within ~10 ms, not left for up to g_maxHoldMs.
static bool s_needOff[8][12] = {{false}};

// Consecutive tickAutoRelease passes that still had un-cleared OFF failures.
// After a few, escalate to a bus-wide ALLCALL FULL_OFF as a last resort.
static uint8_t s_offFailStreak = 0;

// Per-MIDI-note timing state for the rapid-retrigger deferral feature.
// Indexed by MIDI note (0..127). Holds:
//   lastOffMs        when this note was last released (used for gap check)
//   pendingFireMs    if non-zero, fire the NoteOn at this millis() target
//   pendingReleaseMs if non-zero, fire the NoteOff at this millis() target
//                    (set when the source's NoteOff arrived BEFORE the
//                    deferred fire executed)
//   pendingVelocity  velocity to use when the deferred fire runs
struct KeyTiming {
    uint32_t lastOffMs;
    uint32_t onAtMs;          // millis() when this note's last NoteOn fired
    uint32_t effMinStrikeMs;  // min-strike chosen for THIS note (isolated → longer)
    uint32_t pendingFireMs;
    uint32_t pendingReleaseMs;
    uint32_t cushionOffMs;    // if non-zero: soft-release cushion active, cut to 0 at this time
    uint8_t  pendingVelocity;
    // --- auto re-strike (tremolo sustain) ---
    bool     held;            // source has this note pressed (NoteOn, no NoteOff yet)
    uint8_t  heldVel;         // velocity to re-strike with
    uint32_t heldSinceMs;     // when the source pressed it (NOT reset per re-strike)
    uint32_t reStrikeAtMs;    // next re-strike action time
    bool     reStrikeLifting; // true = currently in the brief key-lift gap
};
static KeyTiming s_keyTiming[128] = {};

uint32_t g_minRetriggerGapMs = DEFAULT_MIN_RETRIGGER_GAP_MS;
uint32_t g_minStrikeMs       = DEFAULT_MIN_STRIKE_MS;
uint8_t  g_masterVolume      = DEFAULT_MASTER_VOLUME;
uint32_t g_isoStrikeMs       = DEFAULT_ISO_STRIKE_MS;
uint32_t g_isoGapMs          = DEFAULT_ISO_GAP_MS;
float    g_velCurve          = DEFAULT_VEL_CURVE;
uint8_t  g_humanizeVel       = DEFAULT_HUMANIZE_VEL;
uint8_t  g_humanizeMs        = DEFAULT_HUMANIZE_MS;
// millis() of the last actual strike on ANY key — the density gauge used to
// tell an isolated staccato note (long gap since this) from one inside a run.
static uint32_t s_lastFireMs = 0;

// Auto re-strike: while a note is held, re-hit it every g_restrikeMs so
// long/sustained notes stay audible instead of striking once and decaying.
// 0 = off. The key-lift gap (g_restrikeLiftMs) is how long the solenoid
// releases so the hammer/jack can reset before the next strike.
uint32_t g_restrikeMs     = DEFAULT_RESTRIKE_MS;
uint32_t g_restrikeLiftMs = DEFAULT_RESTRIKE_LIFT_MS;

// Single choke point for the hold timeout — clamps to the thermal-safe
// ceiling so no path (serial, web UI, NVS) can extend the stuck-on window
// past what's safe. See MAX_HOLD_CEILING_MS.
void set_max_hold_ms(uint32_t ms) {
    if (ms < 50)                    ms = 50;
    if (ms > MAX_HOLD_CEILING_MS)   ms = MAX_HOLD_CEILING_MS;
    g_maxHoldMs = ms;
}

// Soft release / anti-clank (see config.h).
bool     g_softRelease = (DEFAULT_SOFT_RELEASE != 0);
uint16_t g_releasePwm  = DEFAULT_RELEASE_PWM;
uint32_t g_releaseMs   = DEFAULT_RELEASE_MS;

void initPowerBoards() {
    for (int i = 0; i < kNumPowerBoards; i++) {
        auto& b = powerBoards[i];
        b.connected = b.chip.begin();
        Serial.printf("[piano] board %d @ 0x%02X (MIDI %u-%u): %s\n",
                      i, b.chip.address(),
                      b.midi_start, b.midi_start + b.key_count - 1,
                      b.connected ? "OK" : "MISSING");
    }
    // Move every chip to the configured PWM frequency (default 1500 Hz)
    // so we're not buzzing at the factory 200 Hz default.
    setAllBoardsPWMFreq(g_pwmFreqHz);
    Serial.printf("[piano] PWM carrier set to %u Hz on all boards\n", g_pwmFreqHz);
}

// Push a new PWM frequency to every connected board. Persists in
// g_pwmFreqHz so subsequent calls (e.g. via "freq" serial command)
// use the new value.
// Master volume, 0..100. Anything below 100 needs the velocity/PWM path, so
// this turns Full Power OFF — "every note at max force" and "play softer" are
// mutually exclusive. Going back to 100 does NOT re-enable Full Power; that
// stays the user's explicit choice.
void set_master_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    g_masterVolume = vol;
    if (vol < 100 && g_fullPowerMode) {
        g_fullPowerMode = false;
        Serial.println("[vol] Full Power turned OFF — required for soft playing "
                       "(it forces every note to max force)");
    }
}

void setAllBoardsPWMFreq(uint16_t hz) {
    if (hz < 24)   hz = 24;
    if (hz > 1526) hz = 1526;
    g_pwmFreqHz = hz;
    for (int i = 0; i < kNumPowerBoards; i++) {
        if (powerBoards[i].connected) {
            powerBoards[i].chip.setPWMFrequency(hz);
        }
    }
}

// Linear scan is fine: 8 boards, runs once per note.
static int boardForNote(uint8_t midi_note) {
    for (int i = 0; i < kNumPowerBoards; i++) {
        auto& b = powerBoards[i];
        if (midi_note >= b.midi_start &&
            midi_note <  b.midi_start + b.key_count) return i;
    }
    return -1;
}

// Internal — actually writes to the PCA9685. NO gap checking, NO deferral.
// Called both by the public dispatchNoteOn (immediate path) and the tick
// function (when a deferred fire reaches its scheduled time).
static bool fireNoteOnNow(uint8_t midi_note, uint8_t velocity) {
    int idx = boardForNote(midi_note);
    if (idx < 0) {
#if DEBUG_DISPATCH
        Serial.printf("[disp] note=%u OUT_OF_RANGE\n", midi_note);
#endif
        return false;
    }
    auto& b = powerBoards[idx];
    if (!b.connected) {
#if DEBUG_DISPATCH
        Serial.printf("[disp] note=%u board=%d (0x%02X) MISSING — skipped\n",
                      midi_note, idx, b.chip.address());
#endif
        return false;
    }

    uint8_t semitone = midi_note - b.midi_start;
    if (semitone >= 12) return false;
    uint8_t channel  = kKeyToChannel[semitone];

    uint32_t pwm;
    if (g_fullPowerMode) {
        // FULL_ON path — output stays HIGH for the entire cycle, no PWM
        // modulation. Maximum strike force, zero coil hum, no velocity
        // dynamics. setPWM() interprets >= 4095 as FULL_ON via the
        // ON_H[4] bit.
        pwm = 4096;
    } else {
        // Velocity-mapped PWM. Apply multiplier (clamped to 1..127), then
        // map to the tunable PWM floor/ceiling. Solenoids have a hard
        // minimum-pull threshold, so the floor must be high enough that
        // even velocity = 1 still actuates.
        float vBoosted = (float)velocity * g_velocityMult;
        // Humanize velocity: a real player never strikes the same note twice
        // with identical force. A few units of scatter stops repeated notes
        // and sustained chords sounding machine-stamped.
        if (g_humanizeVel > 0) {
            vBoosted += (float)random(-(int)g_humanizeVel, (int)g_humanizeVel + 1);
        }
        if (vBoosted > 127.0f) vBoosted = 127.0f;
        if (vBoosted < 1.0f)   vBoosted = 1.0f;
        uint16_t lo = g_minStrikePWM;
        uint16_t hi = g_maxStrikePWM;
        if (hi < lo) hi = lo;
        // Perceptual velocity curve. A straight MIDI->force line is the single
        // biggest reason solenoid pianos sound robotic: loudness is not linear
        // in hammer force, and the musical detail lives at the soft end where
        // a linear map squashes it flat.
        //   curve > 1  expands the soft end -> expressive pp, wider dynamics
        //   curve = 1  linear (previous behaviour)
        float t = (vBoosted - 1.0f) / 126.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        if (g_velCurve != 1.0f) t = powf(t, g_velCurve);
        pwm = lo + (uint32_t)((float)(hi - lo) * t + 0.5f);
        if (pwm > hi) pwm = hi;
    }

    // Per-key force multiplier — final tweak after velocity mapping.
    if (midi_note < 128) {
        float m = g_keyForceMult[midi_note];
        if (m < 0.0f) m = 0.0f;
        if (m > 4.0f) m = 4.0f;
        uint32_t scaled = (uint32_t)((float)pwm * m + 0.5f);
        if (g_fullPowerMode) {
            // Full-power mode already uses the 4096 FULL_ON sentinel; keep it.
            pwm = 4096;
        } else {
            // Variable-PWM mode: NEVER emit 4096 (that switches the channel to
            // constant DC and defeats the anti-EMI reason PWM mode exists). A
            // keyforce>1.0 must stay clamped to the 4095 PWM ceiling.
            pwm = (scaled > 4095) ? 4095 : scaled;
        }
    }

    // --- Master volume ------------------------------------------------------
    // One knob for "play the whole piano softer". Scales the strike force DOWN
    // toward the audibility floor, so quieter never means silent: a solenoid
    // has a hard minimum pull below which the hammer never reaches the string.
    //
    // Volume is meaningless in Full-Power mode (that IS maximum force by
    // definition), so set_master_volume() turns Full Power off when you go
    // below 100 — the two are physically contradictory.
    if (g_masterVolume < 100 && !g_fullPowerMode) {
        uint32_t floorPwm = g_minStrikePWM;
        // Keep a little headroom under the floor so 0% is quiet but still
        // strikes; going fully to 0 would just produce silent, dead keys.
        uint32_t softFloor = (floorPwm * 82) / 100;
        if (pwm > softFloor) {
            pwm = softFloor + ((pwm - softFloor) * g_masterVolume) / 100;
        }
    }

    uint32_t now = millis();
    b.chip.setPWM(channel, (uint16_t)pwm);
    s_onAt[idx][semitone]   = now;      // arm auto-release watchdog
    s_needOff[idx][semitone] = false;   // fresh strike supersedes a prior failed-OFF
                                        // retry — otherwise the watchdog would
                                        // cut this note ~10 ms after attack
    if (midi_note < 128) {
        s_keyTiming[midi_note].onAtMs = now;       // for minStrike
        s_keyTiming[midi_note].cushionOffMs = 0;   // supersede any in-flight cushion
        // Pick this note's minimum strike duration NOW, from how long it's been
        // since the last strike on any key. A lone note after silence gets the
        // longer isolated minimum so it's audible; a note inside a run keeps the
        // short one so fast passages stay fast. Decided at attack (we can only
        // see the past), then honored by the deferred-release logic below.
        bool isolated = (s_lastFireMs == 0) || (now - s_lastFireMs > g_isoGapMs);
        uint32_t eff = g_minStrikeMs;
        if (isolated && g_isoGapMs > 0 && g_isoStrikeMs > g_minStrikeMs) {
            eff = g_isoStrikeMs;
        }
        s_keyTiming[midi_note].effMinStrikeMs = eff;
    }
    s_lastFireMs = now;                            // update the density gauge
#if DEBUG_DISPATCH
    Serial.printf("[disp] NoteOn  note=%u v=%u → board %d (0x%02X) ch=%u pwm=%u\n",
                  midi_note, velocity, idx, b.chip.address(), channel, pwm);
#endif
    return true;
}

// Attempt to drive one channel to 0. Returns true only if the I²C OFF write
// is confirmed. On success: disarm the watchdog slot + clear any retry flag.
// On failure: KEEP the watchdog armed and set s_needOff so tickAutoRelease
// keeps re-hammering the OFF until it lands. This is the heart of the
// fire-safety fix — a coil is only considered "off" after a verified write.
static bool tryChannelOff(int board_idx, uint8_t semitone) {
    if (board_idx < 0 || board_idx >= kNumPowerBoards) return false;
    auto& b = powerBoards[board_idx];
    if (!b.connected) return false;
    bool ok = b.chip.setPWM(kKeyToChannel[semitone], 0);
    if (ok) {
        s_onAt[board_idx][semitone]   = 0;
        s_needOff[board_idx][semitone] = false;
    } else {
        // Leave s_onAt armed; flag for fast retry next tick.
        s_needOff[board_idx][semitone] = true;
    }
    return ok;
}

// Cut a channel to 0 and clear its cushion state. The TRUE "off". Used by the
// non-cushion release path, cushion completion, and anywhere we need a
// guaranteed off. Retry-on-failure is handled by tryChannelOff + the watchdog.
static void hardOffNow(uint8_t midi_note) {
    int idx = boardForNote(midi_note);
    if (idx < 0) return;
    auto& b = powerBoards[idx];
    if (!b.connected) return;
    uint8_t semitone = midi_note - b.midi_start;
    if (semitone >= 12) return;
    tryChannelOff(idx, semitone);            // clears s_onAt only on success
    if (midi_note < 128) s_keyTiming[midi_note].cushionOffMs = 0;
}

// Internal release. With soft-release on, drops to a low cushion current
// (braking the retract spring so the plunger lands softly instead of
// clanking the backstop) and schedules the true cut-to-0 via cushionOffMs.
// With soft-release off, cuts straight to 0.
static bool fireNoteOffNow(uint8_t midi_note) {
    int idx = boardForNote(midi_note);
    if (idx < 0) return false;
    auto& b = powerBoards[idx];
    if (!b.connected) return false;
    uint8_t semitone = midi_note - b.midi_start;
    if (semitone >= 12) return false;
    uint8_t channel = kKeyToChannel[semitone];

    // Channel already off and no cushion in flight → nothing to release.
    // Without this guard, a duplicate NoteOff (common in real MIDI files,
    // and guaranteed by octave-folding collisions) would write the cushion
    // current to an IDLE coil — a spurious 22 ms energize pulse.
    if (s_onAt[idx][semitone] == 0 &&
        (midi_note >= 128 || s_keyTiming[midi_note].cushionOffMs == 0)) {
        return true;
    }

    if (g_softRelease && g_releaseMs > 0 && g_releasePwm > 0 && midi_note < 128) {
        uint32_t now = millis();
        bool ok = b.chip.setPWM(channel, g_releasePwm);   // cushion current
        if (ok) {
            // Watchdog stays armed during the cushion (s_onAt), so even if the
            // cushion-completion OFF later fails, the g_maxHoldMs timeout still
            // force-releases this channel — the cushion is fully covered.
            s_onAt[idx][semitone] = now;
            s_keyTiming[midi_note].cushionOffMs = now + g_releaseMs;
        } else {
            // Cushion write didn't land — don't depend on a cushion that
            // isn't running. Go straight to the retrying OFF path.
            tryChannelOff(idx, semitone);
            s_keyTiming[midi_note].cushionOffMs = 0;
        }
#if DEBUG_DISPATCH
        Serial.printf("[disp] NoteOff note=%u → cushion %u for %lums (ok=%d)\n",
                      midi_note, (unsigned)g_releasePwm, (unsigned long)g_releaseMs, ok);
#endif
    } else {
        hardOffNow(midi_note);                       // straight to 0 (retrying)
#if DEBUG_DISPATCH
        Serial.printf("[disp] NoteOff note=%u → off (board %d ch=%u)\n",
                      midi_note, idx, channel);
#endif
    }
    return true;
}

// ---- Public dispatch with gap-aware deferral ------------------------------
//
// dispatchNoteOn checks how long ago this same MIDI note was released. If
// the gap is shorter than g_minRetriggerGapMs, the new NoteOn is queued
// (s_keyTiming[note].pendingFireMs) and will be executed by tickPendingFires
// once enough time has elapsed. Otherwise it fires immediately.
//
// dispatchNoteOff updates lastOffMs and either fires off immediately (normal
// case) or — if a NoteOn is still queued for this key — schedules a brief
// release after the deferred fire so the note has a minimum audible duration.

bool dispatchNoteOn(uint8_t midi_note, uint8_t velocity) {
    // Mark the note held for auto re-strike (tremolo sustain). Applies to
    // both the immediate and deferred paths — the note is "held" the moment
    // the source presses it. heldSinceMs is the orphan-safety anchor and is
    // NOT reset by re-strikes.
    if (midi_note < 128) {
        uint32_t now = millis();
        KeyTiming &kt = s_keyTiming[midi_note];
        // Anchor the thermal/orphan timer to the FIRST press only. A stream
        // of NoteOns for an already-held key (sustain re-articulation, buggy
        // source) must NOT slide the safety cutoff forward — otherwise the
        // coil could be driven indefinitely.
        if (!kt.held) kt.heldSinceMs = now;
        kt.held            = true;
        kt.heldVel         = velocity;
        kt.reStrikeAtMs    = now + g_restrikeMs;   // first re-strike after one interval
        kt.reStrikeLifting = false;
    }
    if (g_minRetriggerGapMs > 0 && midi_note < 128) {
        uint32_t now = millis();
        KeyTiming &rt = s_keyTiming[midi_note];

        // Rapid repeat WHILE a soft-release cushion is still holding the
        // plunger partway down (cushionOffMs in flight): the key hasn't
        // physically reset. Cut the cushion to 0 NOW so the plunger lifts,
        // and space the re-strike by the full gap from this instant. Without
        // this the cushion (g_releaseMs) can outlast the retrigger gap, so the
        // deferred strike fires onto a still-depressed key and the repeat
        // never articulates — the "repeats don't work with soft-release on"
        // bug. (The gap alone couldn't fix it: it was measured from the
        // cushion START, but the plunger only frees at the cushion END.)
        if (rt.cushionOffMs != 0) {
            hardOffNow(midi_note);                 // clears cushionOffMs + s_onAt
            rt.pendingFireMs    = now + g_minRetriggerGapMs;
            rt.pendingVelocity  = velocity;
            rt.pendingReleaseMs = 0;
#if DEBUG_DISPATCH
            Serial.printf("[disp] note=%u retrigger during cushion → cut + defer %lu ms\n",
                          midi_note, (unsigned long)g_minRetriggerGapMs);
#endif
            return true;
        }

        // Normal retrigger gap, measured from the last full (cushion-free)
        // release so the plunger has time to reset between strikes.
        uint32_t lastOff = rt.lastOffMs;
        if (lastOff > 0) {
            uint32_t elapsed = now - lastOff;
            if (elapsed < g_minRetriggerGapMs) {
                rt.pendingFireMs    = lastOff + g_minRetriggerGapMs;
                rt.pendingVelocity  = velocity;
                rt.pendingReleaseMs = 0;  // unscheduled until NoteOff
#if DEBUG_DISPATCH
                Serial.printf("[disp] note=%u deferred by %lu ms (gap=%lu)\n",
                              midi_note,
                              (unsigned long)(g_minRetriggerGapMs - elapsed),
                              (unsigned long)g_minRetriggerGapMs);
#endif
                return true;
            }
        }
    }
    // Humanize timing. Real hands are never sample-exact: a chord rolls by a
    // few ms and a line breathes. Perfectly simultaneous note-ons are the
    // giveaway that a machine is playing. Reuses the existing deferred-fire
    // path, so the strike still goes through fireNoteOnNow and stays covered
    // by the release watchdog.
    if (g_humanizeMs > 0 && midi_note < 128 &&
        s_keyTiming[midi_note].pendingFireMs == 0) {
        uint32_t jitter = (uint32_t)random(0, (int)g_humanizeMs + 1);
        if (jitter > 0) {
            s_keyTiming[midi_note].pendingFireMs    = millis() + jitter;
            s_keyTiming[midi_note].pendingVelocity  = velocity;
            s_keyTiming[midi_note].pendingReleaseMs = 0;
            return true;
        }
    }

    // Fire immediately. An immediate NoteOn supersedes ALL pending timers
    // for this note — clear the queued fire AND any scheduled release, or a
    // stale pendingReleaseMs from a prior deferred-off would later cut this
    // freshly-struck note short (dropped held note). cushionOffMs is cleared
    // inside fireNoteOnNow.
    if (midi_note < 128) {
        s_keyTiming[midi_note].pendingFireMs    = 0;
        s_keyTiming[midi_note].pendingReleaseMs = 0;
    }
    return fireNoteOnNow(midi_note, velocity);
}

bool dispatchNoteOff(uint8_t midi_note) {
    if (midi_note < 128) {
        // Source released the key → stop any auto re-strike immediately.
        s_keyTiming[midi_note].held = false;
        s_keyTiming[midi_note].reStrikeLifting = false;
    }
    if (midi_note < 128) {
        uint32_t now = millis();

        // Case 1: a fire is queued but hasn't run yet. Source already
        // wants the note off — schedule a brief audible release after
        // the deferred fire so the note actually plays for ~50 ms.
        if (s_keyTiming[midi_note].pendingFireMs != 0) {
            s_keyTiming[midi_note].lastOffMs = now;
            s_keyTiming[midi_note].pendingReleaseMs =
                s_keyTiming[midi_note].pendingFireMs + DEFERRED_STRIKE_MIN_MS;
            return true;
        }

        // Case 2: note is currently held. Has it been held long enough?
        // If MIDI sent a very short note, the solenoid hasn't had time to fully
        // strike. Defer the release until this note's own minimum-strike window
        // elapses — which is the LONGER isolated value for a lone staccato note
        // and the shorter one for a note inside a run (chosen at attack).
        uint32_t eff = s_keyTiming[midi_note].effMinStrikeMs;
        if (eff == 0) eff = g_minStrikeMs;   // notes struck before this feature existed
        if (eff > 0 && s_keyTiming[midi_note].onAtMs > 0) {
            uint32_t held = now - s_keyTiming[midi_note].onAtMs;
            if (held < eff) {
                s_keyTiming[midi_note].pendingReleaseMs =
                    s_keyTiming[midi_note].onAtMs + eff;
                // lastOffMs will be set when the deferred release actually
                // runs (in tickPendingFires), so retrigger-gap is measured
                // from the *real* release time, not the MIDI off time.
#if DEBUG_DISPATCH
                Serial.printf("[disp] noteOff %u deferred: held %lu ms < eff minStrike %lu\n",
                              midi_note, (unsigned long)held, (unsigned long)eff);
#endif
                return true;
            }
        }

        s_keyTiming[midi_note].lastOffMs = now;
    }
    return fireNoteOffNow(midi_note);
}

// Probes each PCA9685 every few seconds and re-initializes any that have
// stopped responding. PWM switching of high-current solenoid coils can
// inject enough noise on the shared I²C bus / 5V rail to glitch a chip's
// state machine. Without recovery, you'd notice notes silently dropping
// from one board until you reboot.
void tickChipHealth() {
    static uint32_t s_lastMs = 0;
    constexpr uint32_t kPeriodMs = 5000;
    uint32_t now = millis();
    if (now - s_lastMs < kPeriodMs) return;
    s_lastMs = now;

    for (int i = 0; i < kNumPowerBoards; i++) {
        auto& b = powerBoards[i];

        if (!b.connected) {
            // OFFLINE BOARD WITH A BELIEVED-ON COIL: the chip dropped off
            // the bus while (we think) driving a solenoid. It's outside
            // every normal release mechanism (per-channel retries need an
            // ACKing chip), so this 5 s tick is its only software bound —
            // keep hammering: force a bus recovery, try a per-chip verified
            // all-off, and fire the ALLCALL as a broadside. Only a VERIFIED
            // per-chip all-off (or the begin() below) clears its model.
            bool believedOn = false;
            for (uint8_t s = 0; s < 12; s++) {
                if (s_onAt[i][s] != 0 || s_needOff[i][s]) { believedOn = true; break; }
            }
            if (believedOn) {
                i2cForceRecovery();
                if (b.chip.allOff()) {   // verified per-chip release
                    for (uint8_t s = 0; s < 12; s++) { s_onAt[i][s] = 0; s_needOff[i][s] = false; }
                    Serial.printf("[health] offline board %d (0x%02X): verified all-off landed\n",
                                  i, b.chip.address());
                } else {
                    broadcastAllOff();   // best-effort broadside; model stays armed
                }
            }

            // HOT-RECONNECT: re-probe boards that were missing at boot or
            // dropped later. One cheap address ACK every 5 s; if the chip is
            // back, run the full init (begin() = VERIFIED all channels OFF),
            // sync the RAM model, and rejoin the active set. A board plugged
            // in after power-up "just starts working" within 5 s.
            if (b.chip.isConnected() && b.chip.begin()) {
                b.chip.setPWMFrequency(g_pwmFreqHz);
                for (uint8_t s = 0; s < 12; s++) { s_onAt[i][s] = 0; s_needOff[i][s] = false; }
                b.connected = true;
                Serial.printf("[health] board %d (0x%02X) DETECTED — online\n",
                              i, b.chip.address());
            }
            continue;
        }

        if (b.chip.isConnected()) continue;  // still ACKing — good

        Serial.printf("[health] board %d (0x%02X) NOT RESPONDING — re-initing\n",
                      i, b.chip.address());
        if (b.chip.begin()) {
            b.chip.setPWMFrequency(g_pwmFreqHz);
            // begin() reset the chip → all channels physically OFF. Sync the
            // RAM model so we don't keep tracking channels as energized (or
            // as needing an OFF) that the fresh chip already has low.
            for (uint8_t s = 0; s < 12; s++) { s_onAt[i][s] = 0; s_needOff[i][s] = false; }
            Serial.printf("[health] board %d (0x%02X) recovered\n",
                          i, b.chip.address());
        } else {
            // PERFORMANCE + CORRECTNESS: take the dead board OUT of the
            // active set. Leaving it "connected" made every dispatched note
            // to it burn a full I²C timeout (stall under load) and left its
            // RAM state stale. Its believed-on channels are physically
            // indeterminate — flag them so the watchdog keeps retrying OFFs
            // the moment the chip returns (the hot-reconnect path above also
            // force-clears via begin()). Notes to this board are skipped
            // cleanly by dispatch until it comes back.
            for (uint8_t s = 0; s < 12; s++) {
                if (s_onAt[i][s] != 0) s_needOff[i][s] = true;
            }
            b.connected = false;
            Serial.printf("[health] board %d (0x%02X) recovery FAILED — marked offline, will re-probe\n",
                          i, b.chip.address());
        }
    }
}

// Drain deferred fires + their post-fire releases. Cheap — 128 cmp ops per
// call. Designed to be called at the same cadence as tickAutoRelease (~100 Hz).
void tickPendingFires() {
    uint32_t now = millis();
    for (int n = 0; n < 128; n++) {
        if (s_keyTiming[n].pendingFireMs != 0 && now >= s_keyTiming[n].pendingFireMs) {
            fireNoteOnNow((uint8_t)n, s_keyTiming[n].pendingVelocity);
            s_keyTiming[n].pendingFireMs = 0;
            // If the source's NoteOff already scheduled a release that is due
            // this same tick (tick ran late, or DEFERRED_STRIKE_MIN_MS < tick
            // period), push it out so the just-struck note gets its minimum
            // audible time instead of firing and instantly releasing.
            if (s_keyTiming[n].pendingReleaseMs != 0 &&
                s_keyTiming[n].pendingReleaseMs <= now) {
                s_keyTiming[n].pendingReleaseMs = now + DEFERRED_STRIKE_MIN_MS;
            }
        }
        if (s_keyTiming[n].pendingReleaseMs != 0 && now >= s_keyTiming[n].pendingReleaseMs) {
            fireNoteOffNow((uint8_t)n);   // may START a soft-release cushion
            s_keyTiming[n].lastOffMs = now;
            s_keyTiming[n].pendingReleaseMs = 0;
        }
        // Soft-release cushion completion: after the cushion window, cut the
        // channel fully to 0. This is the true release when soft-release is on.
        // Do NOT touch lastOffMs here — it was already set at the real release
        // moment (dispatchNoteOff or the pendingRelease branch above); the
        // retrigger gap should measure from THAT, not the cushion-end time.
        if (s_keyTiming[n].cushionOffMs != 0 && now >= s_keyTiming[n].cushionOffMs) {
            hardOffNow((uint8_t)n);       // clears cushionOffMs + s_onAt
        }
    }
}

// Auto re-strike (tremolo sustain). While a note is held, re-hit it every
// g_restrikeMs so long notes stay audible. Each re-strike is a brief
// key-lift (channel off for g_restrikeLiftMs so the hammer resets) followed
// by a fresh strike. Bypasses gap/minstrike (direct fire/off) so it can't
// fight those.
//
// THERMAL SAFETY: each re-strike's fireNoteOnNow resets s_onAt, so the
// tickAutoRelease g_maxHoldMs watchdog would NEVER trip for a tremoloing
// note. So tickRestrike enforces the SAME thermal budget itself, measured
// from heldSinceMs (the first press, never reset by re-strikes) and capped
// at the runtime-clamped g_maxHoldMs. A held/orphaned note therefore can
// never drive its coil longer than g_maxHoldMs total — identical to a
// non-re-striking held note. No separate (larger) constant.
void tickRestrike() {
    // NOTE: intentionally NOT gated by "g_restrikeMs == 0" at the top — a
    // note could be mid-lift (channel off, waiting to strike back) when the
    // feature is turned off, and we must still complete that strike or the
    // note is stranded silent. We also always enforce the thermal bound on
    // held notes here regardless.
    uint32_t now = millis();
    for (int n = 0; n < 128; n++) {
        KeyTiming &kt = s_keyTiming[n];
        if (!kt.held) continue;

        // Drop notes that don't map to a real connected channel (out of
        // range / missing board): they were never energized.
        int idx = boardForNote((uint8_t)n);
        if (idx < 0) { kt.held = false; kt.reStrikeLifting = false; continue; }
        uint8_t st = (uint8_t)n - powerBoards[idx].midi_start;
        if (st >= 12) { kt.held = false; kt.reStrikeLifting = false; continue; }

        // Thermal / orphan bound from the FIRST press (never reset per
        // re-strike), inheriting the runtime-clamped g_maxHoldMs. This is
        // what stops re-strike from defeating the fire-safety watchdog.
        // Unsigned subtraction = millis()-rollover safe.
        if ((uint32_t)(now - kt.heldSinceMs) >= g_maxHoldMs) {
            hardOffNow((uint8_t)n);
            kt.held = false;
            kt.reStrikeLifting = false;
            continue;
        }

        // ALWAYS finish an in-progress lift (strike back on) so turning the
        // feature off mid-cycle can't leave the note silently off.
        if (kt.reStrikeLifting) {
            if ((int32_t)(now - kt.reStrikeAtMs) < 0) continue;   // gap not done
            fireNoteOnNow((uint8_t)n, kt.heldVel);
            kt.reStrikeLifting = false;
            kt.reStrikeAtMs = now + g_restrikeMs;
            continue;
        }

        // Beyond here we only START new re-strike cycles — skip if disabled.
        if (g_restrikeMs == 0) continue;

        // Don't interfere with a note mid-transition in the normal dispatch
        // path (deferred gap fire, deferred/min-strike release, cushion).
        if (kt.pendingFireMs || kt.pendingReleaseMs || kt.cushionOffMs) continue;

        if ((int32_t)(now - kt.reStrikeAtMs) < 0) continue;       // interval not up
        if (s_onAt[idx][st] == 0) continue;                       // not on → don't lift

        // Lift the key briefly so the hammer/jack resets before the next hit.
        hardOffNow((uint8_t)n);
        kt.reStrikeLifting = true;
        kt.reStrikeAtMs = now + g_restrikeLiftMs;
    }
}

// Auto-release watchdog. Walk every armed channel; if any has been held
// longer than g_maxHoldMs, force-release it. SAFETY NET ONLY — primary
// release path is NoteOff, this only catches orphans (dropped BLE packets,
// firmware bugs). With g_maxHoldMs = 5 s default, legitimate sustained
// notes are never cut short.
void tickAutoRelease() {
    uint32_t now = millis();
    bool anyPending = false;
    for (int i = 0; i < kNumPowerBoards; i++) {
        auto& b = powerBoards[i];
        if (!b.connected) continue;
        for (uint8_t s = 0; s < 12; s++) {
            // (a) A previous OFF write failed — retry it EVERY tick (fast),
            //     independent of the hold timeout. This is what stops a
            //     glitched OFF from stranding an energized coil.
            if (s_needOff[i][s]) {
                if (!tryChannelOff(i, s)) anyPending = true;
                continue;
            }
            // (b) Held longer than the safety timeout — force release.
            uint32_t t = s_onAt[i][s];
            if (t != 0 && (now - t) >= g_maxHoldMs) {
                if (!tryChannelOff(i, s)) anyPending = true;
#if DEBUG_DISPATCH
                else Serial.printf("[auto-release] board %d ch=%u after %lu ms\n",
                                   i, kKeyToChannel[s], (unsigned long)(now - t));
#endif
            }
        }
    }

    // Escalation: if any OFF write is still failing after several ticks, the
    // per-channel writes aren't getting through — hit the whole bus with an
    // ALLCALL FULL_OFF as a last resort.
    //
    // The RAM model (s_onAt/s_needOff) is cleared ONLY on a VERIFIED
    // broadcast. A verified clear is what stops the anti-spin problem (a
    // present-but-data-NACKing chip still ACKs the 0x70 broadcast, so the
    // flags clear and we stop re-broadcasting). An UNVERIFIED broadcast —
    // hung bus, nothing ACKs — must NOT clear anything: the coils may still
    // be energized, so we force a bus-recovery dance (bypassing its rate
    // limit) and keep every flag armed so the retry/escalation cycle
    // continues until a confirmed release lands.
    if (anyPending) {
        if (++s_offFailStreak >= 4) {
            Serial.println("[auto-release] OFF writes failing — ALLCALL FULL_OFF escalation");
            if (broadcastAllOff()) {
                s_offFailStreak = 0;
                // ALLCALL "verified" is bus-level (any chip's ACK) — it says
                // nothing about a chip that's OFF the bus. Only clear the
                // model for boards that are actually reachable; offline
                // boards stay armed for the health tick to keep working.
                for (int i = 0; i < kNumPowerBoards; i++) {
                    if (!powerBoards[i].connected) continue;
                    for (uint8_t s = 0; s < 12; s++) { s_onAt[i][s] = 0; s_needOff[i][s] = false; }
                }
            } else {
                // Broadcast didn't get through either — bus is hard-stuck.
                // Free it by force and re-escalate on the next failing pass.
                Serial.println("[auto-release] ALLCALL failed — forcing bus recovery");
                i2cForceRecovery();
                s_offFailStreak = 3;   // re-escalate after one more failing pass
            }
        }
    } else {
        s_offFailStreak = 0;
    }
}

void allKeysOff() {
    // First the per-board allOff (catches each chip individually with its
    // own primary address), then the ALLCALL broadcast (catches anything
    // the per-board write might have missed due to bus glitches).
    for (int i = 0; i < kNumPowerBoards; i++) {
        if (!powerBoards[i].connected) {
            // Board is offline — but it may have dropped off the bus WHILE
            // driving a coil (boards can go offline after being energized,
            // via tickChipHealth). We cannot verify a release on a chip
            // that isn't ACKing, so keep its believed-on channels flagged;
            // the health tick keeps hammering it and only a VERIFIED
            // all-off/begin() clears the model.
            for (uint8_t s = 0; s < 12; s++) {
                if (s_onAt[i][s] != 0) s_needOff[i][s] = true;
            }
            continue;
        }
        if (powerBoards[i].chip.allOff()) {
            // Confirmed released — safe to clear this board's model.
            for (uint8_t s = 0; s < 12; s++) { s_onAt[i][s] = 0; s_needOff[i][s] = false; }
        } else {
            // Unconfirmed — coils may still be driven. Flag every channel we
            // believe is on for fast watchdog retry instead of forgetting it.
            for (uint8_t s = 0; s < 12; s++) {
                if (s_onAt[i][s] != 0) s_needOff[i][s] = true;
            }
        }
    }
    // Clearing deferred timers is ALWAYS safe (they only ever energize):
    // without this, a queued pendingFire or cushion could re-energize a
    // solenoid moments after a panic/emergency-stop.
    for (int n = 0; n < 128; n++) {
        s_keyTiming[n].pendingFireMs    = 0;
        s_keyTiming[n].pendingReleaseMs = 0;
        s_keyTiming[n].cushionOffMs     = 0;
        s_keyTiming[n].held             = false;   // stop any auto re-strike
        s_keyTiming[n].reStrikeLifting  = false;
    }
    s_offFailStreak = 0;
    // Final belt-and-suspenders sweep. Verified success → whole model clear.
    if (broadcastAllOff()) {
        // Same rule as the escalation clear: a bus-level ACK proves nothing
        // for a chip that's off the bus — only clear reachable boards.
        for (int i = 0; i < kNumPowerBoards; i++) {
            if (!powerBoards[i].connected) continue;
            for (uint8_t s = 0; s < 12; s++) { s_onAt[i][s] = 0; s_needOff[i][s] = false; }
        }
    }
}

// Force every PCA9685 channel on every chip to FULL_OFF in one I²C
// transaction via the ALLCALL address. Works even if a chip's primary
// address dipswitch is misset — every PCA9685 also listens to 0x70 by
// default.
//
// Reaches the chips directly, bypassing the PCA9685 class so it works
// even if AI was somehow disabled in a chip's MODE1. We re-enable AI
// here as part of the broadcast.
bool broadcastAllOff() {
    bool ok1, ok2;
    // 1. Wake + enable AI on every chip via ALLCALL.
    {
        WireGuard guard;
        PCA_BUS.beginTransmission(0x70);
        PCA_BUS.write(0x00);   // MODE1
        PCA_BUS.write(0x21);   // AI=1, SLEEP=0, ALLCALL=1
        ok1 = (PCA_BUS.endTransmission() == 0);
    }
    delayMicroseconds(600);   // oscillator startup, in case some chip slept

    // 2. Burst write to ALL_LED_ON_L..ALL_LED_OFF_H with FULL_OFF set.
    //    This sets every channel of every PCA9685 on the bus to FULL_OFF
    //    simultaneously.
    {
        WireGuard guard;
        PCA_BUS.beginTransmission(0x70);
        PCA_BUS.write(0xFA);   // ALL_LED_ON_L (AI walks to OFF_H)
        PCA_BUS.write(0x00);   // ALL_LED_ON_L
        PCA_BUS.write(0x00);   // ALL_LED_ON_H
        PCA_BUS.write(0x00);   // ALL_LED_OFF_L
        PCA_BUS.write(0x10);   // ALL_LED_OFF_H — FULL_OFF bit set
        ok2 = (PCA_BUS.endTransmission() == 0);
    }
    if (!(ok1 && ok2)) g_i2cFailCount++;
    return ok1 && ok2;
}

// Fire one specific board+channel at the given raw PWM. Bypasses MIDI
// dispatch but arms the auto-release watchdog at the corresponding semitone
// slot — so a stuck "fire" command still releases after g_maxHoldMs.
//
// SAFETY: the auto-release watchdog can only cover channels that map to a
// semitone in kKeyToChannel[] (0-6 and 11-15). Channels 7-10 are unwired
// on the PCB and have NO watchdog slot — energizing one would leave it on
// forever (fire hazard). So we REFUSE to energize any unmapped channel.
// Turning a channel OFF (pwm==0) is always allowed.
bool manualFire(uint8_t board_idx, uint8_t channel, uint16_t pwm) {
    if (board_idx >= kNumPowerBoards) return false;
    auto& b = powerBoards[board_idx];
    if (!b.connected) return false;
    if (channel >= 16) return false;

    // Map channel → semitone slot.
    int semitone = -1;
    for (uint8_t s = 0; s < 12; s++) {
        if (kKeyToChannel[s] == channel) { semitone = s; break; }
    }

    if (semitone < 0) {
        // Unmapped channel (7-10): never leave it energized. Force off and
        // reject any attempt to drive it.
        b.chip.setPWM(channel, 0);
        if (pwm != 0) {
            Serial.printf("[fire] REFUSED ch %u (unwired, no watchdog slot)\n", channel);
            return false;
        }
        return true;
    }

    if (pwm == 0) {
        // Route OFF through the confirmed-success path so a failed manual OFF
        // re-arms s_needOff for the watchdog to retry — same invariant as
        // every other release. (Was: unconditional s_onAt=0, which stranded a
        // coil if the OFF write glitched.)
        tryChannelOff(board_idx, semitone);
        return true;
    }
    b.chip.setPWM(channel, pwm);
    s_onAt[board_idx][semitone]   = millis();   // arm watchdog
    s_needOff[board_idx][semitone] = false;     // fresh energize supersedes any pending-off
    return true;
}

// Fire a MIDI note at a raw PWM, bypassing the whole velocity/soft-release
// dispatch. Maps note → board + channel, then reuses manualFire's safe path.
bool manualFireNote(uint8_t midi_note, uint16_t pwm) {
    int idx = boardForNote(midi_note);
    if (idx < 0) return false;
    auto& b = powerBoards[idx];
    uint8_t semitone = midi_note - b.midi_start;
    if (semitone >= 12) return false;
    // Also clear any dispatch timing state so the normal path doesn't fight
    // the test (e.g. a lingering cushion or pending fire on this note).
    if (midi_note < 128) {
        s_keyTiming[midi_note].pendingFireMs    = 0;
        s_keyTiming[midi_note].pendingReleaseMs = 0;
        s_keyTiming[midi_note].cushionOffMs     = 0;
    }
    return manualFire((uint8_t)idx, kKeyToChannel[semitone], pwm);
}

// SAFE sweep: one solenoid at a time, sequentially across boards. Peak
// current draw is just one solenoid (~0.5 A from 24 V), nowhere near the
// supply limit even if something's wired wrong. Lets you hear and confirm
// each individual key works.
//
// Order: board 0 plays C → C# → D → … → B, then board 1 plays C → … → B,
// etc. Each strike uses PWM ≈ 2500 / 4095 ≈ 60 % duty for 150 ms (plenty
// to actuate but not full power), then releases for 80 ms before the next.
//
// Total time ≈ N_boards × 12 × 230 ms (≈ 14 s for 6 boards). Set
// RUN_TEST_SWEEP_ON_BOOT 0 in config.h to skip.
void testSweepAllBoards() {
    static const char *kNoteNames[12] = {
        "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
    };
    Serial.println("[test] sweep start — ONE solenoid at a time (~0.5 A peak)");

    for (int i = 0; i < kNumPowerBoards; i++) {
        auto& b = powerBoards[i];
        if (!b.connected) {
            Serial.printf("[test]   board %d (0x%02X): skipped (missing)\n",
                          i, b.chip.address());
            continue;
        }
        Serial.printf("[test]   board %d (0x%02X) — strikes C1+offset..B:\n",
                      i, b.chip.address());
        for (uint8_t semitone = 0; semitone < 12; semitone++) {
            uint8_t ch = kKeyToChannel[semitone];
            Serial.printf("[test]     %-2s → PCA9685 ch %2u\n",
                          kNoteNames[semitone], ch);
            b.chip.setPWM(ch, g_sweepStrikePWM);
            delay(SWEEP_STRIKE_MS);
            // Redundant release: write FULL_OFF twice with a small gap.
            // Defends against Wire bus contention with the touch driver
            // on core 0 — a single corrupted transaction can't leave a
            // channel stuck if we always send the release twice.
            b.chip.setPWM(ch, 0);
            delayMicroseconds(200);
            b.chip.setPWM(ch, 0);
            delay(SWEEP_RELEASE_MS);
            // Sweep run can total ~15 s; feed the watchdog so we don't
            // false-trigger a reboot mid-sweep.
            esp_task_wdt_reset();
        }
        // After each board, also force its own allOff() — third release
        // attempt with a different code path (ALL_LED registers vs per-
        // channel).
        b.chip.allOff();
    }
    // Final triple-safety net: ALLCALL FULL_OFF to every PCA9685 on the
    // bus. If anything is somehow still latched HIGH from a glitched
    // earlier write, this catches it.
    broadcastAllOff();
    allKeysOff();
    Serial.println("[test] sweep complete — all solenoids released (triple-checked)");
}
