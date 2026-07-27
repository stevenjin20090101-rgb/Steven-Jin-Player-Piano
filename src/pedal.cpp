// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#include "pedal.h"
#include "config.h"
#include "pca9685.h"

// Tunables (persisted in NVS — see settings.cpp).
bool     g_pedalEnabled    = (DEFAULT_PEDAL_ENABLED != 0);
uint16_t g_pedalUpCounts   = DEFAULT_PEDAL_UP;
uint16_t g_pedalDownCounts = DEFAULT_PEDAL_DOWN;
bool     g_pedalHalf       = (DEFAULT_PEDAL_HALF != 0);

static PCA9685 s_pedalChip(PEDAL_PCA_ADDR);
static bool     s_present     = false;
static uint16_t s_lastWritten = 0xFFFF;   // counts last sent (0xFFFF = none yet)
static uint32_t s_lastWriteMs = 0;
static uint32_t s_lastProbeMs = 0;

// A servo needs a fresh pulse only when its target moves. Writing on every
// loop would pour needless traffic onto the same bus the solenoids use, so
// writes are change-gated AND rate-limited.
static const uint32_t kMinWriteGapMs = 15;
static const uint32_t kReprobeMs     = 5000;

bool pedal_present() { return s_present; }

// Bring the chip up at the SERVO rate. Separate chip = its own frequency, so
// this can't disturb the solenoid boards' carrier.
static bool bringUp() {
    if (!s_pedalChip.begin()) return false;
    if (!s_pedalChip.setPWMFrequency(PEDAL_SERVO_HZ)) return false;
    s_lastWritten = 0xFFFF;      // force the next position write
    return true;
}

void pedal_init() {
    s_present = false;
    s_lastProbeMs = millis();
    if (!g_pedalEnabled) {
        Serial.println("[pedal] disabled (pedalon 1 to enable once wired)");
        return;
    }
    if (!s_pedalChip.isConnected()) {
        Serial.printf("[pedal] no pedal board at 0x%02X — pedal features idle\n",
                      PEDAL_PCA_ADDR);
        return;
    }
    s_present = bringUp();
    Serial.printf("[pedal] %s at 0x%02X (servo %d Hz, ch %d)\n",
                  s_present ? "ready" : "FOUND but init FAILED",
                  PEDAL_PCA_ADDR, PEDAL_SERVO_HZ, PEDAL_PCA_CHANNEL);
}

// Map a CC64 value to servo counts.
//   half-pedal off: <64 = up, >=64 = down (standard MIDI switch behaviour)
//   half-pedal on : linear across the travel, so partial pedalling carries
static uint16_t countsFor(uint8_t cc) {
    int up = g_pedalUpCounts, down = g_pedalDownCounts;
    if (!g_pedalHalf) return (cc >= 64) ? (uint16_t)down : (uint16_t)up;
    return (uint16_t)(up + ((long)(down - up) * cc) / 127);
}

static void writeCounts(uint16_t counts) {
    if (!s_present) return;
    uint32_t now = millis();
    if (counts == s_lastWritten) return;                 // nothing changed
    if (now - s_lastWriteMs < kMinWriteGapMs) return;    // rate limit
    if (s_pedalChip.setPWM(PEDAL_PCA_CHANNEL, counts)) {
        s_lastWritten = counts;
        s_lastWriteMs = now;
    } else {
        // Lost the chip (cable knocked out mid-song). Drop to absent so the
        // re-probe below can recover it; never spin on a dead address.
        s_present = false;
        s_lastProbeMs = now;
        Serial.println("[pedal] write failed — pedal board offline, will retry");
    }
}

void pedal_tick(uint8_t cc64Value) {
    if (!g_pedalEnabled) return;

    // Hot-(re)connect: the pedal board may be powered up after us, or come back
    // after a knock. Cheap address probe every 5 s while absent — same pattern
    // as the power boards' health tick.
    if (!s_present) {
        uint32_t now = millis();
        if (now - s_lastProbeMs >= kReprobeMs) {
            s_lastProbeMs = now;
            if (s_pedalChip.isConnected() && bringUp()) {
                s_present = true;
                Serial.println("[pedal] pedal board back online");
            }
        }
        return;
    }
    writeCounts(countsFor(cc64Value));
}

void pedal_release() {
    if (!g_pedalEnabled || !s_present) return;
    // Bypass the rate limit — a stop must lift the dampers immediately, or the
    // piano keeps ringing after an all-off/panic.
    if (s_pedalChip.setPWM(PEDAL_PCA_CHANNEL, g_pedalUpCounts)) {
        s_lastWritten = g_pedalUpCounts;
        s_lastWriteMs = millis();
    } else {
        s_lastWritten = 0xFFFF;   // force a retry on the next tick
    }
}

void pedal_test_counts(uint16_t counts) {
    if (!g_pedalEnabled || !s_present) return;
    if (s_pedalChip.setPWM(PEDAL_PCA_CHANNEL, counts)) {
        s_lastWritten = counts;
        s_lastWriteMs = millis();
    }
}

void pedal_print_status() {
    Serial.printf("  pedal servo: %s  board=%s(0x%02X)  up=%u down=%u half=%s\n",
                  g_pedalEnabled ? "ENABLED" : "disabled",
                  s_present ? "online" : "absent",
                  PEDAL_PCA_ADDR,
                  g_pedalUpCounts, g_pedalDownCounts,
                  g_pedalHalf ? "on" : "off");
}
