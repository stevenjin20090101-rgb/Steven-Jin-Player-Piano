// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// LilyGo T4-S3 piano controller firmware
//
// Acts as a BLE MIDI peripheral so apps like Synthesia (iPad) can stream
// MIDI directly to the board. Incoming notes drive a WS2812B strip on
// LED_DATA_PIN and surface on the AMOLED touch UI.
//
// Next milestone (not in this build): forward NoteOn/NoteOff over I2C to
// PCA9685 expanders on each power board to fire solenoids.

#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <LV_Helper.h>
#include <FastLED.h>

#include <BLEMIDI_Transport.h>
#include <NimBLEDevice.h>     // for explicit advertising restart on disconnect
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

#include "config.h"
#include "ui.h"
#include "power_boards.h"
#include "settings.h"
#include "pedal.h"

// LVGL is not thread-safe. We render in a dedicated task on core 0 (away from
// Arduino's loopTask on core 1) and protect every API call with this mutex.
// Without this split, lv_timer_handler() blocks the loop for tens of ms per
// frame and MIDI events pile up faster than they drain → 1 fps + dropped
// notes during dense passages.
static SemaphoreHandle_t s_ui_mutex = nullptr;
static TaskHandle_t      s_lvgl_task = nullptr;

static inline bool ui_lock(uint32_t timeout_ms = portMAX_DELAY) {
    return s_ui_mutex && xSemaphoreTake(s_ui_mutex,
        timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static inline void ui_unlock() {
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

static void lvgl_render_task(void *) {
    while (true) {
        if (ui_lock(50)) {
            lv_timer_handler();
            ui_unlock();
        }
        // 5ms cadence — LVGL only actually flushes every LV_DISP_DEF_REFR_PERIOD
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

LilyGo_Class amoled;
BLEMIDI_CREATE_INSTANCE(BLE_MIDI_DEVICE_NAME, MIDI)

CRGB leds[LED_COUNT];

// --- Authorship watermark -------------------------------------------------
// Compiled into the firmware image (kept by __attribute__((used)) so the
// optimizer can't drop it), so even a stolen, header-stripped BINARY still
// carries its origin.  Reveal with:  strings firmware.bin | grep PPFW
// The cryptographic proof (Ed25519 signature) is in PROVENANCE.md — this
// fingerprint ties this binary to that signature.
__attribute__((used)) static const volatile char kBuildProvenance[] =
    "PPFW-PROVENANCE::author=Steven Jin::"
    "ed25519fp=eab16a502f679465::"
    "id=stevenjin20090101@gmail.com::year=2026::"
    "proof=see-PROVENANCE.md";

AppState appState = {
    .bleConnected = false,
    .peerName = "",
    .notesReceived = 0,
    .lastNote = 0,
    .lastVelocity = 0,
    .ledMode = LED_MODE_NOTE_REACTIVE,
    .ledBrightness = LED_DEFAULT_BRIGHTNESS,
    .screenBrightness = 180,
    .ledStaticColor = 0x0040FF,
    .rainbowSpeed = 1,
    .noteDecayRate = 6,
    .ledCount = DEFAULT_LED_ACTIVE,
    .ledOffset = DEFAULT_LED_OFFSET,
    .ledScalePct = DEFAULT_LED_SCALE_PCT,
    .ledTail = DEFAULT_LED_TAIL,
    .ledReverse = (DEFAULT_LED_REVERSE != 0),
    .ledReactivePalette = DEFAULT_REACT_PALETTE,
    .ledGlow = DEFAULT_LED_GLOW,
    .ledVelBright = (DEFAULT_LED_VELBRIGHT != 0),
    .inputMode = INPUT_MODE_BLE_ONLY,
    .touchVelocity = 100,
};

// Idle-dim seconds (0 = never dim). Persisted; tunable via "dimsecs".
uint32_t g_idleDimSecs = DEFAULT_IDLE_DIM_SECS;

void apply_screen_brightness(uint8_t value) {
    // Only update the TARGET. The idle dimmer in loop() is the single owner
    // of the physical panel level — it ramps smoothly to this within ~0.5 s.
    // (Two writers used to fight: slider set the panel directly while the
    // dimmer held a stale level, causing a visible flicker on wake.)
    appState.screenBrightness = value;
}

static const char *noteNameC(uint8_t note) {
    static const char *names[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    snprintf(buf, sizeof(buf), "%s%d", names[note % 12], (note / 12) - 1);
    return buf;
}

// per-LED decay buffers used by note-reactive mode. noteHeat is the fading
// brightness envelope; noteColor is the base colour picked by the palette
// when the note fired (so any palette works, not just hue-by-pitch).
static uint8_t noteHeat[LED_COUNT]  = {0};
static CRGB    noteColor[LED_COUNT];
// Per-LED count of currently-held notes. While > 0 the LED stays lit at full
// (sustain) instead of decaying; it only fades once the key(s) are released.
// A counter (not a bool) because several notes can map to the same LED.
static uint8_t ledHeld[LED_COUNT]   = {0};

static void clearLedReactive() {
    memset(noteHeat, 0, sizeof(noteHeat));
    memset(ledHeld,  0, sizeof(ledHeld));
}
// Set true when a note lights an LED, so the loop pushes a frame within ~12 ms
// instead of waiting for the next 30 fps tick — makes reactions feel instant.
static volatile bool s_ledReactiveDirty = false;

// Calibration marker: 'ledtest <note>' lights exactly the LED that note maps
// to (bright white) for a few seconds, overriding the current mode, so you can
// see whether that key's LED lines up and trim scale/offset until it does.
static int      s_ledTestIdx     = -1;
static uint32_t s_ledTestUntilMs = 0;

// Number of physical LEDs actually driven (clamped to the array size).
static inline int ledN() {
    int n = appState.ledCount;
    if (n < 1) n = 1;
    if (n > LED_COUNT) n = LED_COUNT;
    return n;
}

// Number of LEDs the playable KEYS map onto = physical count minus the tail
// LEDs that sit past the last solenoid key. Notes only ever light [0..this-1];
// the tail LEDs [this..ledN()-1] are driven but held dark.
static inline int ledUsable() {
    int n = ledN() - (int)appState.ledTail;
    if (n < 1) n = 1;
    return n;
}

// Map a MIDI note to a physical LED index, honouring the runtime strip length,
// SCALE (gain, corrects drift when there are fewer LEDs than keys), direction,
// and OFFSET (fine per-light shift) so the lit LED lines up under the key.
// Anchored at the low end: Offset pins the low key, Scale then stretches the
// spread so the high key lands right — classic two-point calibration.
static int noteToLed(uint8_t note) {
    if (note < MIDI_NOTE_MIN) note = MIDI_NOTE_MIN;
    if (note > MIDI_NOTE_MAX) note = MIDI_NOTE_MAX;
    int n = ledUsable();                           // map only over keyed LEDs
    int span = MIDI_NOTE_MAX - MIDI_NOTE_MIN;      // key range width
    if (span < 1) span = 1;
    // fraction along the keyboard (0..1) × scale gain, spread over the strip
    long scaled = (long)(note - MIDI_NOTE_MIN) * (n - 1) * (long)appState.ledScalePct;
    int idx = (int)(scaled / (100L * span));
    if (appState.ledReverse) idx = (n - 1) - idx;
    idx += appState.ledOffset;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;                      // never enters the dark tail
    return idx;
}

// Pick the base colour for a note in note-reactive mode, per the selected
// palette. Pitch-indexed palettes spread colour across the keyboard; the
// velocity palette colours by how hard the key was hit.
static CRGB reactiveColorFor(uint8_t note, uint8_t vel) {
    uint8_t pitchIdx = map(note, MIDI_NOTE_MIN, MIDI_NOTE_MAX, 0, 255);
    switch (appState.ledReactivePalette) {
        case 1: return CRGB(appState.ledStaticColor);                       // solid
        case 2: return CHSV(map(vel, 1, 127, 160, 0), 255, 255);            // velocity: soft blue → hard red
        case 3: return ColorFromPalette(HeatColors_p,   pitchIdx);          // fire
        case 4: return ColorFromPalette(OceanColors_p,  pitchIdx);          // ocean
        case 5: return ColorFromPalette(ForestColors_p, pitchIdx);          // forest
        case 6: return ColorFromPalette(LavaColors_p,   pitchIdx);          // lava
        case 7: return ColorFromPalette(PartyColors_p,  pitchIdx);          // party
        case 0:
        default: return CHSV(map(note, MIDI_NOTE_MIN, MIDI_NOTE_MAX, 0, 240), 255, 255); // pitch rainbow
    }
}

// One-shot bus walk: probe every 7-bit address and log who answers. Useful
// for confirming all 8 PCA9685 boards plus touch + PMU showed up before we
// start dispatching notes.
// Scan the PCA9685 power-board bus (PCA_BUS — Wire on the original board,
// Wire1 on the isolated board). Surfaces the power boards + any bus
// neighbors so you can confirm dipswitch addresses and spot missing boards.
static void scanI2CBus() {
    Serial.println("[i2c] scanning power-board bus (0x03..0x7F)...");
    uint8_t detected[32];
    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x7F; ++addr) {
        PCA_BUS.beginTransmission(addr);
        uint8_t err = PCA_BUS.endTransmission();
        if (err == 0) {
            // Print ONLY hits. (The old per-address "probe 0xNN => --" dump
            // was 125 blocking serial lines ≈ ¼ s of boot time for noise.)
            const char *name = "";
            if (addr == 0x70) name = "  (PCA9685 All-Call - any PCA9685 will ACK)";
            else if (addr >= 0x40 && addr <= 0x46) name = "  (power board)";
            else if (addr >= 0x40 && addr <= 0x6F) name = "  (possible PCA9685 individual)";
            Serial.printf("[i2c]   0x%02X ACK%s\n", addr, name);
            if (found < (int)(sizeof(detected) / sizeof(detected[0]))) {
                detected[found] = addr;
            }
            ++found;
        }
    }
    Serial.printf("[i2c] scan complete — %d device(s) on power bus\n", found);
    Serial.print  ("[i2c] addresses: ");
    int max_listed = found;
    if (max_listed > (int)(sizeof(detected) / sizeof(detected[0]))) {
        max_listed = sizeof(detected) / sizeof(detected[0]);
    }
    if (found == 0) {
        Serial.println("(none — check BOARD_REV matches your hardware + power)");
    } else {
        for (int i = 0; i < max_listed; ++i) {
            Serial.printf("0x%02X%s", detected[i], i + 1 < max_listed ? ", " : "\n");
        }
    }
    // Mirror onto the AMOLED so you can read it without a serial monitor.
    ui_set_i2c_info(max_listed, detected);
}

// Single-producer / single-consumer ring buffer. Producer: BLE-MIDI callbacks
// (which may fire from the NimBLE task). Consumer: loop(). Keeps LVGL access
// confined to the main thread.
struct NoteEvent { uint8_t on; uint8_t note; uint8_t velocity; };
static constexpr int EVENT_Q_SIZE = 128;
static volatile NoteEvent eventQ[EVENT_Q_SIZE];
static volatile uint8_t eventHead = 0;
static volatile uint8_t eventTail = 0;

// The queue has TWO producers on TWO cores — BLE MIDI callbacks (NimBLE,
// core 0) and on-screen touch keys (LVGL task, core 1) — plus one consumer
// (loop, core 1). A plain lock-free SPSC ring is unsafe with two producers
// racing on eventHead: a lost NoteOff would strand a note. This cross-core
// spinlock serializes every head/tail mutation. Sections are tiny (a few
// byte writes), never held across I²C, so the overhead is negligible.
static portMUX_TYPE s_qMux = portMUX_INITIALIZER_UNLOCKED;

static void enqueueEvent(uint8_t on, uint8_t note, uint8_t velocity) {
    portENTER_CRITICAL(&s_qMux);
    uint8_t next = (eventHead + 1) % EVENT_Q_SIZE;
    if (next != eventTail) {           // drop if full
        eventQ[eventHead].on = on;
        eventQ[eventHead].note = note;
        eventQ[eventHead].velocity = velocity;
        eventHead = next;
    }
    portEXIT_CRITICAL(&s_qMux);
}

// Pop one event under the lock; dispatch happens OUTSIDE the lock (I²C is
// slow). Returns false if the queue was empty.
static bool dequeueEvent(NoteEvent &out) {
    bool got = false;
    portENTER_CRITICAL(&s_qMux);
    if (eventTail != eventHead) {
        out.on       = eventQ[eventTail].on;
        out.note     = eventQ[eventTail].note;
        out.velocity = eventQ[eventTail].velocity;
        eventTail = (eventTail + 1) % EVENT_Q_SIZE;
        got = true;
    }
    portEXIT_CRITICAL(&s_qMux);
    return got;
}

// Discard all queued events atomically (used by the disconnect stop).
static void flushEventQueue() {
    portENTER_CRITICAL(&s_qMux);
    eventTail = eventHead;
    portEXIT_CRITICAL(&s_qMux);
}

// Public entry point — both BLE callbacks (NimBLE task) and on-screen
// piano key taps (LVGL task) funnel through here so there's exactly one
// I²C dispatch path. Filters by appState.inputMode so the user's mode
// dropdown actually gates which sources play.
void note_input_enqueue(uint8_t on, uint8_t note, uint8_t velocity, bool from_ble) {
    if (from_ble && appState.inputMode == INPUT_MODE_TOUCH_ONLY) return;
    if (!from_ble && appState.inputMode == INPUT_MODE_BLE_ONLY) return;
    enqueueEvent(on, note, velocity);
}

// Logging in the MIDI hot path blocks on USB CDC. Under Rush E that meant
// hundreds of synchronous prints per second, starving everything else. Off
// by default; flip this to 1 in a debug build if you need to see events.
#define DEBUG_MIDI_PRINT 0

static void handleNoteOn(byte channel, byte note, byte velocity) {
    if (velocity == 0) {
#if DEBUG_MIDI_PRINT
        Serial.printf("[midi] NoteOff ch=%u note=%u (%s)\n",
                      channel, note, noteNameC(note));
#endif
        note_input_enqueue(0, note, 0, /*from_ble=*/true);
        return;
    }
#if DEBUG_MIDI_PRINT
    Serial.printf("[midi] NoteOn  ch=%u note=%u (%s) vel=%u\n",
                  channel, note, noteNameC(note), velocity);
#endif
    note_input_enqueue(1, note, velocity, /*from_ble=*/true);
}

static void handleNoteOff(byte channel, byte note, byte velocity) {
#if DEBUG_MIDI_PRINT
    Serial.printf("[midi] NoteOff ch=%u note=%u (%s)\n",
                  channel, note, noteNameC(note));
#endif
    note_input_enqueue(0, note, 0, /*from_ble=*/true);
}

// Set by handleControlChange (NimBLE core 0) when the app sends a MIDI
// panic; serviced by loop() on core 1 (same reason as the disconnect stop —
// keep the release sequential with dispatch, off the BLE callback).
static volatile bool g_requestAllOff = false;

// --- Sustain-pedal (CC64) state -------------------------------------------
// Written by handleControlChange on the NimBLE task (core 0), read by loop()
// on core 1. Plain volatiles: each is a single byte/bool, and nothing here
// drives hardware — a torn read at worst logs one stale value.
// g_pedalSeen answers the practical question "does this song/app actually
// send pedal data?" without needing any pedal hardware attached.
static volatile uint8_t g_sustainRaw      = 0;      // last CC64 value, 0..127
static volatile bool    g_sustainDown     = false;  // >=64 = pedal down
static volatile bool    g_pedalEventDirty = false;  // a transition to log
static volatile bool    g_pedalSeen       = false;  // any pedal CC ever seen
static volatile uint32_t g_pedalCcCount   = 0;      // total pedal CCs received

static void handleControlChange(byte channel, byte cc, byte value) {
#if DEBUG_MIDI_PRINT
    Serial.printf("[midi] CC      ch=%u cc=%u val=%u\n", channel, cc, value);
#endif
    // Honor the standard "stop everything" control changes an app/DAW sends
    // when you hit stop/panic: CC120 All-Sound-Off, CC123 All-Notes-Off,
    // CC121 Reset-All-Controllers. Release immediately instead of waiting
    // for the g_maxHoldMs watchdog on every held note.
    if (cc == 120 || cc == 121 || cc == 123) {
        g_requestAllOff = true;
    }

    // --- Piano pedals ------------------------------------------------------
    // CC64 sustain/damper (the "everything rings and blends" pedal), CC66
    // sostenuto, CC67 una corda/soft. Standard MIDI: <64 = up, >=64 = down;
    // the raw 0..127 value also carries HALF-pedalling for actuators that can
    // hold an intermediate position.
    // Captured here on the NimBLE task (core 0) as plain volatiles; loop()
    // on core 1 consumes them. No actuator is driven from this callback.
    if (cc == 64 || cc == 66 || cc == 67) {
        if (cc == 64) {
            g_sustainRaw = value;
            bool down = (value >= 64);
            if (down != g_sustainDown) {
                g_sustainDown = down;
                g_pedalEventDirty = true;   // loop() logs the transition
            }
        }
        g_pedalSeen = true;                 // proves this source sends pedal data
        g_pedalCcCount++;
    }
}

static void handleProgramChange(byte channel, byte program) {
#if DEBUG_MIDI_PRINT
    Serial.printf("[midi] PC      ch=%u prog=%u\n", channel, program);
#endif
}

static void handlePitchBend(byte channel, int bend) {
#if DEBUG_MIDI_PRINT
    Serial.printf("[midi] Bend    ch=%u value=%d\n", channel, bend);
#endif
}

static void handleClock() {
    // Intentionally empty — discard incoming MIDI clock entirely. Some apps
    // pump these every ~21 ms; counting / logging them is pure overhead.
}

bool g_ledsEnabled = (DEFAULT_LEDS_ENABLED != 0);

static void updateLeds() {
    // Master kill-switch: when off, do NO FastLED work at all. This removes
    // the ~9 ms/frame WS2812 output block that competes with note dispatch
    // and the BLE stack during dense passages — the top suspect for
    // "crashes on complex songs".
    if (!g_ledsEnabled) return;

    // Calibration override: while a 'ledtest' marker is active, show ONLY that
    // one LED bright white so you can line it up under the played key.
    if (s_ledTestIdx >= 0 && (int32_t)(s_ledTestUntilMs - millis()) > 0) {
        fill_solid(leds, LED_COUNT, CRGB::Black);
        if (s_ledTestIdx < ledN()) leds[s_ledTestIdx] = CRGB::White;
        FastLED.setBrightness(appState.ledBrightness);
        FastLED.show();
        return;
    }
    if (s_ledTestIdx >= 0) { s_ledTestIdx = -1; clearLedReactive(); }

    // Skip redundant shows: at 300 LEDs each FastLED.show() is ~9 ms of
    // core-1 time. OFF and idle NOTE_REACTIVE frames are identical black
    // frames — pushing them every 33 ms is pure waste, and during heavy
    // MIDI it competes with note dispatch. One trailing frame is always
    // sent after content ends so the strip actually goes dark.
    static bool s_prevHadContent = true;   // true → first frame always shows
    bool hasContent = true;

    // Mode switch: wipe the note-reactive decay buffer so ghosts of the old
    // mode's heat can't reappear when switching back into NOTE_REACTIVE.
    static LedMode s_prevMode = LED_MODE_COUNT;
    bool modeChanged = (appState.ledMode != s_prevMode);
    if (modeChanged) {
        s_prevMode = appState.ledMode;
        clearLedReactive();                // wipe heat + held state
        s_prevHadContent = true;           // force one frame in the new mode
    }

    switch (appState.ledMode) {
        case LED_MODE_OFF:
            fill_solid(leds, LED_COUNT, CRGB::Black);
            hasContent = false;
            break;
        case LED_MODE_STATIC: {
            // A solid color never changes, so we only need to push it to the
            // strip when the color or brightness actually changes — then it
            // just sits there lit at ZERO ongoing cost (no per-frame 9 ms
            // FastLED.show()). This is why "solid color" is essentially free
            // vs. reactive, which must re-send every frame.
            static uint32_t s_lastStatic = 0xFFFFFFFF;
            static uint8_t  s_lastStaticBri = 255;
            if (!modeChanged &&
                appState.ledStaticColor == s_lastStatic &&
                appState.ledBrightness == s_lastStaticBri) {
                return;                       // unchanged → nothing to do
            }
            s_lastStatic = appState.ledStaticColor;
            s_lastStaticBri = appState.ledBrightness;
            fill_solid(leds, LED_COUNT, CRGB::Black);
            fill_solid(leds, ledUsable(), CRGB(appState.ledStaticColor));
            FastLED.setBrightness(appState.ledBrightness);
            FastLED.show();
            return;                           // shown once; skip the tail show
        }
        case LED_MODE_RAINBOW: {
            static uint8_t hue = 0;
            // Per-LED hue math instead of fill_rainbow's uint8 deltaHue:
            // 256 / count truncates to 0 on long strips, which rendered the
            // whole strip a single solid color. This spreads one full wheel
            // across exactly the keyed strip length (tail LEDs stay dark).
            int n = ledUsable();
            for (int i = 0; i < LED_COUNT; ++i) {
                leds[i] = (i < n) ? (CRGB)CHSV(hue + (uint8_t)((i * 256) / n), 255, 255)
                                  : CRGB::Black;
            }
            hue += appState.rainbowSpeed;
            break;
        }
        case LED_MODE_NOTE_REACTIVE: {
            uint8_t decay = appState.noteDecayRate;
            if (decay < 1) decay = 1;
            hasContent = false;
            int n = ledUsable();            // tail LEDs stay dark (no solenoid there)
            for (int i = 0; i < LED_COUNT; ++i) {
                if (i < n && ledHeld[i] > 0) {
                    // Key still held → keep the LED lit, DON'T fade (sustain).
                    if (noteHeat[i] == 0) noteHeat[i] = 255;   // ensure it's visible
                    hasContent = true;
                    CRGB c = noteColor[i];
                    c.nscale8(noteHeat[i]);
                    leds[i] = c;
                } else if (i < n && noteHeat[i] > 0) {
                    // Released → fade out from wherever it was.
                    hasContent = true;
                    CRGB c = noteColor[i];
                    c.nscale8(noteHeat[i]);
                    leds[i] = c;
                    noteHeat[i] = (noteHeat[i] > decay) ? noteHeat[i] - decay : 0;
                } else {
                    leds[i] = CRGB::Black;
                }
            }
            break;
        }
        default:
            break;
    }

    if (!hasContent && !s_prevHadContent) return;   // steady black — skip
    s_prevHadContent = hasContent;

    FastLED.setBrightness(appState.ledBrightness);
    FastLED.show();
}

// Set from BLE callbacks (NimBLE task); consumed by loop() so LVGL is only
// touched from the main thread.
static volatile bool g_status_dirty = false;

// Last "someone is using the piano" timestamp for the idle dimmer. Updated
// by BOTH note paths: queued events (BLE/touch) in loop(), and serial
// console lines (GUI MIDI player streams `note` commands over serial — a
// serial-driven song must keep the screen awake too).
static uint32_t s_lastNoteActivityMs = 0;

// ---- Serial console -------------------------------------------------------
//
// A tiny non-blocking line reader. Lets you tune PWM and fire test notes
// without re-flashing. Type commands in the Serial Monitor (no autocomplete,
// just commands + Enter):
//
//   help                     show this list
//   status                   show connected boards + current PWM settings
//   off                      panic — release every solenoid
//   sweep                    re-run the test sweep with current settings
//   pwm <0..4095>            set sweep strike PWM, run a one-shot sweep
//   min <0..4095>            set MIN_STRIKE_PWM (velocity=1 maps here)
//   max <0..4095>            set MAX_STRIKE_PWM (velocity=127 maps here)
//   fire <board> <ch> <pwm>  fire one channel directly (board 0..6, ch 0..15)
//   note <midi> <vel>        simulate a MIDI NoteOn (vel 0 = NoteOff)
//
// Examples:
//   pwm 1800          → "what's the lightest sweep that still lifts a key?"
//   fire 2 11 3000    → fire A# on board 2 (the 3rd board) at 73 % duty
//   note 60 64        → middle C, medium velocity
//   note 60 0         → release middle C
//   off               → kill everything
//
// The auto-release watchdog still applies to any channel fired via these
// commands, so a forgotten "fire" releases itself after MAX_HOLD_MS.

static char    s_lineBuf[80];
static uint8_t s_lineLen = 0;

static void printHelp() {
    Serial.println(
        "\n  serial console commands:\n"
        "    help                     this list\n"
        "    status                   boards + PWM settings\n"
        "    off                      release everything (one ALLCALL)\n"
        "    broadcast                ALLCALL FULL_OFF (hits every PCA9685 at once)\n"
        "    panic                    EMERGENCY STOP — 5x ALLCALL + force Full Power on\n"
        "    snappy                   lowest-latency preset (leds off, no deferral) + save\n"
        "    cinematic                soft/expressive preset (velocity dynamics + soft release) + save\n"
        "    save                     persist current settings to flash now\n"
        "    reset                    wipe saved settings; reboot to revert to defaults\n"
        "    sweep                    re-run the test sweep\n"
        "    pwm <0..4095>            set sweep PWM + run a sweep\n"
        "    min <0..4095>            set velocity=1 PWM floor\n"
        "    max <0..4095>            set velocity=127 PWM ceiling\n"
        "    hold <ms>                safety auto-release timeout (default 5000)\n"
        "    gap <0..300>             min ms between same-key NoteOff and next NoteOn (default 30)\n"
        "    minstrike <0..500>       min ms a solenoid stays energized — fixes \"short notes forgotten\"\n"
        "    isostrike <0..500>       longer min-strike for a LONE short note (after silence) so it sounds\n"
        "    isogap <0..2000>         silence (ms) before a note counts as isolated (0 = boost off)\n"
        "    restrike <0|40..1000>    auto re-hit held notes every N ms (tremolo sustain; 0=off)\n"
        "    softrelease 0|1          cushion current on release to stop the backstop clank\n"
        "    releasepwm <0..4095>     cushion current level (default 1100)\n"
        "    releasems <0..200>       cushion duration in ms (default 22)\n"
        "    keyforce <midi> <mult>   per-key force multiplier (0.0..2.0)\n"
        "    keyforce_all <mult>      set every key to this multiplier\n"
        "    keyforce_white <mult>    set every white key only\n"
        "    keyforce_black <mult>    set every black key only\n"
        "    velmult <0.1..5.0>       multiply incoming MIDI velocity (force boost)\n"
        "    freq <24..1526>          PWM carrier in Hz (1500 = high whine instead of buzz)\n"
        "    fullpower 0|1            1 = no PWM, FULL_ON every note (max force, no hum)\n"
        "    keyviz 0|1               piano keyboard key-highlight on NoteOn/Off\n"
        "    leds 0|1                 WS2812 strip (OFF frees ~9ms/frame under heavy load)\n"
        "    ledmode <0..3>           0 off, 1 static, 2 rainbow, 3 note-reactive\n"
        "    ledbright <0..255>       LED strip brightness\n"
        "    ledcount <1..300>        physical LEDs on the strip (sets note→LED scale)\n"
        "    ledoffset <n>            fine shift the lit LED under the played key (+/-)\n"
        "    ledscale <10..400>       %% gain on note→LED spread (fix drift, fewer LEDs than keys)\n"
        "    ledtail <n>              keep the last N LEDs dark (past keys with no solenoid)\n"
        "    ledtest <midi>           light that key's LED white for 4s (alignment calibration)\n"
        "    ledreverse 0|1           flip strip direction (high→low notes)\n"
        "    reactcolor <0..7>        reactive palette: rainbow/solid/velocity/fire/ocean/forest/lava/party\n"
        "    ledglow <0..10>          spread each note across ±N neighbour LEDs (fuller look)\n"
        "    velbright 0|1            note brightness follows how hard you play\n"
        "    decay <1..40>            note fade speed (higher = shorter linger)\n"
        "    rainspeed <1..40>        rainbow-mode scroll speed\n"
        "    pedalon 0|1              sustain-pedal servo (needs pedal board at 0x47)\n"
        "    pedalup <80..600>        servo counts for pedal RELEASED\n"
        "    pedaldown <80..600>      servo counts for pedal PRESSED\n"
        "    pedalhalf 0|1            continuous half-pedalling from the CC64 value\n"
        "    pedaltest <80..600>      drive the servo to a raw count (find endpoints)\n"
        "    dimsecs <0..3600>        screen idle-dim delay in seconds (0 = never dim)\n"
        "    waterfall 0|1            (removed — no-op, retained for back-compat)\n"
        "    fire <board> <ch> <pwm>  fire one channel (board 0..6, ch 0..15)\n"
        "    note <midi> <vel>        simulate MIDI NoteOn (vel 0 = off)\n"
    );
}

static void printStatus() {
    Serial.printf("\n  PWM settings: sweep=%u  min=%u  max=%u  hold=%lu ms  velmult=%.2f\n",
                  g_sweepStrikePWM, g_minStrikePWM, g_maxStrikePWM,
                  (unsigned long)g_maxHoldMs, g_velocityMult);
    Serial.printf("  isostrike=%lu ms (lone-note boost)  isogap=%lu ms  %s\n",
                  (unsigned long)g_isoStrikeMs, (unsigned long)g_isoGapMs,
                  (g_isoStrikeMs > g_minStrikeMs && g_isoGapMs > 0) ? "[active]" : "[off]");
    Serial.printf("  freq=%u Hz   fullpower=%s   gap=%lu ms   minstrike=%lu ms\n",
                  g_pwmFreqHz, g_fullPowerMode ? "ON" : "OFF",
                  (unsigned long)g_minRetriggerGapMs,
                  (unsigned long)g_minStrikeMs);
    Serial.printf("  softrelease=%s  releasepwm=%u  releasems=%lu ms\n",
                  g_softRelease ? "ON" : "OFF",
                  (unsigned)g_releasePwm, (unsigned long)g_releaseMs);
    Serial.printf("  restrike=%lu ms (0=off; auto-tremolo for held notes)\n",
                  (unsigned long)g_restrikeMs);
    Serial.printf("  leds=%s   dimsecs=%lu s (0=never)\n",
                  g_ledsEnabled ? "ON" : "OFF", (unsigned long)g_idleDimSecs);
    {
        const char *mn[] = {"off","static","rainbow","reactive"};
        const char *pn[] = {"rainbow","solid","velocity","fire","ocean","forest","lava","party"};
        uint8_t md = appState.ledMode < LED_MODE_COUNT ? appState.ledMode : 0;
        uint8_t pl = appState.ledReactivePalette < REACT_PALETTE_COUNT ? appState.ledReactivePalette : 0;
        Serial.printf("  led: mode=%s bright=%u/255 count=%u offset=%d scale=%u%% reverse=%s palette=%s\n",
                      mn[md], appState.ledBrightness, appState.ledCount,
                      appState.ledOffset, appState.ledScalePct,
                      appState.ledReverse ? "on" : "off", pn[pl]);
        Serial.printf("       glow=%u velbright=%s decay=%u rainspeed=%u tail=%u (keyed LEDs=%d)\n",
                      appState.ledGlow, appState.ledVelBright ? "on" : "off",
                      appState.noteDecayRate, appState.rainbowSpeed,
                      appState.ledTail, ledUsable());
    }
    Serial.printf("  pedal: %s  sustain=%s (CC64=%u)  pedalCCs=%lu\n",
                  g_pedalSeen ? "source SENDS pedal data"
                              : "no pedal data seen yet from this source",
                  g_sustainDown ? "DOWN" : "up", g_sustainRaw,
                  (unsigned long)g_pedalCcCount);
    pedal_print_status();
    Serial.printf("  i2cFails=%lu (rises during PWM-EMI storms; steady 0 is healthy)\n",
                  (unsigned long)g_i2cFailCount);
    Serial.println("  boards:");
    for (int i = 0; i < kNumPowerBoards; i++) {
        Serial.printf("    [%d] 0x%02X (MIDI %u-%u): %s\n",
                      i, powerBoards[i].chip.address(),
                      powerBoards[i].midi_start,
                      powerBoards[i].midi_start + powerBoards[i].key_count - 1,
                      powerBoards[i].connected ? "OK" : "MISSING");
    }
}

static void handleLine(char *line) {
    // Strip trailing whitespace
    while (s_lineLen > 0 && (line[s_lineLen-1] == ' ' || line[s_lineLen-1] == '\r')) {
        line[--s_lineLen] = 0;
    }
    if (s_lineLen == 0) return;

    Serial.printf("> %s\n", line);

    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        printHelp();
    } else if (!strcmp(line, "status")) {
        printStatus();
    } else if (!strcmp(line, "off")) {
        allKeysOff();
        clearLedReactive();
        pedal_release();          // pedal up → dampers drop → ringing stops
        Serial.println("  all keys released (per-board + ALLCALL)");
    } else if (!strcmp(line, "broadcast")) {
        broadcastAllOff();
        Serial.println("  ALLCALL FULL_OFF broadcast sent to 0x70");
    } else if (!strcmp(line, "panic")) {
        // Emergency stop. Hammers ALLCALL FULL_OFF five times with delays
        // to defeat bus glitches that single broadcasts might lose. Also
        // forces Full Power mode so any further notes don't use PWM.
        for (int i = 0; i < 5; ++i) { broadcastAllOff(); delay(40); }
        allKeysOff();
        clearLedReactive();
        pedal_release();          // dampers down — stop the ringing too
        g_fullPowerMode = true;
        Serial.println("  PANIC: all solenoids released, Full Power forced ON");
    } else if (!strcmp(line, "snappy")) {
        // Lowest-latency "like before" preset: no LED blocking, no timing
        // deferral, no cushion — every NoteOn fires the instant it arrives.
        g_ledsEnabled   = false;    // kill the 9ms/frame FastLED BLE-starve
        g_fullPowerMode = true;     // no PWM
        g_minRetriggerGapMs = 0;    // no retrigger deferral
        g_minStrikeMs   = 20;       // just enough to guarantee a strike
        g_softRelease   = false;    // no release cushion / extra I2C write
        g_restrikeMs    = 0;        // no auto-tremolo
        clearLedReactive();
        fill_solid(leds, LED_COUNT, CRGB::Black); FastLED.show();
        settings_save();
        Serial.println("  SNAPPY preset applied + saved: leds off, fullpower, gap 0, "
                       "minstrike 20, softrelease off, restrike off — lowest latency");
    } else if (!strcmp(line, "cinematic")) {
        // Expressive/soft preset for subtle pieces (Interstellar, Zimmer, etc.):
        // velocity DYNAMICS on (so pp stays soft, ff swells), a gentle release
        // cushion to kill the backstop clank, and short min-strike so quiet
        // notes still sound. Leaves the user's min/max strike RANGE alone (that's
        // the piano-specific soft↔loud calibration) — this flips the enablers.
        g_fullPowerMode = false;    // <-- key: velocity→force dynamics (not max-every-note)
        g_softRelease   = true;     // cushion current on release = no clank
        g_releasePwm    = DEFAULT_RELEASE_PWM;
        g_releaseMs     = 28;       // gentle let-down
        g_minStrikeMs   = 45;       // soft short notes still strike
        g_minRetriggerGapMs = 22;   // clean fast repeats without machine-gunning
        g_restrikeMs    = 0;        // no tremolo by default (add with 'restrike' if a note must sing)
        g_ledsEnabled   = true;     // lights on
        appState.ledMode = LED_MODE_NOTE_REACTIVE;
        settings_save();
        Serial.printf("  CINEMATIC preset applied + saved: velocity dynamics ON, soft-release ON, "
                      "minstrike 45, gap 22. Strike range kept (min=%u max=%u).\n"
                      "  For soft pp: lower 'min' until the quietest notes just barely sound.\n"
                      "  For a note that must sustain: 'restrike 120' adds a gentle tremolo.\n",
                      g_minStrikePWM, g_maxStrikePWM);
    } else if (!strcmp(line, "save")) {
        settings_save();
    } else if (!strcmp(line, "reset")) {
        settings_factory();
        Serial.println("  reboot now to load factory defaults from config.h");
    } else if (!strcmp(line, "sweep")) {
        testSweepAllBoards();
    } else if (!strncmp(line, "pwm ", 4)) {
        int v = atoi(line + 4);
        if (v < 0 || v > 4095) { Serial.println("  PWM out of range (0..4095)"); return; }
        g_sweepStrikePWM = (uint16_t)v;
        Serial.printf("  sweep PWM set to %d, running sweep\n", v);
        testSweepAllBoards();
    } else if (!strncmp(line, "min ", 4)) {
        int v = atoi(line + 4);
        if (v < 0 || v > 4095) { Serial.println("  min PWM out of range"); return; }
        g_minStrikePWM = (uint16_t)v;
        Serial.printf("  MIN_STRIKE_PWM = %d (velocity 1 now maps here)\n", v);
    } else if (!strncmp(line, "max ", 4)) {
        int v = atoi(line + 4);
        if (v < 0 || v > 4095) { Serial.println("  max PWM out of range"); return; }
        g_maxStrikePWM = (uint16_t)v;
        Serial.printf("  MAX_STRIKE_PWM = %d (velocity 127 now maps here)\n", v);
    } else if (!strncmp(line, "hold ", 5)) {
        int v = atoi(line + 5);
        set_max_hold_ms((uint32_t)(v < 0 ? 0 : v));   // clamps to thermal-safe ceiling
        Serial.printf("  g_maxHoldMs = %lu ms (clamped to <= %d ms for fire safety)\n",
                      (unsigned long)g_maxHoldMs, MAX_HOLD_CEILING_MS);
    } else if (!strncmp(line, "gap ", 4)) {
        int v = atoi(line + 4);
        if (v < 0 || v > 300) { Serial.println("  gap out of range (0..300 ms)"); return; }
        g_minRetriggerGapMs = (uint32_t)v;
        Serial.printf("  g_minRetriggerGapMs = %d ms — same-key NoteOns within this window are deferred\n", v);
    } else if (!strncmp(line, "minstrike ", 10)) {
        int v = atoi(line + 10);
        if (v < 0 || v > 500) { Serial.println("  minstrike out of range (0..500 ms)"); return; }
        g_minStrikeMs = (uint32_t)v;
        Serial.printf("  g_minStrikeMs = %d ms — short NoteOffs deferred so the solenoid actually strikes\n", v);
    } else if (!strncmp(line, "isostrike ", 10)) {
        int v = atoi(line + 10);
        if (v < 0 || v > 500) { Serial.println("  isostrike out of range (0..500 ms)"); return; }
        g_isoStrikeMs = (uint32_t)v;
        Serial.printf("  g_isoStrikeMs = %d ms — a LONE short note (after silence) is stretched to at least this "
                      "(vs %lu ms inside a run). %s\n", v, (unsigned long)g_minStrikeMs,
                      (g_isoStrikeMs > g_minStrikeMs && g_isoGapMs > 0) ? "active" : "(<= minstrike or gap 0 → OFF)");
    } else if (!strncmp(line, "isogap ", 7)) {
        int v = atoi(line + 7);
        if (v < 0 || v > 2000) { Serial.println("  isogap out of range (0..2000 ms)"); return; }
        g_isoGapMs = (uint32_t)v;
        Serial.printf("  g_isoGapMs = %d ms — a note with more than this much silence before it counts as isolated "
                      "(0 = boost OFF)\n", v);
    } else if (!strncmp(line, "restrike ", 9)) {
        int v = atoi(line + 9);
        if (v != 0 && (v < 40 || v > 1000)) {
            Serial.println("  restrike out of range (0 = off, or 40..1000 ms)"); return;
        }
        g_restrikeMs = (uint32_t)v;
        Serial.printf("  g_restrikeMs = %d ms — %s\n", v,
                      v ? "held notes auto re-hit at this interval (tremolo sustain)"
                        : "auto re-strike OFF (held notes strike once)");
    } else if (!strncmp(line, "softrelease ", 12)) {
        int v = atoi(line + 12);
        g_softRelease = (v != 0);
        Serial.printf("  softrelease = %s — %s\n", g_softRelease ? "ON" : "OFF",
                      g_softRelease ? "cushion current on release (anti-clank)"
                                    : "release cuts straight to 0");
    } else if (!strncmp(line, "releasepwm ", 11)) {
        int v = atoi(line + 11);
        if (v < 0 || v > 4095) { Serial.println("  releasepwm out of range (0..4095)"); return; }
        g_releasePwm = (uint16_t)v;
        Serial.printf("  g_releasePwm = %d (cushion current; keep low enough not to hold the key)\n", v);
    } else if (!strncmp(line, "releasems ", 10)) {
        int v = atoi(line + 10);
        if (v < 0 || v > 200) { Serial.println("  releasems out of range (0..200 ms)"); return; }
        g_releaseMs = (uint32_t)v;
        Serial.printf("  g_releaseMs = %d ms (cushion duration before cut to 0)\n", v);
    } else if (!strncmp(line, "keyforce ", 9)) {
        int note; float mult;
        if (sscanf(line + 9, "%d %f", &note, &mult) != 2) {
            Serial.println("  usage: keyforce <midi 0..127> <0.0..2.0>"); return;
        }
        if (note < 0 || note > 127) { Serial.println("  note out of range"); return; }
        if (mult < 0.0f || mult > 4.0f) { Serial.println("  mult out of range (0..4)"); return; }
        g_keyForceMult[note] = mult;
        Serial.printf("  g_keyForceMult[%d] = %.2fx\n", note, mult);
    } else if (!strncmp(line, "keyforce_all ", 13) ||
               !strncmp(line, "keyforce_white ", 15) ||
               !strncmp(line, "keyforce_black ", 15)) {
        // Group apply. Sets the multiplier on every key, every white key
        // (no sharps), or every black key (sharps only). Useful for
        // "all blacks are stiffer, bump them" without 36 individual writes.
        const char *arg = strchr(line, ' ') + 1;
        float mult = atof(arg);
        if (mult < 0.0f || mult > 4.0f) { Serial.println("  mult out of range (0..4)"); return; }
        // Black key semitones in any octave: 1=C#, 3=D#, 6=F#, 8=G#, 10=A#
        bool wantWhite = strncmp(line, "keyforce_white", 14) == 0;
        bool wantBlack = strncmp(line, "keyforce_black", 14) == 0;
        bool wantAll   = !wantWhite && !wantBlack;
        int hits = 0;
        for (int n = 0; n < 128; n++) {
            int s = n % 12;
            bool isBlack = (s == 1 || s == 3 || s == 6 || s == 8 || s == 10);
            if (wantAll || (wantWhite && !isBlack) || (wantBlack && isBlack)) {
                g_keyForceMult[n] = mult;
                hits++;
            }
        }
        Serial.printf("  set %d keys to %.2fx (%s)\n", hits, mult,
                      wantAll ? "all" : (wantWhite ? "white only" : "black only"));
    } else if (!strncmp(line, "velmult ", 8)) {
        float f = atof(line + 8);
        if (f < 0.1f || f > 5.0f) { Serial.println("  velmult out of range (0.1..5.0)"); return; }
        g_velocityMult = f;
        Serial.printf("  g_velocityMult = %.2f (incoming velocity scaled by this)\n", f);
    } else if (!strncmp(line, "freq ", 5)) {
        int hz = atoi(line + 5);
        if (hz < 24 || hz > 1526) { Serial.println("  freq out of range (24..1526 Hz)"); return; }
        setAllBoardsPWMFreq((uint16_t)hz);
        Serial.printf("  PWM carrier = %d Hz on all boards (higher = less coil hum)\n", hz);
    } else if (!strncmp(line, "fullpower ", 10)) {
        int v = atoi(line + 10);
        g_fullPowerMode = (v != 0);
        Serial.printf("  fullpower = %s — %s\n",
                      g_fullPowerMode ? "ON" : "OFF",
                      g_fullPowerMode
                        ? "every NoteOn drives FULL_ON. Max force, no hum, no velocity."
                        : "velocity-mapped PWM with min/max/velmult.");
    } else if (!strncmp(line, "waterfall ", 10)) {
        int v = atoi(line + 10);
        g_waterfallEnabled = (v != 0);
        Serial.println("  waterfall code has been removed — flag retained but unused");
    } else if (!strncmp(line, "keyviz ", 7)) {
        int v = atoi(line + 7);
        g_keyboardVizEnabled = (v != 0);
        Serial.printf("  keyboard viz = %s (key highlight on NoteOn/Off)\n",
                      g_keyboardVizEnabled ? "ON" : "OFF");
    } else if (!strncmp(line, "dimsecs ", 8)) {
        int v = atoi(line + 8);
        if (v < 0 || v > 3600) { Serial.println("  dimsecs out of range (0..3600, 0=never)"); return; }
        g_idleDimSecs = (uint32_t)v;
        Serial.printf("  g_idleDimSecs = %d s — screen fades down after this long idle (0=never)\n", v);
    } else if (!strncmp(line, "leds ", 5)) {
        int v = atoi(line + 5);
        g_ledsEnabled = (v != 0);
        if (!g_ledsEnabled) { clearLedReactive(); fill_solid(leds, LED_COUNT, CRGB::Black); FastLED.show(); }
        Serial.printf("  LEDs = %s%s\n", g_ledsEnabled ? "ON" : "OFF",
                      g_ledsEnabled ? "" : " (no FastLED work — frees ~9ms/frame under load)");
    } else if (!strncmp(line, "ledmode ", 8)) {
        int v = atoi(line + 8);
        if (v < 0 || v >= LED_MODE_COUNT) { Serial.println("  ledmode 0=off 1=static 2=rainbow 3=reactive"); return; }
        appState.ledMode = (LedMode)v;
        const char *nm[] = {"OFF","STATIC","RAINBOW","NOTE-REACTIVE"};
        Serial.printf("  LED mode = %s\n", nm[v]);
    } else if (!strncmp(line, "ledbright ", 10)) {
        int v = atoi(line + 10);
        if (v < 0 || v > 255) { Serial.println("  ledbright out of range (0..255)"); return; }
        appState.ledBrightness = (uint8_t)v;
        FastLED.setBrightness(appState.ledBrightness);
        Serial.printf("  LED brightness = %d/255 (%d%%)\n", v, (v * 100) / 255);
    } else if (!strncmp(line, "ledcount ", 9)) {
        int v = atoi(line + 9);
        if (v < 1 || v > LED_COUNT) { Serial.printf("  ledcount out of range (1..%d)\n", LED_COUNT); return; }
        appState.ledCount = (uint16_t)v;
        memset(noteHeat, 0, sizeof(noteHeat));
        Serial.printf("  LED count = %d (mapping updates live; reboot to change clock-out length)\n", v);
    } else if (!strncmp(line, "ledoffset ", 10)) {
        int v = atoi(line + 10);
        if (v < -LED_COUNT || v > LED_COUNT) { Serial.println("  ledoffset out of range"); return; }
        appState.ledOffset = (int16_t)v;
        Serial.printf("  LED offset = %d (shifts the lit LED under the played key)\n", v);
    } else if (!strncmp(line, "ledreverse ", 11)) {
        int v = atoi(line + 11);
        appState.ledReverse = (v != 0);
        Serial.printf("  LED reverse = %s (strip runs %s)\n",
                      appState.ledReverse ? "ON" : "OFF",
                      appState.ledReverse ? "high-note → low-note" : "low-note → high-note");
    } else if (!strncmp(line, "reactcolor ", 11)) {
        int v = atoi(line + 11);
        if (v < 0 || v >= REACT_PALETTE_COUNT) { Serial.println("  reactcolor 0..7 (rainbow/solid/velocity/fire/ocean/forest/lava/party)"); return; }
        appState.ledReactivePalette = (uint8_t)v;
        const char *pn[] = {"pitch-rainbow","solid","velocity","fire","ocean","forest","lava","party"};
        Serial.printf("  note-reactive palette = %d (%s)\n", v, pn[v]);
    } else if (!strncmp(line, "ledscale ", 9)) {
        int v = atoi(line + 9);
        if (v < 10 || v > 400) { Serial.println("  ledscale out of range (10..400 %)"); return; }
        appState.ledScalePct = (uint16_t)v;
        Serial.printf("  LED scale = %d%% (trims the note→LED drift across the keyboard)\n", v);
    } else if (!strncmp(line, "ledtail ", 8)) {
        int v = atoi(line + 8);
        if (v < 0 || v >= LED_COUNT) { Serial.println("  ledtail out of range"); return; }
        appState.ledTail = (uint8_t)v;
        clearLedReactive();
        Serial.printf("  LED tail = %d — the last %d LED(s) (past the solenoid keys) stay dark; "
                      "notes now map across %d LEDs\n", v, v, ledUsable());
    } else if (!strncmp(line, "ledtest ", 8)) {
        int note = atoi(line + 8);
        if (note < 0 || note > 127) { Serial.println("  usage: ledtest <midi 0..127>"); return; }
        s_ledTestIdx = noteToLed((uint8_t)note);
        s_ledTestUntilMs = millis() + 4000;    // hold the white marker ~4 s
        s_ledReactiveDirty = true;
        Serial.printf("  ledtest: note %d (%s) → LED %d lit white for 4 s — align it under the key\n",
                      note, noteNameC((uint8_t)note), s_ledTestIdx);
    } else if (!strncmp(line, "ledglow ", 8)) {
        int v = atoi(line + 8);
        if (v < 0 || v > 10) { Serial.println("  ledglow out of range (0..10)"); return; }
        appState.ledGlow = (uint8_t)v;
        Serial.printf("  LED glow = %d (each note lights ±%d neighbours with falloff)\n", v, v);
    } else if (!strncmp(line, "velbright ", 10)) {
        int v = atoi(line + 10);
        appState.ledVelBright = (v != 0);
        Serial.printf("  velocity→brightness = %s\n", appState.ledVelBright ? "ON (harder = brighter)" : "OFF (all notes full)");
    } else if (!strncmp(line, "decay ", 6)) {
        int v = atoi(line + 6);
        if (v < 1 || v > 40) { Serial.println("  decay out of range (1..40)"); return; }
        appState.noteDecayRate = (uint8_t)v;
        Serial.printf("  decay = %d (higher = notes fade faster / linger less)\n", v);
    } else if (!strncmp(line, "rainspeed ", 10)) {
        int v = atoi(line + 10);
        if (v < 1 || v > 40) { Serial.println("  rainspeed out of range (1..40)"); return; }
        appState.rainbowSpeed = (uint8_t)v;
        Serial.printf("  rainbow speed = %d\n", v);
    } else if (!strncmp(line, "pedalon ", 8)) {
        int v = atoi(line + 8);
        g_pedalEnabled = (v != 0);
        if (g_pedalEnabled) pedal_init(); else pedal_release();
        Serial.printf("  sustain pedal = %s\n", g_pedalEnabled ? "ENABLED" : "disabled");
    } else if (!strncmp(line, "pedalup ", 8)) {
        int v = atoi(line + 8);
        if (v < 80 || v > 600) { Serial.println("  pedalup out of range (80..600 counts)"); return; }
        g_pedalUpCounts = (uint16_t)v;
        pedal_release();                    // move to the new UP position now
        Serial.printf("  pedal UP = %d counts (~%.2f ms pulse)\n", v, v * 20.0 / 4096.0);
    } else if (!strncmp(line, "pedaldown ", 10)) {
        int v = atoi(line + 10);
        if (v < 80 || v > 600) { Serial.println("  pedaldown out of range (80..600 counts)"); return; }
        g_pedalDownCounts = (uint16_t)v;
        Serial.printf("  pedal DOWN = %d counts (~%.2f ms pulse)\n", v, v * 20.0 / 4096.0);
    } else if (!strncmp(line, "pedalhalf ", 10)) {
        int v = atoi(line + 10);
        g_pedalHalf = (v != 0);
        Serial.printf("  half-pedalling = %s\n", g_pedalHalf
                      ? "ON (CC64 value maps continuously)"
                      : "off (CC64 <64 = up, >=64 = down)");
    } else if (!strncmp(line, "pedaltest ", 10)) {
        // Drive the servo to a raw count so you can find the endpoints safely.
        int v = atoi(line + 10);
        if (v < 80 || v > 600) { Serial.println("  pedaltest out of range (80..600)"); return; }
        if (!g_pedalEnabled) { Serial.println("  enable it first: pedalon 1"); return; }
        pedal_test_counts((uint16_t)v);
        Serial.printf("  pedal servo -> %d counts (~%.2f ms). Creep up on the limits — "
                      "a stalled servo cooks itself.\n", v, v * 20.0 / 4096.0);
    } else if (!strncmp(line, "fire ", 5)) {
        int b, c, p;
        if (sscanf(line + 5, "%d %d %d", &b, &c, &p) != 3) {
            Serial.println("  usage: fire <board> <channel> <pwm>"); return;
        }
        bool ok = manualFire((uint8_t)b, (uint8_t)c, (uint16_t)p);
        Serial.printf("  fire board=%d ch=%d pwm=%d → %s\n",
                      b, c, p, ok ? "OK" : "FAIL");
    } else if (!strncmp(line, "note ", 5)) {
        int n, v;
        if (sscanf(line + 5, "%d %d", &n, &v) != 2) {
            Serial.println("  usage: note <midi> <velocity>"); return;
        }
        if (v == 0) {
            dispatchNoteOff((uint8_t)n);
        } else {
            dispatchNoteOn((uint8_t)n, (uint8_t)v);
        }
    } else if (!strncmp(line, "rawnote ", 8)) {
        // Strike tester: fire a MIDI note at a RAW pwm (0..4096), bypassing
        // the velocity curve / soft-release / min-strike. Lets you sweep
        // FORCE independently; the GUI handles the release timing (DURATION).
        int n, pwm;
        if (sscanf(line + 8, "%d %d", &n, &pwm) != 2) {
            Serial.println("  usage: rawnote <midi> <pwm 0..4096>"); return;
        }
        if (pwm < 0) pwm = 0; if (pwm > 4096) pwm = 4096;
        bool ok = manualFireNote((uint8_t)n, (uint16_t)pwm);
        Serial.printf("  rawnote %d pwm=%d → %s\n", n, pwm, ok ? "OK" : "no board for note");
    } else {
        Serial.printf("  unknown command — type 'help'\n");
    }
}

static void pollSerial() {
    // Cap commands handled per loop iteration. The GUI MIDI player streams
    // `note` lines in bursts (10-note chords); without a cap a burst would
    // dispatch unbounded inline I²C writes here, bypassing the event-drain
    // budget and delaying the safety ticks. Leftover bytes stay in the RX
    // buffer and are handled next iteration (loop runs ~1 kHz idle).
    int linesHandled = 0;
    while (Serial.available() && linesHandled < 8) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (s_lineLen > 0) {
                s_lineBuf[s_lineLen] = 0;
                handleLine(s_lineBuf);
                s_lineLen = 0;
                linesHandled++;
            }
        } else if (s_lineLen < sizeof(s_lineBuf) - 1) {
            s_lineBuf[s_lineLen++] = (char)c;
        }
    }
    // Serial activity (GUI MIDI player / console) counts as piano activity
    // for the idle dimmer — a serial-driven song must not dim mid-piece.
    if (linesHandled > 0) s_lastNoteActivityMs = millis();
}

static void onBleConnected() {
    appState.bleConnected = true;
    appState.peerName = "MIDI client";
    Serial.println("[BLE] central connected");
    g_status_dirty = true;
}

// Set by onBleDisconnected (NimBLE task, core 0); serviced by loop() on
// core 1. We do NOT call allKeysOff() from the BLE callback: that runs on a
// different core than the dispatch pipeline, so it would race with a
// concurrent fireNoteOnNow / tickPendingFires and could leave a solenoid
// energized AFTER the stop (queued NoteOns firing post-disconnect). Handling
// it on core 1 makes the stop + eventQ flush + dispatch strictly sequential.
static volatile bool g_requestStopFromDisconnect = false;

static void onBleDisconnected() {
    appState.bleConnected = false;
    appState.peerName = "";
    Serial.println("[BLE] central disconnected — restarting advertising");
    g_status_dirty = true;

    // Defer the actual solenoid stop to loop() (core 1) — see the flag's
    // comment above. Keep this callback short.
    g_requestStopFromDisconnect = true;

    // Explicit re-advertise. lathoub's BLE-MIDI library should restart
    // advertising automatically on disconnect, but on some NimBLE versions
    // / iOS combos it's flaky — call it ourselves to be sure. NimBLE
    // start() is idempotent; if advertising is already running, no-op.
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv) adv->start();
}

// Print why the chip just rebooted. Crucial for diagnosing recurring
// crashes — PANIC means software bug, INT_WDT/TASK_WDT means hang,
// BROWNOUT means power supply sagged (24V load too high or rail noise
// coupling into 5V), DEEPSLEEP/POWERON/EXT are normal.
static void logResetReason() {
    const char *name = "UNKNOWN";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:    name = "POWERON (cold boot)"; break;
        case ESP_RST_EXT:        name = "EXT (reset button)"; break;
        case ESP_RST_SW:         name = "SW (esp_restart called)"; break;
        case ESP_RST_PANIC:      name = "*** PANIC (software crash / Guru Meditation) ***"; break;
        case ESP_RST_INT_WDT:    name = "*** INT_WDT (interrupt watchdog) ***"; break;
        case ESP_RST_TASK_WDT:   name = "*** TASK_WDT (task watchdog) ***"; break;
        case ESP_RST_WDT:        name = "*** WDT (other watchdog) ***"; break;
        case ESP_RST_DEEPSLEEP:  name = "DEEPSLEEP (woke from sleep)"; break;
        case ESP_RST_BROWNOUT:   name = "*** BROWNOUT (power supply sagged) ***"; break;
        case ESP_RST_SDIO:       name = "SDIO"; break;
        default: break;
    }
    Serial.printf("[boot] reset reason: %s\n", name);
}

