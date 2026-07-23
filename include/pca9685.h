// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// PCA9685 driver — 16-channel 12-bit PWM expander (NXP). This is the chip
// 吟晚风 placed on the power boards (see ESP32S3_Schematic, U1).
//
// As of the 2026-05 opto-isolated board rev, the power boards live on a
// DEDICATED second I²C bus (Wire1, GPIO 42 SDA / 40 SCL) — separate from
// the touch/PMU bus (Wire, GPIO 6/7). We init Wire1 ourselves in setup().
// PCA_BUS below is the single knob for which bus this driver talks to.
//
// Differences from PCA9635 (which I previously coded against by mistake):
//   - 12-bit PWM (0..4095) instead of 8-bit
//   - Each channel has 4 registers: ON_L, ON_H, OFF_L, OFF_H
//   - LED base register is 0x06 (not 0x02)
//   - No LEDOUTx mode select — channels are always in PWM mode
//   - Has FULL_ON / FULL_OFF bits in ON_H[4] / OFF_H[4] for hard on/off
//   - PWM frequency is set via PRE_SCALE (default ≈ 200 Hz at int clock,
//     and PRE_SCALE can only be written when SLEEP=1)

#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"   // defines PCA_BUS (Wire or Wire1) from BOARD_REV

// Bus-wide mutex. Take this around EVERY Wire transaction in our code
// (PCA9685 writes, ALLCALL broadcasts, etc). Without it, the LVGL touch
// driver (running on the same core, polling 0x5A periodically) can
// preempt mid-transaction and corrupt the I²C bus state — observed as
// "screen dark + stuck note + watchdog can't recover" hangs under heavy
// BLE MIDI load with PWM mode.
//
// One-shot init from setup() via wire_lock_init(). All subsequent code
// uses WireGuard RAII wrapper:
//   { WireGuard g; Wire.beginTransmission(...); ... Wire.endTransmission(); }
extern SemaphoreHandle_t g_wireMutex;
void wire_lock_init();

// Total I²C writes that failed even after retry — an EMI-storm meter.
// Shown in "status" and the heartbeat log.
extern uint32_t g_i2cFailCount;

// Run the bus-recovery dance immediately, bypassing its 100 ms rate limit.
// Used by the safety escalation when even the ALLCALL broadcast fails —
// at that point freeing the bus outranks the anti-storm throttle.
void i2cForceRecovery();

struct WireGuard {
    WireGuard()  { if (g_wireMutex) xSemaphoreTake(g_wireMutex, portMAX_DELAY); }
    ~WireGuard() { if (g_wireMutex) xSemaphoreGive(g_wireMutex); }
};

class PCA9685 {
public:
    explicit PCA9685(uint8_t address) : _addr(address) {}

    bool begin();                                  // wake, configure, all OFF
    bool isConnected();                            // I²C ack test
    bool setPWM(uint8_t channel, uint16_t value);  // 0..4095, or 4096+ = full ON;
                                                   // returns false if the I²C
                                                   // write failed (CRITICAL for
                                                   // OFF writes — caller must
                                                   // not treat a failed OFF as
                                                   // "channel is now safe")
    bool allOff();                                 // FULL_OFF on every channel;
                                                   // false = write not confirmed
    bool setPWMFrequency(uint16_t hz);             // 24..1526 Hz
    uint8_t address() const { return _addr; }

private:
    uint8_t _addr;
    bool writeReg(uint8_t reg, uint8_t val);
    bool writeBurst(uint8_t reg, const uint8_t *data, uint8_t n);
};

namespace PCA9685Reg {
    constexpr uint8_t MODE1      = 0x00;
    constexpr uint8_t MODE2      = 0x01;
    constexpr uint8_t LED0_ON_L  = 0x06;   // base; channel N at 0x06 + 4*N
    constexpr uint8_t ALL_LED_ON_L = 0xFA; // writes to all channels at once
    constexpr uint8_t PRE_SCALE  = 0xFE;

    // MODE1 bits
    constexpr uint8_t MODE1_RESTART = 0x80;
    constexpr uint8_t MODE1_EXTCLK  = 0x40;
    constexpr uint8_t MODE1_AI      = 0x20;
    constexpr uint8_t MODE1_SLEEP   = 0x10;
    constexpr uint8_t MODE1_ALLCALL = 0x01;

    // OFF_H bit that forces output LOW regardless of count.
    constexpr uint8_t FULL_OFF      = 0x10;
}
