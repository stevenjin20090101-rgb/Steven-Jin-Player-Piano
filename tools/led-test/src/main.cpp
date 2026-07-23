// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// GPIO41 SLOW TOGGLE TEST (engineer's request)
// ---------------------------------------------
// Forget WS2812 timing for a moment. This just drives GPIO41 HIGH for 2 s,
// then LOW for 2 s, forever. GPIO41 is the LED data pin that feeds the strip
// THROUGH the ISO7721 isolator.
//
// What to measure with a multimeter (DC volts), black probe on GND:
//
//   1) On GPIO41 itself (ESP side, BEFORE the isolator):
//        should read ~3.3 V during HIGH, ~0 V during LOW.
//        → confirms the ESP is actually driving the pin.
//
//   2) On the strip's DATA/DIN pad (AFTER the isolator):
//        HIGH phase should read ~5 V  → level config (R12/R15) is correct
//                                        for a 5 V strip. GOOD.
//        HIGH phase reads ~3.3 V      → isolator is outputting 3.3 V; a 5 V
//                                        WS2812 may not see it. Resistor
//                                        config (R13/R14 vs R12/R15) is wrong.
//        HIGH phase reads ~0 V (never rises) → signal is NOT crossing the
//                                        isolator: wrong resistor jumpers,
//                                        isolator not powered on the strip
//                                        side, or a broken trace.
//        Stuck HIGH (never drops to 0) → isolator inverts / idles high →
//                                        that's why WS2812 never latches.
//
// The onboard prints tell you which phase it's in so you can sync the meter.
// NOTE: this replaces the piano firmware. Re-flash the main project when done.

#include <Arduino.h>

static const int LED_PIN = 41;   // LED data → ISO7721 → strip DIN

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n[toggle-test] GPIO41 slow square wave: 2s HIGH / 2s LOW");
    Serial.println("[toggle-test] Meter GND->GPIO41 (ESP side) and GND->strip DIN (after isolator).");
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
}

void loop() {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[toggle-test] HIGH  (expect ~3.3V on GPIO41, ~5V on strip DIN)");
    delay(2000);

    digitalWrite(LED_PIN, LOW);
    Serial.println("[toggle-test] LOW   (expect ~0V both points)");
    delay(2000);
}
