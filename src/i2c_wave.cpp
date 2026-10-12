// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// See include/i2c_wave.h. The wave comes from the LEDC peripheral (exact
// frequency, no CPU), on an OPEN-DRAIN pad: the pin only ever pulls the line
// low and lets go, so the high level and the rising edge are made by the
// line's own pull-up and isolator — the waveform the scope shows is the one a
// real I²C transaction would have, not a push-pull drive fighting the bus.
//
// Why the single-line modes are the safe ones: with SDA held high nothing on
// the bus can see a START, so clock pulses on SCL are ignored by every chip;
// with SCL held high, edges on SDA are only ever START and STOP conditions
// with no clock, so no address is ever formed. `both` toggles the two lines
// in phase; a chip would need a mistimed edge to read it as a START, and even
// then the test began with every coil verified off and ends with the all-off
// sequence — an energize could last seconds, not minutes.
//
// `bus` is different: real traffic. An address probe (START, the address, the
// chip's ACK or a NACK, STOP) goes to each power board in turn, hundreds of
// times a second, with the bus driver running as usual — so the scope sees
// the real 400 kHz clock on SCL and, on SDA, the address bits from this side
// and the ACK pulled low by the far side through the isolator. Probes only:
// nothing is ever written to a chip.
#include "i2c_wave.h"
#include "config.h"
#include "power_boards.h"

#include <Wire.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>

static const uint32_t kMinHz      = 10;
static const uint32_t kMaxHz      = 2000000;
static const uint32_t kMaxSeconds = 30;
static const uint8_t  kChanScl    = 6;    // LEDC channels nothing else in the firmware uses
static const uint8_t  kChanSda    = 7;

// A 50 % square wave at `hz` on `pin`, open-drain.
static bool startWave(uint8_t pin, uint8_t chan, uint32_t hz, Print &out) {
    // The widest duty resolution the 80 MHz LEDC clock allows at this frequency
    // (capped at 10 bits): 50 % is then exact to within one clock.
    uint8_t bits = 1;
    while (bits < 10 && (80000000UL >> (bits + 1)) >= hz) bits++;
    const double got = ledcSetup(chan, (double)hz, bits);
    if (got <= 0) {
        out.printf("  i2cwave: the LEDC peripheral refused %lu Hz for GPIO %u\n", (unsigned long)hz, pin);
        return false;
    }
    ledcAttachPin(pin, chan);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT_OD);   // after the attach: keep the routing, drop the push-pull
    ledcWrite(chan, 1u << (bits - 1));
    out.printf("  GPIO %u: %.0f Hz, open-drain (the pull-up makes the high level)\n", pin, got);
    return true;
}

static void stopWave(uint8_t pin, uint8_t chan) {
    ledcWrite(chan, 0);
    ledcDetachPin(pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);   // let go of the line; the pull-up has it
}

bool i2c_wave_run(const char *which, uint32_t hz, uint32_t seconds, Print &out) {
#if !PCA_USES_WIRE1
    (void)hz; (void)seconds;
    out.println("  i2cwave needs the isolated board (its own power-board bus); on this board the PCAs share the touch bus");
    return false;
#else
    const bool bus = !strcmp(which, "bus");
    const bool scl = !strcmp(which, "scl") || !strcmp(which, "both");
    const bool sda = !strcmp(which, "sda") || !strcmp(which, "both");
    if (!scl && !sda && !bus) {
        out.println("  usage: i2cwave scl|sda|both <hz 10..2000000> [seconds 1..30]  |  i2cwave bus [seconds 1..30]");
        return false;
    }
    if (!bus && (hz < kMinHz || hz > kMaxHz)) {
        out.println("  i2cwave: hz out of range (10..2000000)");
        return false;
    }
    if (seconds < 1) seconds = 1;
    if (seconds > kMaxSeconds) seconds = kMaxSeconds;

    // SAFETY, in this order: nothing may come on (inhibit), nothing may stay on
    // (release everything), and the model must confirm it before the bus goes
    // away — an offline board still believed on is a refusal, not a risk.
    set_energize_inhibit(true);
    allKeysOff();
    broadcastAllOff();
    if (!coilsVerifiedOff()) {
        set_energize_inhibit(false);
        out.println("  i2cwave refused: a coil is not verified off (a board off the bus still believed on?) — see status");
        return false;
    }
    if (bus) {
        // Real traffic, probes only (see the top of the file). The bus stays up.
        uint32_t acks[8] = {0};
        uint32_t rounds = 0;
        out.printf("  i2cwave: every coil verified off — probing %d power boards in turn for %lu s, scope SCL/SDA now\n",
                   kNumPowerBoards, (unsigned long)seconds);
        const uint32_t until = millis() + seconds * 1000UL;
        while ((int32_t)(millis() - until) < 0) {
            for (int i = 0; i < kNumPowerBoards && i < 8; i++) {
                if (powerBoards[i].chip.isConnected()) acks[i]++;
                delayMicroseconds(300);             // a gap the scope can trigger on
            }
            rounds++;
            if ((rounds & 31) == 0) esp_task_wdt_reset();
        }
        out.printf("  %lu rounds — ACKs:", (unsigned long)rounds);
        for (int i = 0; i < kNumPowerBoards && i < 8; i++) {
            out.printf(" 0x%02X=%lu", powerBoards[i].chip.address(), (unsigned long)acks[i]);
        }
        out.println("\n  (a board that never ACKs is not reachable through the isolator and trunk)");
        allKeysOff();
        set_energize_inhibit(false);
        return true;
    }

    out.printf("  i2cwave: every coil verified off — taking the power-board bus down for %lu s\n",
               (unsigned long)seconds);
    PCA_BUS.end();

    bool ok = true;
    if (scl) ok = startWave(I2C_SCL_PIN, kChanScl, hz, out) && ok;
    if (sda) ok = startWave(I2C_SDA_PIN, kChanSda, hz, out) && ok;
    if (ok) {
        out.printf("  square wave on %s at %lu Hz for %lu s — scope the trunk now\n",
                   (scl && sda) ? "SCL and SDA" : (scl ? "SCL (SDA held high)" : "SDA (SCL held high)"),
                   (unsigned long)hz, (unsigned long)seconds);
        for (uint32_t t = 0; t < seconds * 10; t++) {   // loop() is held the whole time: feed the watchdog
            delay(100);
            esp_task_wdt_reset();
        }
    }
    if (scl) stopWave(I2C_SCL_PIN, kChanScl);
    if (sda) stopWave(I2C_SDA_PIN, kChanSda);

    // The bus back exactly as setup() brings it up, then the boot's own safety
    // sequence: three ALLCALL all-offs, every board re-probed and configured,
    // every channel released through the verified-OFF path.
    PCA_BUS.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
    PCA_BUS.setTimeOut(5);
    for (int i = 0; i < 3; ++i) { broadcastAllOff(); delay(50); }
    initPowerBoards();
    allKeysOff();
    set_energize_inhibit(false);
    out.println(ok ? "  i2cwave done: bus restored, boards re-probed, everything off"
                   : "  i2cwave aborted: bus restored, boards re-probed, everything off");
    return ok;
#endif
}
