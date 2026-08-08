// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#include "settings.h"
#include "config.h"
#include "power_boards.h"
#include "ui.h"
#include "pedal.h"

#include <Preferences.h>
#include <esp_system.h>   // esp_reset_reason()

// All keys live under one namespace. Each key must be ≤ 15 chars per NVS
// spec. Names kept short on purpose — they're stored verbatim in flash.
static const char *kNS = "piano";

// NVS uses wear-leveling and internally compares the new value to the
// existing one before committing, so calling putXxx with the same value
// is a no-op for flash wear. We can safely save the whole settings
// snapshot periodically without worrying about wearing out flash.
static const uint32_t kAutoSavePeriodMs = 30000;   // 30 s
static uint32_t s_lastSaveMs = 0;

void settings_load() {
    Preferences p;
    if (!p.begin(kNS, /*readOnly=*/true)) {
        Serial.println("[settings] NVS namespace missing — using defaults");
        return;
    }

    g_sweepStrikePWM = p.getUInt  ("sweepPWM",  SWEEP_STRIKE_PWM);
    g_minStrikePWM   = p.getUInt  ("minPWM",    MIN_STRIKE_PWM);
    g_maxStrikePWM   = p.getUInt  ("maxPWM",    MAX_STRIKE_PWM);
    set_max_hold_ms(p.getUInt("holdMs", DEFAULT_MAX_HOLD_MS));  // clamp NVS value too
    g_velocityMult   = p.getFloat ("velmult",   1.0f);
    g_fullPowerMode  = p.getBool  ("fullpower", DEFAULT_FULL_POWER_MODE != 0);
    g_pwmFreqHz      = p.getUShort("pwmFreq",   DEFAULT_PWM_FREQ_HZ);
    g_minRetriggerGapMs = p.getUInt("retGapMs", DEFAULT_MIN_RETRIGGER_GAP_MS);
    g_minStrikeMs       = p.getUInt("minStrike", DEFAULT_MIN_STRIKE_MS);
    g_isoStrikeMs       = p.getUInt("isoStrike", DEFAULT_ISO_STRIKE_MS);
    g_isoGapMs          = p.getUInt("isoGap",    DEFAULT_ISO_GAP_MS);
    set_master_volume(p.getUChar("volume", DEFAULT_MASTER_VOLUME));
    g_velCurve          = p.getFloat("velCurve",  DEFAULT_VEL_CURVE);
    g_humanizeVel       = p.getUChar("humanVel",  DEFAULT_HUMANIZE_VEL);
    g_humanizeMs        = p.getUChar("humanMs",   DEFAULT_HUMANIZE_MS);
    g_waterfallEnabled  = p.getBool("waterfall", DEFAULT_WATERFALL_ENABLED != 0);
    g_keyboardVizEnabled = p.getBool("keyviz", true);
    g_softRelease       = p.getBool("softRel",  DEFAULT_SOFT_RELEASE != 0);
    g_releasePwm        = p.getUShort("relPwm", DEFAULT_RELEASE_PWM);
    g_releaseMs         = p.getUInt("relMs",    DEFAULT_RELEASE_MS);
    g_ledsEnabled       = p.getBool("leds",     DEFAULT_LEDS_ENABLED != 0);
    g_restrikeMs        = p.getUInt("restrike", DEFAULT_RESTRIKE_MS);
    g_idleDimSecs       = p.getUInt("dimSecs",  DEFAULT_IDLE_DIM_SECS);
    g_pedalEnabled      = p.getBool  ("pedalOn",   DEFAULT_PEDAL_ENABLED != 0);
    g_pedalUpCounts     = p.getUShort("pedalUp",   DEFAULT_PEDAL_UP);
    g_pedalDownCounts   = p.getUShort("pedalDn",   DEFAULT_PEDAL_DOWN);
    g_pedalHalf         = p.getBool  ("pedalHalf", DEFAULT_PEDAL_HALF != 0);

    appState.ledCount           = p.getUShort("ledCount",  DEFAULT_LED_ACTIVE);
    appState.ledOffset          = p.getShort ("ledOffset", DEFAULT_LED_OFFSET);
    appState.ledScalePct        = p.getUShort("ledScale",  DEFAULT_LED_SCALE_PCT);
    appState.ledTail            = p.getUChar ("ledTail",   DEFAULT_LED_TAIL);
    appState.ledReverse         = p.getBool  ("ledRev",    DEFAULT_LED_REVERSE != 0);
    appState.ledReactivePalette = p.getUChar ("reactPal",  DEFAULT_REACT_PALETTE);
    appState.ledGlow            = p.getUChar ("ledGlow",   DEFAULT_LED_GLOW);
    appState.ledVelBright       = p.getBool  ("ledVelBri", DEFAULT_LED_VELBRIGHT != 0);

    appState.screenBrightness = p.getUChar("scrBri",  180);
    appState.ledBrightness    = p.getUChar("ledBri",  LED_DEFAULT_BRIGHTNESS);
    appState.ledMode          = (LedMode)p.getUChar("ledMode", LED_MODE_NOTE_REACTIVE);
    appState.ledStaticColor   = p.getUInt ("ledColor", 0x0040FF);
    appState.rainbowSpeed     = p.getUChar("rainSpd", 1);
    appState.noteDecayRate    = p.getUChar("decay",   6);
    appState.inputMode        = (InputMode)p.getUChar("inMode", INPUT_MODE_BLE_ONLY);
    appState.touchVelocity    = p.getUChar("touchVel", 100);

    // Per-key force multiplier table. Stored as a single 512-byte blob
    // (128 floats × 4 bytes). Only loaded if a previous save exists —
    // otherwise leave the in-RAM defaults (all 1.0).
    size_t got = p.getBytes("keyForce", g_keyForceMult, sizeof(g_keyForceMult));
    if (got != sizeof(g_keyForceMult)) {
        Serial.printf("[settings] keyForce blob missing (%u bytes) — defaults stand\n",
                      (unsigned)got);
    }

    p.end();
    Serial.println("[settings] loaded from NVS");
}

