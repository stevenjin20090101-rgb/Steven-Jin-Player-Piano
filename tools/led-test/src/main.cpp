// Minimal WS2812B test. Drives the SAME color buffer out on BOTH GPIO 18
// and GPIO 41 simultaneously, so whichever pin your strip is physically
// wired to will light up — no need to know the pin in advance.
//
// Cycles: RED -> GREEN -> BLUE -> WHITE (dim) -> OFF, 1 second each,
// forever. If the strip does ANY of these, the hardware + wiring + that
// pin are all good, and the "dead LED" in the main firmware was just
// note-reactive mode showing black at idle.
//
// If NOTHING lights on either pin:
//   - wrong data pin (try changing PIN_A/PIN_B below)
//   - strip wired backwards (DIN vs DOUT — data arrow must point INTO strip)
//   - strip damaged (reverse-polarity from the earlier short can kill it)
//   - not enough 5V current for the strip

#include <Arduino.h>
#include <FastLED.h>

#define PIN_A       18     // original-board LED pin
#define PIN_B       41     // isolated-board LED pin
#define LED_COUNT   30     // set to your strip length (safe to over/under guess)
#define BRIGHTNESS  60     // keep modest so we don't pull huge current on white

CRGB leds[LED_COUNT];

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n[ledtest] driving WS2812B on GPIO 18 AND GPIO 41");
    Serial.println("[ledtest] whichever pin your strip is on will light up");

    // Register the same buffer on both pins. show() clocks both out.
    FastLED.addLeds<WS2812B, PIN_A, GRB>(leds, LED_COUNT);
    FastLED.addLeds<WS2812B, PIN_B, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(BRIGHTNESS);
}

void show(const char *name, CRGB color) {
    Serial.printf("[ledtest] %s\n", name);
    fill_solid(leds, LED_COUNT, color);
    FastLED.show();
    delay(1000);
}

void loop() {
    show("RED",        CRGB::Red);
    show("GREEN",      CRGB::Green);
    show("BLUE",       CRGB::Blue);
    show("WHITE(dim)", CRGB(60, 60, 60));
    show("OFF",        CRGB::Black);
}
