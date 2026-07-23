// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// I²C bus scanner for the LilyGo T4-S3 piano controller.
//
// As of the 2026-05 opto-isolated board rev, the PCA9685 power boards are
// on a DEDICATED second I²C bus: Wire1, GPIO 42 (SDA) / 40 (SCL). The
// touch + PMU stay on the internal Wire bus (GPIO 6/7). This scanner talks
// to Wire1 so it sees the power boards directly.
//
// Use this to verify dipswitches on each PCA9685 power board are set to the
// addresses you expect (0x40..0x46). Plug one board in at a time, scan,
// flip switches, scan again — figure out which physical switch position
// is bit 0 vs bit 5 before installing them all.

#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <Wire.h>

// Must match the main firmware (config.h).
static const int PCA_SDA = 42;
static const int PCA_SCL = 40;
static const uint32_t PCA_FREQ = 400000;

LilyGo_Class amoled;

static const char *deviceHint(uint8_t a) {
    if (a >= 0x40 && a <= 0x46) return "  <- PCA9685 power board";
    if (a == 0x70)               return "  <- PCA9685 ALL-CALL (every chip responds)";
    if (a >= 0x68 && a <= 0x6F)  return "  (reserved I2C range)";
    if (a >= 0x78)               return "  (reserved I2C range)";
    return "";
}

static void doScan() {
    Serial.println("\n--- scan (Wire1 / power-board bus) ---");
    int found = 0;
    for (uint8_t a = 0; a < 128; a++) {
        Wire1.beginTransmission(a);
        if (Wire1.endTransmission() == 0) {
            const char *flag = "";
            if (a == 0x00) flag = " (general call)";
            else if (a >= 0x01 && a <= 0x07) flag = " (reserved low)";
            else if (a >= 0x78)              flag = " (reserved high)";
            Serial.printf("  0x%02X%s%s\n", a, deviceHint(a), flag);
            found++;
        }
    }
    Serial.printf("[scan] %d devices on Wire1\n", found);
}

// Broadcast "all off" to every PCA9685 via the ALL-CALL address (0x70).
static void silenceAllPCA9685() {
    Wire1.beginTransmission(0x70);
    Wire1.write(0x00);   // MODE1
    Wire1.write(0x21);   // SLEEP=0, AI=1, ALLCALL=1
    if (Wire1.endTransmission() != 0) {
        Serial.println("[scan] ALLCALL 0x70 no ACK — check Wire1 wiring / isolator power");
        return;
    }
    delay(1);
    Wire1.beginTransmission(0x70);
    Wire1.write(0xFA);   // ALL_LED_ON_L (AI walks to OFF_H)
    Wire1.write(0x00);
    Wire1.write(0x00);
    Wire1.write(0x00);
    Wire1.write(0x10);   // ALL_LED_OFF_H — FULL_OFF bit set
    Wire1.endTransmission();
    Serial.println("[scan] broadcast FULL_OFF to every PCA9685 (via 0x70)");
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n[scan] booting...");

    // Bring up the display bus (touch/PMU) so the board is happy, but we
    // scan Wire1 which we init ourselves on the power-board pins.
    amoled.begin();
    amoled.setBrightness(120);
    amoled.fillScreen(0x0000);

    Wire1.begin(PCA_SDA, PCA_SCL, PCA_FREQ);

    // Silence first, THEN scan. If any solenoid was stuck on, it releases here.
    silenceAllPCA9685();

    Serial.println("[scan] starting, will rescan every 3 seconds");
}

void loop() {
    doScan();
    delay(3000);
}