void setup() {
    // Larger RX buffer for MIDI-player bursts over serial: a 10-note chord
    // is ~130 bytes of `note` lines arriving back-to-back; combined with the
    // 8-line pollSerial budget this rides out multi-chord bursts without
    // overflow. Must be set before begin().
    Serial.setRxBufferSize(1024);
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[piano] boot");
    logResetReason();

    // Keep the authorship watermark in the flashed image. A volatile read
    // makes kBuildProvenance a live reference so the linker's --gc-sections
    // can't drop the string — it then appears in `strings firmware.bin`.
    { volatile char _kp = kBuildProvenance[0]; (void)_kp; }

    // Hardware watchdog. If the main loop hangs for more than 8 s (BLE host
    // task crash, Wire bus lockup, or any freeze), the ESP32 self-reboots.
    // After reboot, broadcastAllOff() below silences any solenoid still
    // energized from before the crash. Without this, a hung firmware leaves
    // coils energized indefinitely — the "screen dark + stuck note" mode.
    // 8 s timeout — fastest practical recovery. Stuck solenoid
    // heats ~5°C in 8 s vs the 80°C-in-5-min thermal envelope; this keeps
    // worst-case stuck-on heating bounded even across multiple crash loops.
    // Loop iteration is microseconds normally; only manual sweep is long
    // and feeds the watchdog internally.
    esp_task_wdt_init(8, true);    // panic on timeout
    esp_task_wdt_add(NULL);        // supervise the loopTask (current task)

    // Pull saved tunables from NVS BEFORE anything reads them. After this,
    // appState fields and the g_* globals reflect either user-saved values
    // or the config.h factory defaults.
    settings_load();

    // Crash-loop guard. If we got here 3+ times without a healthy uptime
    // in between, settings_boot_inc() forces Full Power on regardless of
    // saved settings. settings_boot_reset() in loop() clears the counter
    // once we've been running stably for 30 s.
    settings_boot_inc();

    // *** EARLIEST fire-safety release ***
    // The PCA9685s hold their last PWM state across an ESP reset — a coil that
    // was energized when we crashed/rebooted is STILL DRIVING right now. On the
    // isolated board the PCA bus (Wire1) is independent of the display, so bring
    // it up and broadcast FULL_OFF *before* amoled.begin(), which is slow and —
    // on a flaky panel or a post-brownout boot — can hang in the infinite loop
    // below WITHOUT ever releasing the coils. This guarantees solenoids are cut
    // within ~150 ms of every boot even if the display never comes up.
#if PCA_USES_WIRE1
    Wire1.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
    PCA_BUS.setTimeOut(5);
    wire_lock_init();                         // mutex must exist before broadcastAllOff
    for (int i = 0; i < 3; ++i) { broadcastAllOff(); delay(50); }
    Serial.println("[boot] early ALLCALL FULL_OFF sent (pre-display safety)");
#endif

    if (!amoled.begin()) {
        Serial.println("[piano] amoled.begin() failed — check board variant");
        // Coils are already released above (isolated board). Keep re-releasing
        // while halted so nothing can creep back on, and feed the watchdog so
        // this stays a defined, SAFE halt rather than a WDT reboot loop.
        while (true) {
            esp_task_wdt_reset();
#if PCA_USES_WIRE1
            broadcastAllOff();
#endif
            delay(500);
        }
    }
    amoled.setRotation(1);
    amoled.setBrightness(appState.screenBrightness);
    beginLvglHelper(amoled);

    // Slow the shared I²C bus to a margin-safe rate. Touch + PMU + 8 PCA9685
    // chips + Cat7 trunk to power boards is a lot of capacitance; 100 kHz
    // gives a comfortable floor and we can bump to 400 kHz later once we've
    // verified clean transactions in scope.
#if PCA_USES_WIRE1
    // Isolated board: the DEDICATED PCA9685 bus (Wire1), its timeout, the wire
    // mutex, and the emergency all-off were ALL brought up early (before the
    // display) for fire safety. Re-broadcast FULL_OFF now that every rail is
    // definitely up — belt-and-suspenders in case the early one raced a rail.
    for (int i = 0; i < 3; ++i) {
        broadcastAllOff();
        delay(50);
    }
#else
    // Original board: PCAs share the touch/PMU bus that amoled.begin()
    // already initialized. Just bump the clock — do NOT call Wire.begin()
    // again, that would clobber the touch driver's config.
    Wire.setClock(I2C_FREQ_HZ);
    // CRITICAL for PWM-mode responsiveness: the ESP32 Arduino default I²C
    // timeout is 50 ms PER FAILED TRANSACTION. 20 ms keeps margin for the
    // touch controller's clock stretching while cutting worst-case stall 2.5×.
    PCA_BUS.setTimeOut(20);
    // Mutex must exist BEFORE any of our code touches the PCA bus.
    wire_lock_init();
    // Emergency release — the PCAs kept their last PWM state across the reset.
    for (int i = 0; i < 3; ++i) {
        broadcastAllOff();
        delay(50);
    }
#endif

    initPowerBoards();
    pedal_init();          // optional sustain-pedal servo (its own PCA9685)
    // Scan runs after ui_init() below so the result can also be mirrored on
    // the AMOLED via the I2C status label.

#if RUN_TEST_SWEEP_ON_BOOT
    // Click every wired channel in sequence — verifies the I²C → PCA9685 →
    // MOSFET → solenoid chain on each connected board before BLE comes up.
    // Disable in config.h once everything is validated.
    testSweepAllBoards();
#endif

    // Register only the physically-installed LEDs (from NVS, default 73) so
    // each show() clocks out just the real strip — ~4× faster than clocking
    // the full 300-LED array and lighter on core-1 during dense passages.
    // (Changing the count at runtime saves to NVS and takes effect on reboot;
    //  offset/reverse/palette/brightness all apply live with no reboot.)
    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, ledN());
    FastLED.setBrightness(appState.ledBrightness);
    // Power budget for the strip's dedicated 5 V supply. FastLED scales down
    // any frame that would exceed it, so effects can't brown the rail out.
    FastLED.setMaxPowerInVoltsAndMilliamps(5, LED_MAX_MILLIAMPS);
    fill_solid(leds, LED_COUNT, CRGB::Black);
    FastLED.show();

    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.setHandleNoteOn(handleNoteOn);
    MIDI.setHandleNoteOff(handleNoteOff);
    MIDI.setHandleControlChange(handleControlChange);
    MIDI.setHandleProgramChange(handleProgramChange);
    MIDI.setHandlePitchBend(handlePitchBend);
    MIDI.setHandleClock(handleClock);

    BLEMIDI.setHandleConnected(onBleConnected);
    BLEMIDI.setHandleDisconnected(onBleDisconnected);

    // Fast advertising for quick auto-reconnect. Intervals are in 0.625 ms
    // units, so 32..64 = 20..40 ms — the BLE "fast connect" window Apple
    // devices scan for. The default is several times slower, which is what
    // makes a re-pair feel like it hangs. Cheap: we only advertise while
    // disconnected, so there's no ongoing cost once the iPad is on.
    {
        NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
        if (adv) {
            adv->setMinInterval(32);   // 20 ms
            adv->setMaxInterval(64);   // 40 ms
            adv->start();
        }
    }

    ui_init();
    ui_refresh_status();
    scanI2CBus();   // populates the I2C label on the AMOLED + Serial summary
    Serial.printf("[piano] BLE MIDI advertising as \"%s\"\n", BLE_MIDI_DEVICE_NAME);
    Serial.println("[piano] serial console ready — type 'help' for commands");

    s_ui_mutex = xSemaphoreCreateMutex();
    // Pin LVGL to core 1 — SAME core as the Arduino loop. Both tasks share
    // Wire (LVGL's touch driver polls CST226SE at 0x5A; dispatch writes to
    // PCA9685s at 0x40..0x46). ESP32's Wire library is not multi-core safe,
    // so running LVGL on core 0 used to let touch polls interleave with
    // dispatch writes mid-transaction — the smoking gun for "screen dark +
    // stuck note + PWM hum" crashes during heavy passages. On the same
    // core, FreeRTOS serializes them and the race vanishes.
    xTaskCreatePinnedToCore(
        lvgl_render_task,   // function
        "lvgl",             // name
        8192,               // stack words
        nullptr,            // params
        1,                  // priority (below NimBLE host)
        &s_lvgl_task,
        1                   // core 1 — same as Arduino loop, serializes Wire access
    );
}

