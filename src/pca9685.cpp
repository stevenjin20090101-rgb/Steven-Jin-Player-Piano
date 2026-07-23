// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#include "pca9685.h"
#include "config.h"   // for I2C_SDA_PIN, I2C_SCL_PIN

// Bus mutex. Guards every PCA_BUS transaction. Even on a dedicated bus,
// broadcastAllOff() can be called from the NimBLE task (core 0) via the
// disconnect callback while dispatch runs on core 1 — the mutex keeps
// those two from colliding on Wire1.
SemaphoreHandle_t g_wireMutex = nullptr;
void wire_lock_init() {
    if (!g_wireMutex) g_wireMutex = xSemaphoreCreateMutex();
}

// Running count of I²C writes that failed after retry. Surfaced in the
// status output + heartbeat so an EMI storm is visible in the logs instead
// of just "feeling laggy".
uint32_t g_i2cFailCount = 0;

// Classic I²C bus-recovery dance. A slave can hold SDA low after a
// botched transaction (e.g., we crashed mid-write); the bus is then
// stuck because the master sees SDA low and assumes another master is
// transmitting. Manually pulsing SCL up to 9 times forces the slave's
// state machine to advance to a STOP condition, then a STOP is generated
// on SDA. After this, PCA_BUS.begin() recovers normal operation.
//
// RATE-LIMITED to once per 100 ms: under a PWM-EMI failure storm (dozens
// of glitched writes per second), running the full teardown + re-init per
// failed write serialized the dispatch loop for hundreds of ms — the
// "piano sticks when many keys play in PWM mode" symptom. Failed OFF
// writes don't need the dance to be safe anyway: the s_needOff watchdog
// retries them every 10 ms.
// File-scope so i2cForceRecovery() can bypass the limiter. Initialized to
// "long ago" so the FIRST recovery after boot always runs (a bus stuck at
// power-on would otherwise be unrecoverable for the first 100 ms window).
static uint32_t s_lastRecoveryMs = (uint32_t)-100000;

static void recoverI2CBus() {
    uint32_t now = millis();
    if (now - s_lastRecoveryMs < 100) return;   // storm guard
    s_lastRecoveryMs = now;
    PCA_BUS.end();
    pinMode(I2C_SCL_PIN, OUTPUT_OPEN_DRAIN);
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);
    // 9 SCL pulses — enough to step any stuck slave through to STOP
    for (int i = 0; i < 9; ++i) {
        digitalWrite(I2C_SCL_PIN, LOW);
        delayMicroseconds(5);
        digitalWrite(I2C_SCL_PIN, HIGH);
        delayMicroseconds(5);
    }
    // Generate a STOP: SDA goes high while SCL is high
    pinMode(I2C_SDA_PIN, OUTPUT_OPEN_DRAIN);
    digitalWrite(I2C_SDA_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(I2C_SDA_PIN, HIGH);
    delayMicroseconds(5);
    // Restore Wire1 control of the pins
    PCA_BUS.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
    Serial.println("[i2c] PCA bus recovery dance done");
}

// Force an immediate recovery attempt regardless of the rate limit —
// the safety escalation's last resort when even ALLCALL writes fail.
void i2cForceRecovery() {
    s_lastRecoveryMs = millis() - 1000;   // disarm the storm guard
    recoverI2CBus();
}

bool PCA9685::writeReg(uint8_t reg, uint8_t val) {
    // Thin wrapper over writeBurst so single-register writes inherit the
    // same recovery + retry + failure-counting behavior as bursts.
    return writeBurst(reg, &val, 1);
}

bool PCA9685::writeBurst(uint8_t reg, const uint8_t *data, uint8_t n) {
    // Relies on MODE1.AI=1 set in begin(). One START, register pointer
    // auto-increments through the n bytes, one STOP. Mutex protects the
    // whole transaction from cross-core preemption.
    WireGuard guard;
    PCA_BUS.beginTransmission(_addr);
    PCA_BUS.write(reg);
    for (uint8_t i = 0; i < n; i++) PCA_BUS.write(data[i]);
    uint8_t err = PCA_BUS.endTransmission();
    if (err == 0) return true;

    // Err codes: 2=NACK addr, 3=NACK data, 4=other, 5=timeout.
    // 4/5 mean the bus is hung — a slave's stuck holding SDA low and the
    // master can't START. Try the (rate-limited) recovery dance, then one
    // retry. Skip recovery on simple NACKs (chip missing or just busy).
    if (err == 4 || err == 5) {
        recoverI2CBus();
        PCA_BUS.beginTransmission(_addr);
        PCA_BUS.write(reg);
        for (uint8_t i = 0; i < n; i++) PCA_BUS.write(data[i]);
        if (PCA_BUS.endTransmission() == 0) return true;
    }
    g_i2cFailCount++;
    return false;
}

