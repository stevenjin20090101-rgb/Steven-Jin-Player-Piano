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
// sequence.
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
#include <driver/gpio.h>

static const uint32_t kMinHz        = 10;
static const uint32_t kMaxHz        = 2000000;
static const uint32_t kMaxSeconds   = 600;     // "until stopped" still ends after 10 min
static const uint32_t kHeartbeatMs  = 10000;
static const uint8_t  kChanScl      = 6;       // LEDC channels nothing else in the firmware uses
static const uint8_t  kChanSda      = 7;

enum WaveMode : uint8_t { WAVE_NONE, WAVE_SCL, WAVE_SDA, WAVE_BOTH, WAVE_BUS };

static WaveMode s_mode        = WAVE_NONE;
static uint32_t s_hz          = 0;
static uint32_t s_startMs     = 0;
static uint32_t s_untilMs     = 0;     // 0 = no time limit beyond the cap
static uint32_t s_heartbeatMs = 0;
static uint32_t s_rounds      = 0;     // bus mode: probe rounds done
static uint32_t s_acks[8]     = {0};

static const char *modeName(WaveMode m) {
    switch (m) {
        case WAVE_SCL:  return "SCL";
        case WAVE_SDA:  return "SDA";
        case WAVE_BOTH: return "SCL+SDA";
        case WAVE_BUS:  return "bus probes";
        default:        return "none";
    }
}