void settings_save() {
    Preferences p;
    if (!p.begin(kNS, /*readOnly=*/false)) {
        Serial.println("[settings] save FAILED — could not open NVS RW");
        return;
    }

    p.putUInt  ("sweepPWM",  g_sweepStrikePWM);
    p.putUInt  ("minPWM",    g_minStrikePWM);
    p.putUInt  ("maxPWM",    g_maxStrikePWM);
    p.putUInt  ("holdMs",    g_maxHoldMs);
    p.putFloat ("velmult",   g_velocityMult);
    p.putBool  ("fullpower", g_fullPowerMode);
    p.putUShort("pwmFreq",   g_pwmFreqHz);
    p.putUInt  ("retGapMs",  g_minRetriggerGapMs);
    p.putUInt  ("minStrike", g_minStrikeMs);
    p.putUInt  ("isoStrike", g_isoStrikeMs);
    p.putUInt  ("isoGap",    g_isoGapMs);
    p.putUChar ("volume",    g_masterVolume);
    p.putFloat ("velCurve",  g_velCurve);
    p.putUChar ("humanVel",  g_humanizeVel);
    p.putUChar ("humanMs",   g_humanizeMs);
    p.putBool  ("waterfall", g_waterfallEnabled);
    p.putBool  ("keyviz",    g_keyboardVizEnabled);
    p.putBool  ("softRel",   g_softRelease);
    p.putUShort("relPwm",    g_releasePwm);
    p.putUInt  ("relMs",     g_releaseMs);
    p.putBool  ("leds",      g_ledsEnabled);
    p.putUInt  ("restrike",  g_restrikeMs);
    p.putUInt  ("dimSecs",   g_idleDimSecs);
    p.putBool  ("pedalOn",   g_pedalEnabled);
    p.putUShort("pedalUp",   g_pedalUpCounts);
    p.putUShort("pedalDn",   g_pedalDownCounts);
    p.putBool  ("pedalHalf", g_pedalHalf);

    p.putUShort("ledCount",  appState.ledCount);
    p.putShort ("ledOffset", appState.ledOffset);
    p.putUShort("ledScale",  appState.ledScalePct);
    p.putUChar ("ledTail",   appState.ledTail);
    p.putBool  ("ledRev",    appState.ledReverse);
    p.putUChar ("reactPal",  appState.ledReactivePalette);
    p.putUChar ("ledGlow",   appState.ledGlow);
    p.putBool  ("ledVelBri", appState.ledVelBright);

    p.putUChar("scrBri",  appState.screenBrightness);
    p.putUChar("ledBri",  appState.ledBrightness);
    p.putUChar("ledMode", (uint8_t)appState.ledMode);
    p.putUInt ("ledColor", appState.ledStaticColor);
    p.putUChar("rainSpd", appState.rainbowSpeed);
    p.putUChar("decay",   appState.noteDecayRate);
    p.putUChar("inMode",  (uint8_t)appState.inputMode);
    p.putUChar("touchVel", appState.touchVelocity);

    // 128-float per-key force multiplier table.
    p.putBytes("keyForce", g_keyForceMult, sizeof(g_keyForceMult));

    p.end();
    s_lastSaveMs = millis();
    Serial.println("[settings] saved to NVS");
}

// Cheap RAM snapshot of everything settings_save() persists. settings_tick
// compares against this and SKIPS the whole NVS open/walk/commit when
// nothing changed — which is almost every tick on the permanent install.
// (NVS dedupes writes internally, but the namespace open + 25 key walks +
// the 512-byte blob compare still cost ms; a memcmp costs microseconds.
// It also avoids the rare multi-10ms flash-compaction stall mid-song.)
struct SettingsSnap {
    uint32_t sweepPWM, minPWM, maxPWM, holdMs, retGapMs, minStrike, relMs,
             restrike, dimSecs, ledColor, isoStrike, isoGap;
    uint16_t pwmFreq, relPwm, ledCount, ledScale, pedalUp, pedalDn;
    int16_t  ledOffset;
    float    velmult, velCurve;
    bool     fullpower, waterfall, keyviz, softRel, leds, ledReverse, ledVelBri,
             pedalOn, pedalHalf;
    uint8_t  scrBri, ledBri, ledMode, rainSpd, decay, inMode, touchVel, reactPal, ledGlow, ledTail, volume, humanVel, humanMs;
    float    keyForce[128];
};
static SettingsSnap s_snap;
static bool s_snapValid = false;