bool PCA9685::begin() {
    using namespace PCA9685Reg;
    bool ok = true;

    // 1. Enable AI while still ASLEEP. Registers are writable during sleep,
    //    and we deliberately write the FULL_OFF state BEFORE waking so a
    //    chip that retained latched-ON outputs from a previous run can't
    //    re-drive its coils for even a moment at wake-up. (FULL_OFF has
    //    priority over FULL_ON per the datasheet, and is static — it does
    //    not need the oscillator.)
    if (!writeReg(MODE1, MODE1_AI | MODE1_SLEEP | MODE1_ALLCALL)) return false;

    // 2. Force every channel FULL_OFF — VERIFIED. Callers use begin()'s
    //    return to decide the chip's outputs are safe (clearing s_onAt /
    //    s_needOff, setting connected=true). An unverified all-off here
    //    would let the RAM model be wiped while coils are still driven —
    //    the exact stuck-coil hazard the model exists to prevent.
    if (!allOff()) return false;

    // 3. Wake (SLEEP=0). Outputs come up already FULL_OFF.
    ok &= writeReg(MODE1, MODE1_AI | MODE1_ALLCALL);
    delayMicroseconds(600);   // ~500 µs oscillator startup per datasheet

    // 4. MODE2: OUTDRV=1 (totem-pole, drives MOSFET gates directly),
    //    OCH=0 (update outputs on STOP — cleaner than per-ACK).
    ok &= writeReg(MODE2, 0x04);

    return ok;
}

bool PCA9685::isConnected() {
    WireGuard guard;
    PCA_BUS.beginTransmission(_addr);
    return PCA_BUS.endTransmission() == 0;
}

bool PCA9685::setPWM(uint8_t channel, uint16_t value) {
    if (channel >= 16) return false;
    uint8_t base = PCA9685Reg::LED0_ON_L + 4 * channel;
    uint8_t buf[4];

    if (value == 0) {
        // Hard off: FULL_OFF bit, ignores ON/OFF compare values.
        buf[0] = 0x00;                  // ON_L
        buf[1] = 0x00;                  // ON_H
        buf[2] = 0x00;                  // OFF_L
        buf[3] = PCA9685Reg::FULL_OFF;  // OFF_H = 0x10
    } else if (value >= 4095) {
        // Hard on: FULL_ON bit (ON_H bit 4).
        buf[0] = 0x00;                  // ON_L
        buf[1] = 0x10;                  // ON_H FULL_ON
        buf[2] = 0x00;                  // OFF_L
        buf[3] = 0x00;                  // OFF_H
    } else {
        // Variable PWM: rising edge at count 0, falling edge at `value`.
        buf[0] = 0x00;                       // ON_L
        buf[1] = 0x00;                       // ON_H
        buf[2] = value & 0xFF;               // OFF_L
        buf[3] = (value >> 8) & 0x0F;        // OFF_H
    }
    // Return the I²C success flag. For OFF writes (value==0) the caller MUST
    // check this: a failed OFF leaves the coil energized, so the watchdog
    // slot must stay armed until a confirmed-successful OFF lands.
    return writeBurst(base, buf, 4);
}

bool PCA9685::allOff() {
    // ALL_LED_ON_L..ALL_LED_OFF_H broadcast to every channel in one shot.
    // Caller must check the result before treating the chip's outputs as
    // released (same confirmed-write invariant as setPWM OFF writes).
    uint8_t buf[4] = { 0x00, 0x00, 0x00, PCA9685Reg::FULL_OFF };
    return writeBurst(PCA9685Reg::ALL_LED_ON_L, buf, 4);
}

// Set PWM frequency in Hz. PCA9685 formula:
//   freq = 25 MHz / (4096 * (PRE_SCALE + 1))
// Range: PRE_SCALE 3..255 → 24..1526 Hz.
//
// 200 Hz (default at boot) is in the bass audible range — the solenoid
// coil mechanically vibrates at the PWM rate, producing a buzz. 1500 Hz
// pushes it to a high-pitched whine which is less obtrusive. To
// eliminate audible hum entirely, set PWM to FULL_ON (g_fullPowerMode).
//
// PRE_SCALE is writable only when SLEEP=1, so we briefly put the chip
// to sleep, write the new value, wake up, then issue RESTART so PWM
// resumes cleanly.
bool PCA9685::setPWMFrequency(uint16_t hz) {
    using namespace PCA9685Reg;
    if (hz < 24)   hz = 24;
    if (hz > 1526) hz = 1526;
    uint32_t ps = (25000000UL / (4096UL * hz)) - 1;
    if (ps < 3)   ps = 3;
    if (ps > 255) ps = 255;
    uint8_t prescale = (uint8_t)ps;

    // SLEEP=1, AI=1, ALLCALL=1 — required to write PRE_SCALE
    if (!writeReg(MODE1, MODE1_AI | MODE1_SLEEP | MODE1_ALLCALL)) return false;
    writeReg(PRE_SCALE, prescale);
    // Wake (clear SLEEP)
    writeReg(MODE1, MODE1_AI | MODE1_ALLCALL);
    delayMicroseconds(600);   // oscillator stabilization
    // RESTART bit kicks PWM counters back to a clean state at the new freq
    writeReg(MODE1, MODE1_AI | MODE1_ALLCALL | MODE1_RESTART);
    return true;
}