// A 50 % square wave at `hz` on `pin`, open-drain.
static bool startWave(uint8_t pin, uint8_t chan, uint32_t hz, Print &out) {
    // The widest duty resolution the LEDC clock allows at this frequency, so
    // 50 % is exact to within one clock. On the S3 the core runs LEDC from the
    // 40 MHz crystal (esp32-hal-ledc.c: LEDC_USE_XTAL_CLK), and the divider
    // must stay >= 1: hz x 2^bits <= 40 MHz. Capped at 10 bits; if the
    // peripheral still refuses, fewer bits are tried before giving up.
    uint8_t bits = 1;
    while (bits < 10 && (40000000UL >> (bits + 1)) >= hz) bits++;
    double got = 0;
    for (; bits >= 1; bits--) {
        got = ledcSetup(chan, (double)hz, bits);
        if (got > 0) break;
    }
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

bool i2c_wave_active() { return s_mode != WAVE_NONE; }

bool i2c_wave_start(const char *which, uint32_t hz, uint32_t seconds, Print &out) {
#if !PCA_USES_WIRE1
    (void)hz; (void)seconds; (void)which;
    out.println("  i2cwave needs the isolated board (its own power-board bus); on this board the PCAs share the touch bus");
    return false;
#else
    if (s_mode != WAVE_NONE) {
        out.printf("  i2cwave: already running (%s) — `i2cwave stop` first\n", modeName(s_mode));
        return false;
    }
    WaveMode mode = WAVE_NONE;
    if      (!strcmp(which, "scl"))  mode = WAVE_SCL;
    else if (!strcmp(which, "sda"))  mode = WAVE_SDA;
    else if (!strcmp(which, "both")) mode = WAVE_BOTH;
    else if (!strcmp(which, "bus"))  mode = WAVE_BUS;
    if (mode == WAVE_NONE) {
        out.println("  usage: i2cwave scl|sda|both <hz 10..2000000> [seconds]  |  i2cwave bus [seconds]  |  i2cwave stop   (no seconds = until stopped)");
        return false;
    }
    if (mode != WAVE_BUS && (hz < kMinHz || hz > kMaxHz)) {
        out.println("  i2cwave: hz out of range (10..2000000)");
        return false;
    }
    if (seconds == 0 || seconds > kMaxSeconds) seconds = kMaxSeconds;

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

    const bool scl = (mode == WAVE_SCL || mode == WAVE_BOTH);
    const bool sda = (mode == WAVE_SDA || mode == WAVE_BOTH);
    if (mode == WAVE_BUS) {
        memset(s_acks, 0, sizeof s_acks);
        s_rounds = 0;
        out.printf("  i2cwave ON: every coil verified off — probing %d power boards in turn (clock on SCL, address + ACK on SDA)\n",
                   kNumPowerBoards);
    } else {
        out.println("  i2cwave: every coil verified off — taking the power-board bus down");
        PCA_BUS.end();
        bool ok = true;
        if (scl) ok = startWave(I2C_SCL_PIN, kChanScl, hz, out) && ok;
        if (sda) ok = startWave(I2C_SDA_PIN, kChanSda, hz, out) && ok;
        if (!ok) {
            s_mode = mode;                         // so stop() tears down what did start
            s_hz   = hz;
            i2c_wave_stop(out);
            return false;
        }
        out.printf("  i2cwave ON: square wave on %s at %lu Hz\n",
                   (scl && sda) ? "SCL and SDA" : (scl ? "SCL (SDA held high)" : "SDA (SCL held high)"),
                   (unsigned long)hz);
    }
    s_mode        = mode;
    s_hz          = hz;
    s_startMs     = millis();
    s_untilMs     = s_startMs + seconds * 1000UL;
    s_heartbeatMs = s_startMs;
    out.printf("  (ends after %lu s, or at `i2cwave stop`; the piano's screen says while it is on)\n",
               (unsigned long)seconds);
    return true;
#endif
}

void i2c_wave_stop(Print &out) {
#if PCA_USES_WIRE1
    if (s_mode == WAVE_NONE) return;
    const WaveMode mode = s_mode;
    const uint32_t ran  = (millis() - s_startMs) / 1000UL;
    s_mode = WAVE_NONE;
    if (mode == WAVE_BUS) {
        out.printf("  i2cwave OFF after %lu s: %lu rounds — ACKs:", (unsigned long)ran, (unsigned long)s_rounds);
        for (int i = 0; i < kNumPowerBoards && i < 8; i++) {
            out.printf(" 0x%02X=%lu", powerBoards[i].chip.address(), (unsigned long)s_acks[i]);
        }
        out.println("\n  (a board that never ACKs is not reachable through the isolator and trunk)");
    } else {
        if (mode == WAVE_SCL || mode == WAVE_BOTH) stopWave(I2C_SCL_PIN, kChanScl);
        if (mode == WAVE_SDA || mode == WAVE_BOTH) stopWave(I2C_SDA_PIN, kChanSda);
        // The bus back exactly as setup() brings it up, then the boot's own
        // safety sequence: three ALLCALL all-offs, every board re-probed and
        // configured, every channel released through the verified-OFF path.
        PCA_BUS.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
        PCA_BUS.setTimeOut(5);
        for (int i = 0; i < 3; ++i) { broadcastAllOff(); delay(50); }
        initPowerBoards();
        out.printf("  i2cwave OFF after %lu s: bus restored, boards re-probed\n", (unsigned long)ran);
    }
    allKeysOff();
    set_energize_inhibit(false);
    out.println("  everything off; the piano plays again");
#else
    (void)out;
#endif
}

void i2c_wave_tick() {
#if PCA_USES_WIRE1
    if (s_mode == WAVE_NONE) return;
    const uint32_t now = millis();
    if (s_mode == WAVE_BUS) {
        // One round of probes a pass (about 2.5 ms for seven boards): hundreds
        // of real transactions a second with a gap the scope can trigger on.
        for (int i = 0; i < kNumPowerBoards && i < 8; i++) {
            if (powerBoards[i].chip.isConnected()) s_acks[i]++;
            delayMicroseconds(300);
        }
        s_rounds++;
    }
    if ((int32_t)(now - s_heartbeatMs) >= (int32_t)kHeartbeatMs) {
        s_heartbeatMs = now;
        Serial.printf("[i2cwave] %s on for %lu s — `i2cwave stop` ends it\n",
                      modeName(s_mode), (unsigned long)((now - s_startMs) / 1000UL));
    }
    if ((int32_t)(now - s_untilMs) >= 0) {
        Serial.println("[i2cwave] time limit reached");
        i2c_wave_stop(Serial);
    }
#endif
}

bool i2c_wave_status_text(char *buf, size_t len) {
    if (s_mode == WAVE_NONE || buf == nullptr || len == 0) return false;
    const uint32_t secs = (millis() - s_startMs) / 1000UL;
    if (s_mode == WAVE_BUS) {
        snprintf(buf, len, "Signal test: bus probes, %lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    } else if (s_hz >= 1000) {
        snprintf(buf, len, "Signal test: %s %lu kHz, %lu:%02lu", modeName(s_mode), (unsigned long)(s_hz / 1000),
                 (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    } else {
        snprintf(buf, len, "Signal test: %s %lu Hz, %lu:%02lu", modeName(s_mode), (unsigned long)s_hz,
                 (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    }
    return true;
}

void i2c_wave_print_status(Print &out) {
    if (s_mode == WAVE_NONE) {
        out.println("  signal test: off");
        return;
    }
    const uint32_t secs = (millis() - s_startMs) / 1000UL;
    if (s_mode == WAVE_BUS) {
        out.printf("  signal test: ON — bus probes for %lu s, %lu rounds (`i2cwave stop` ends it)\n",
                   (unsigned long)secs, (unsigned long)s_rounds);
    } else {
        out.printf("  signal test: ON — %s square wave at %lu Hz for %lu s, bus down (`i2cwave stop` ends it)\n",
                   modeName(s_mode), (unsigned long)s_hz, (unsigned long)secs);
    }
}