static void takeSnap(SettingsSnap &s) {
    memset(&s, 0, sizeof(s));   // zero padding bytes so memcmp is reliable
    s.sweepPWM = g_sweepStrikePWM; s.minPWM = g_minStrikePWM;
    s.maxPWM = g_maxStrikePWM;     s.holdMs = g_maxHoldMs;
    s.retGapMs = g_minRetriggerGapMs; s.minStrike = g_minStrikeMs;
    s.isoStrike = g_isoStrikeMs; s.isoGap = g_isoGapMs;
    s.relMs = g_releaseMs;         s.restrike = g_restrikeMs;
    s.dimSecs = g_idleDimSecs;     s.ledColor = appState.ledStaticColor;
    s.pwmFreq = g_pwmFreqHz;       s.relPwm = g_releasePwm;
    s.velmult = g_velocityMult;
    s.fullpower = g_fullPowerMode; s.waterfall = g_waterfallEnabled;
    s.keyviz = g_keyboardVizEnabled; s.softRel = g_softRelease;
    s.leds = g_ledsEnabled;
    s.scrBri = appState.screenBrightness; s.ledBri = appState.ledBrightness;
    s.ledMode = (uint8_t)appState.ledMode; s.rainSpd = appState.rainbowSpeed;
    s.decay = appState.noteDecayRate; s.inMode = (uint8_t)appState.inputMode;
    s.touchVel = appState.touchVelocity;
    s.ledCount = appState.ledCount; s.ledOffset = appState.ledOffset;
    s.ledScale = appState.ledScalePct;
    s.ledReverse = appState.ledReverse; s.reactPal = appState.ledReactivePalette;
    s.ledGlow = appState.ledGlow; s.ledVelBri = appState.ledVelBright;
    s.ledTail = appState.ledTail;
    s.volume = g_masterVolume; s.velCurve = g_velCurve;
    s.humanVel = g_humanizeVel; s.humanMs = g_humanizeMs;
    s.pedalOn = g_pedalEnabled; s.pedalUp = g_pedalUpCounts;
    s.pedalDn = g_pedalDownCounts; s.pedalHalf = g_pedalHalf;
    memcpy(s.keyForce, g_keyForceMult, sizeof(s.keyForce));
}

void settings_tick() {
    uint32_t now = millis();
    if (now - s_lastSaveMs < kAutoSavePeriodMs) return;

    SettingsSnap cur;
    takeSnap(cur);
    if (s_snapValid && memcmp(&cur, &s_snap, sizeof(cur)) == 0) {
        s_lastSaveMs = now;    // nothing changed — skip the NVS walk entirely
        return;
    }
    settings_save();
    s_snap = cur;
    s_snapValid = true;
}

void settings_factory() {
    Preferences p;
    if (p.begin(kNS, /*readOnly=*/false)) {
        p.clear();
        p.end();
        Serial.println("[settings] NVS cleared — reboot to load factory defaults");
    } else {
        Serial.println("[settings] factory FAILED — could not open NVS RW");
    }
}

// Crash-loop guard. Only counts CRASH reboots (panic / watchdog / brownout)
// — NOT clean power-ons or reset-button presses. This matters for a public
// install: someone flipping the power switch off/on repeatedly must NOT trip
// safe mode; only genuine crashes should. After 3 crashes in a row, force
// Full Power (crash-free) so we're not stuck in a PWM crash loop.
uint8_t settings_boot_inc() {
    esp_reset_reason_t rr = esp_reset_reason();
    bool wasCrash = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                     rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                     rr == ESP_RST_BROWNOUT);

    Preferences p;
    if (!p.begin(kNS, /*readOnly=*/false)) return 0;

    if (!wasCrash) {
        // Clean boot (power-on / reset button) — reset the crash counter.
        if (p.getUChar("bootCnt", 0) != 0) p.putUChar("bootCnt", 0);
        p.end();
        return 0;
    }

    uint8_t n = p.getUChar("bootCnt", 0) + 1;
    p.putUChar("bootCnt", n);
    p.end();
    Serial.printf("[settings] CRASH reboot — crash count = %u\n", n);
    if (n >= 3) {
        Serial.println("[settings] *** SAFE MODE: 3 crashes in a row, forcing Full Power ON ***");
        g_fullPowerMode = true;
    }
    return n;
}

// Reset boot counter — call after firmware has been stable for ~30 s, so
// next normal boot starts fresh.
void settings_boot_reset() {
    Preferences p;
    if (!p.begin(kNS, /*readOnly=*/false)) return;
    if (p.getUChar("bootCnt", 0) != 0) {
        p.putUChar("bootCnt", 0);
        Serial.println("[settings] boot count cleared (stable uptime reached)");
    }
    p.end();
}