void loop() {
    esp_task_wdt_reset();   // feed the hardware watchdog — see setup()

    // --- Safety FIRST, every iteration, before any note dispatch ---
    // Run the release watchdog + deferred-timer drain at the very top so a
    // flood of incoming notes can never delay the stuck-coil safety net.
    // These are cheap (a few hundred cmp ops) and touch only the PCA bus.
    static uint32_t lastSafetyTick = 0;
    uint32_t nowTop = millis();
    if (nowTop - lastSafetyTick >= 10) {   // ~100 Hz
        lastSafetyTick = nowTop;
        tickAutoRelease();
        tickPendingFires();
        tickRestrike();     // auto-tremolo for held notes (no-op when off)
    }

    // Service a BLE-disconnect stop request on THIS core (core 1). Doing it
    // here — not in the NimBLE callback (core 0) — keeps the flush +
    // allKeysOff sequential with dispatch, so no queued/deferred NoteOn can
    // re-energize a solenoid after the stop. settings_save() is intentionally
    // NOT called here (blocking NVS write in the hot path); the 30 s auto-save
    // and explicit "save" cover persistence.
    if (g_requestStopFromDisconnect) {
        g_requestStopFromDisconnect = false;
        flushEventQueue();       // discard any MIDI events queued before the drop
        allKeysOff();            // release outputs + clear every deferred timer
        clearLedReactive();      // and drop any sustained LEDs
        pedal_release();         // lift the pedal or the piano rings on
        Serial.println("[BLE] disconnect stop serviced on core 1");
    }

    // Service a MIDI All-Notes-Off / panic (CC 120/121/123) the same way.
    // Log sustain-pedal transitions from core 1 (never from the BLE callback).
    // This is the "do my MIDI files have pedal?" probe: play a song and watch
    // for these lines. Guarded print so a headless install can't block here.
    if (g_pedalEventDirty) {
        g_pedalEventDirty = false;
        if (Serial.availableForWrite() >= 64) {
            Serial.printf("[pedal] sustain %s (CC64=%u)\n",
                          g_sustainDown ? "DOWN" : "up", g_sustainRaw);
        }
    }

    if (g_requestAllOff) {
        g_requestAllOff = false;
        allKeysOff();
        clearLedReactive();      // held LEDs release on all-notes-off too
        pedal_release();         // CC120/121/123 releases the pedal as well
        Serial.println("[midi] all-notes-off serviced");
    }

    MIDI.read();   // populates eventQ via our handleNoteOn/Off callbacks

    // Drain events for solenoid dispatch. CAP per iteration so a burst can't
    // monopolize the core with back-to-back blocking I²C writes and starve
    // the safety ticks / LVGL. Leftover events stay queued (128-deep) and
    // drain over the next iterations — loop runs fast enough (<1 ms idle)
    // that the cap doesn't add audible latency, but it guarantees the
    // watchdog above keeps its ~100 Hz cadence even under a note flood.
    static const int kMaxDrainPerLoop = 24;
    NoteEvent staged[kMaxDrainPerLoop];
    int staged_count = 0;
    NoteEvent e;
    while (staged_count < kMaxDrainPerLoop && dequeueEvent(e)) {
        // Fire / release the solenoid via the PCA9685 chain (outside the
        // queue lock — I²C is slow).
        if (e.on) dispatchNoteOn(e.note, e.velocity);
        else      dispatchNoteOff(e.note);

        // Light the note's LED RIGHT HERE — the same place, same instant the
        // solenoid fires — so the strip is perfectly in sync and never dropped
        // by LVGL lock contention (the old bug: reactions lived inside the
        // ui_lock block and vanished when the screen was busy).
        if (g_ledsEnabled && appState.ledMode == LED_MODE_NOTE_REACTIVE) {
            int idx = noteToLed(e.note);
            int n   = ledUsable();          // stay within the keyed LEDs (dark tail)
            int g   = appState.ledGlow;
            if (e.on) {
                CRGB col = reactiveColorFor(e.note, e.velocity);
                // Peak brightness: full, or scaled by velocity if enabled.
                uint8_t peak = appState.ledVelBright
                               ? (uint8_t)map(e.velocity, 1, 127, 60, 255) : 255;
                for (int d = -g; d <= g; d++) {       // center + glow neighbours
                    int j = idx + d;
                    if (j < 0 || j >= n) continue;
                    // Linear falloff from the center; brighter note wins overlaps.
                    uint8_t h = (g == 0) ? peak
                                : (uint8_t)((int)peak * (g + 1 - abs(d)) / (g + 1));
                    if (h > noteHeat[j]) { noteHeat[j] = h; noteColor[j] = col; }
                    if (ledHeld[j] < 255) ledHeld[j]++;   // key down → sustain
                }
            } else {
                for (int d = -g; d <= g; d++) {       // key up → allow it to fade
                    int j = idx + d;
                    if (j < 0 || j >= n) continue;
                    if (ledHeld[j] > 0) ledHeld[j]--;
                }
            }
            s_ledReactiveDirty = true;    // ask the loop to push a frame ASAP
        }

        staged[staged_count++] = e;
    }

    static uint32_t lastUiTick = 0;
    if (ui_lock(5)) {
        if (staged_count > 0) s_lastNoteActivityMs = millis();

        for (int i = 0; i < staged_count; i++) {
            const NoteEvent& e = staged[i];
            if (e.on) {
                appState.notesReceived++;
                appState.lastNote = e.note;
                appState.lastVelocity = e.velocity;
                // (LED reaction now fires in the dispatch loop above, in sync
                //  with the solenoid — not here, so it's never lost when the
                //  screen is busy.)
                ui_set_key(e.note, true);
                ui_refresh_note(e.note, e.velocity);
            } else {
                ui_set_key(e.note, false);
            }
        }

        if (g_status_dirty) {
            g_status_dirty = false;
            ui_refresh_status();
        }

        uint32_t now_locked = millis();
        if (now_locked - lastUiTick >= 40) {  // ~25 fps UI tick
            lastUiTick = now_locked;
            ui_tick();

            // --- Gradual idle dimming (runs at the same 25 Hz cadence,
            //     inside the LVGL lock since brightness shares the panel).
            // Idle = time since the MOST RECENT of {last note, last touch}.
            // lv_disp_get_inactive_time covers touch; s_lastNoteActivityMs
            // covers played notes. User's set level (appState.screen-
            // Brightness) is the wake target and is never modified here.
            static uint8_t s_curBright = 0;      // actual level on the panel
            static bool    s_dimInit = false;
            if (!s_dimInit) { s_curBright = appState.screenBrightness; s_dimInit = true; }

            uint32_t idleNoteMs  = now_locked - s_lastNoteActivityMs;
            if (s_lastNoteActivityMs == 0) idleNoteMs = UINT32_MAX;  // no notes yet
            uint32_t idleTouchMs = lv_disp_get_inactive_time(NULL);
            uint32_t idleMs = (idleNoteMs < idleTouchMs) ? idleNoteMs : idleTouchMs;

            uint8_t userLevel = appState.screenBrightness;
            uint8_t floorLevel = (userLevel < IDLE_DIM_FLOOR) ? userLevel : (uint8_t)IDLE_DIM_FLOOR;
            uint8_t target = (g_idleDimSecs != 0 && idleMs >= g_idleDimSecs * 1000UL)
                             ? floorLevel : userLevel;

            if (s_curBright != target) {
                bool idleFade = (target < userLevel);   // going down BECAUSE idle
                if (s_curBright > target) {
                    if (idleFade) {
                        // Idle fade DOWN: slow + cinematic, 1 step / 40 ms
                        // ≈ 7 s from 180 → 10.
                        s_curBright--;
                    } else {
                        // User lowered the slider while awake — track it
                        // fast (16 steps / 40 ms ≈ 0.5 s), not at idle-fade
                        // speed (which lagged the slider by many seconds).
                        int16_t dn = (int16_t)s_curBright - 16;
                        s_curBright = (dn < target) ? target : (uint8_t)dn;
                    }
                } else {
                    // Ramp UP fast on activity: 16 steps / 40 ms ≈ 0.5 s.
                    uint16_t up = s_curBright + 16;
                    s_curBright = (up > target) ? target : (uint8_t)up;
                }
                amoled.setBrightness(s_curBright);
            }
        }
        ui_unlock();
    }
    // If we couldn't get the lock: the SOLENOIDS already fired (dispatch runs
    // before this block) — only this iteration's screen updates (key
    // highlights, note counter) are skipped. Worst case a key highlight
    // lags/misses one frame under heavy LVGL contention; audio is unaffected.

    static uint32_t lastFrame = 0;
    uint32_t now = millis();
    // Normally cap LED output at ~30 fps (each show() clocks the strip out and
    // competes with note dispatch). But when a note JUST lit an LED, push the
    // frame as soon as ~12 ms have passed so the reaction feels instant instead
    // of waiting up to 33 ms. A chord sets the flag once and one frame renders
    // every lit note, so this can't storm show() during dense passages.
    uint32_t frameGap = s_ledReactiveDirty ? 12 : 33;
    if (now - lastFrame >= frameGap) {
        lastFrame = now;
        s_ledReactiveDirty = false;
        updateLeds();
    }

    // Sustain pedal: apply the latest CC64 here on the dispatch core (never
    // from the BLE callback). Change-gated + rate-limited inside, and a no-op
    // when no pedal board is installed.
    pedal_tick(g_sustainRaw);
    // (auto-release + pending-fire ticks now run at the TOP of loop() so a
    // note flood can never delay the safety net — see lastSafetyTick above.)

    // Health check: every 5 s, re-init any PCA9685 that's stopped acking.
    // Cheap (just an I²C address probe per board) and self-healing if
    // EMI from heavy PWM activity ever browns a chip out.
    tickChipHealth();

    // Drain anything typed in the serial console (PWM tuning commands).
    pollSerial();

    // Persist tunables every 30 s. Cheap when nothing changed (NVS
    // internally dedupes writes against current flash contents).
    settings_tick();

    // Single diagnostic heartbeat every 30 s. Guarded by availableForWrite()
    // so a full/unread USB-CDC buffer (e.g. no monitor attached on the
    // permanent install) can never block the loop on a Serial write.
    static uint32_t lastHb = 0;
    if (now - lastHb >= 30000) {
        lastHb = now;
        if (Serial.availableForWrite() >= 128) {
            Serial.printf("[hb] up=%lus  freeHeap=%u  minHeap=%u  ble=%s  i2cFails=%lu\n",
                          (unsigned long)(now / 1000),
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)ESP.getMinFreeHeap(),
                          appState.bleConnected ? "yes" : "no",
                          (unsigned long)g_i2cFailCount);
        }
    }

    // After 30 s of healthy uptime the firmware is clearly not crash-looping
    // — clear the boot counter so next normal reboot starts fresh.
    static bool s_bootCounterCleared = false;
    if (!s_bootCounterCleared && now > 30000) {
        s_bootCounterCleared = true;
        settings_boot_reset();
    }

    // BLE re-advertise watchdog. If we've been disconnected for more than
    // 5 s and aren't advertising, force it. Catches edge cases where the
    // disconnect callback's start() didn't take effect (rare iOS/NimBLE
    // bug). Calling start() when already advertising is a safe no-op.
    static uint32_t lastAdvKick = 0;
    if (!appState.bleConnected && now - lastAdvKick >= 5000) {
        lastAdvKick = now;
        NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
        if (adv) {
            adv->start();
            // Guarded like the heartbeat: on the headless install nothing
            // drains USB-CDC, and an unguarded println here would block the
            // loop every 5 s while disconnected.
            if (Serial.availableForWrite() >= 64) {
                Serial.println("[BLE] advertising kicked (auto-reconnect)");
            }
        }
    }

    delay(1);
}
