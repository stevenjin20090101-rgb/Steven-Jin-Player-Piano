// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#pragma once

#include <Arduino.h>

#define BLE_MIDI_DEVICE_NAME "Steven Piano"

// ===========================================================================
// BOARD REVISION SELECT — set this to match the PHYSICAL board connected.
// ===========================================================================
//   0 = ORIGINAL board (what you have now):
//         LED on GPIO 18, PCA9685s share the touch/PMU bus (Wire, GPIO 6/7).
//   1 = 2026-05 OPTO-ISOLATED board (吟晚风's new rev, not built yet):
//         LED on GPIO 41, PCA9685s on a DEDICATED bus (Wire1, GPIO 42/40).
//
// If the LED doesn't light or the power boards don't show in the I²C scan,
// this number probably doesn't match your hardware.
#define BOARD_REV  1   // NEW opto-isolated board: trunk on Wire1 (SDA 42 /
                       // SCL 40), LED 41 — the wiring 吟晚风 specified and
                       // the configuration confirmed working on this piano.
                       // Only set back to 0 for the original shared-bus board.
// ===========================================================================

#if BOARD_REV == 0
    // --- Original board: PCAs share Wire (GPIO 6/7) ---
    #define I2C_SDA_PIN      6
    #define I2C_SCL_PIN      7
    #define PCA_USES_WIRE1   0    // PCAs on shared Wire (init by amoled.begin)
#else
    // --- Isolated board: PCAs on dedicated Wire1 (GPIO 42/40) ---
    // Two separate I²C peripherals: touch/PMU stay on Wire (6/7); power
    // boards get Wire1 (42/40) through the opto-isolator. Solenoid EMI on
    // the power side can't reach the display/touch bus.
    #define I2C_SDA_PIN      42
    #define I2C_SCL_PIN      40
    #define PCA_USES_WIRE1   1    // PCAs on dedicated Wire1 (we init it)
#endif

// LED data on GPIO 41 for BOTH board revs. GPIO 41 is a free header pin in
// every configuration, matches 吟晚风's final wiring plan (40 SCL / 41 LED /
// 42 SDA), and — unlike the old GPIO 18 — has no entanglement with the
// AMOLED's TE line / R19 jumper. If your LED data wire is physically on the
// GPIO-18 pad, move it to the GPIO-41 pad (header pin 12... check silk).
#define LED_DATA_PIN     41

// Strip: BTF-LIGHTING WS2812B, 5 V, 5 m / 300 LEDs, IP65, black PCB.
// Full-white worst case is 300 × 60 mA = 18 A — far beyond the 5 V rail —
// so main.cpp caps total LED power via FastLED's power manager (3 A budget);
// FastLED automatically dims frames that would exceed it.
#define LED_COUNT        300     // max the firmware supports (array size)
#define LED_DEFAULT_BRIGHTNESS 160

// --- Physical strip / note→LED mapping (all runtime-tunable, persisted) -----
// The strip actually installed is usually shorter than LED_COUNT. The played
// key range (MIDI_NOTE_MIN..MAX) is mapped onto DEFAULT_LED_ACTIVE physical
// LEDs. Offset shifts the whole mapping so the lit LED sits under the played
// key; reverse flips it if the strip runs treble→bass.
#define DEFAULT_LED_ACTIVE    73     // physical LEDs on the strip (<= LED_COUNT)
#define DEFAULT_LED_OFFSET    0      // shift note→LED position (+/-), fine per-light
#define DEFAULT_LED_REVERSE   0      // 1 = strip runs high-note → low-note
// Scale (gain) on the note→LED spread, in percent. 100 = the full key range
// fills the strip exactly. With fewer LEDs than keys the strip drifts ahead of
// the keys as you go up; trimming the scale corrects that drift. Anchored at
// the low end, so: set the LOW key with Offset, then the HIGH key with Scale.
#define DEFAULT_LED_SCALE_PCT 100    // 10..400 %
// LEDs at the far (high-note) end of the strip that sit past the last key with
// a solenoid (e.g. the top keys have no drivers). These are excluded from the
// note→LED mapping and kept dark, so lit LEDs == keys that can actually play.
#define DEFAULT_LED_TAIL      0      // 0..LED_COUNT-1
// Note-reactive colour palette: 0 pitch-rainbow, 1 solid(static colour),
// 2 velocity heat, 3 fire, 4 ocean, 5 forest, 6 lava, 7 party.
#define DEFAULT_REACT_PALETTE 0
#define REACT_PALETTE_COUNT   8
// Note-reactive feel: glow spreads each note across ±N neighbouring LEDs with
// falloff (fuller look on a sparse strip); velocity-brightness scales a note's
// brightness by how hard the key was hit.
#define DEFAULT_LED_GLOW      1      // 0..10 neighbours each side
#define DEFAULT_LED_VELBRIGHT 0      // 1 = brightness follows velocity

// LED strip 5 V supply budget for FastLED's power manager. With a dedicated
// 5 A+ supply, 73 LEDs at full white (~4 A) fit comfortably, so full
// brightness is always available. Lower this if the strip shares the board 5 V.
#define LED_MAX_MILLIAMPS     5000

// Master LED enable. FastLED.show() for 300 LEDs is ~9 ms of tightly-timed
// output PER FRAME that can stall the loop and starve the BLE stack during
// dense MIDI. When disabled, updateLeds() skips ALL FastLED work — zero
// overhead. Default OFF because the strip is currently unused/dead and the
// overhead is a real crash-under-load risk. Toggle with "leds 0|1"; persisted.
#define DEFAULT_LEDS_ENABLED  0
extern bool g_ledsEnabled;

// --- Idle display dimming ---------------------------------------------------
// After this many seconds with NO note activity and NO touch input, the
// AMOLED fades down gradually (1 step / 40 ms ≈ 7 s full fade) to
// IDLE_DIM_FLOOR. Any note or touch ramps it back up fast (~1 s). The
// user-set brightness (appState.screenBrightness) is never modified — the
// dimmer drives the panel directly, so wake always returns to the user's
// level and NVS persistence is unaffected. 0 = never dim.
// Runtime-tunable via "dimsecs <0..3600>"; persisted.
#define DEFAULT_IDLE_DIM_SECS  60
#define IDLE_DIM_FLOOR         10
extern uint32_t g_idleDimSecs;

#define ONEWIRE_PIN      21   // DS18B20 trunk (chained across power boards)

#define I2C_FREQ_HZ      400000  // 400 kHz. If an opto-isolator on the new
                                 // board can't keep up, drop to 100000.

// Which TwoWire instance the PCA9685 driver talks to, derived from the
// board rev above. On the original board this is the shared Wire; on the
// isolated board it's the dedicated Wire1.
#if PCA_USES_WIRE1
    #define PCA_BUS Wire1
#else
    #define PCA_BUS Wire
#endif

// Playable range = the notes the 7 power boards actually cover (C1..B7).
// Kept in sync with the board table in power_boards.cpp so the touch UI
// only ever offers keys that can physically play, and the LED map spans the
// real range. Notes outside this (a piano's bottom A0-B0 and top C8) have no
// board and are silently ignored by dispatch anyway.
#define MIDI_NOTE_MIN    24   // C1  (board 0, 0x40)
#define MIDI_NOTE_MAX    107  // B7  (board 6, 0x46)

// Set to 1 to run testSweepAllBoards() once after init in setup(). Useful
// during bring-up to verify each PCA9685 channel + MOSFET + solenoid chain.
// Default 0 — chain is validated, no need to swing every key on every boot.
// You can still trigger a sweep on demand via the "sweep" serial command
// or the "Run sweep now" button in the GUI.
#define RUN_TEST_SWEEP_ON_BOOT  0

// Waterfall visualization on the AMOLED. Each NoteOn spawns a falling bar
// that animates frame-by-frame in ui_tick() — CPU + Wire traffic per frame.
// Default OFF for safety: cuts LVGL render load roughly in half and
// eliminates one source of bus contention. Toggle via "waterfall 0|1"
// serial command or the web-UI switch. Setting persists to NVS.
#define DEFAULT_WATERFALL_ENABLED  0

// Set to 1 to print every dispatchNoteOn/Off to Serial. Helpful while
// debugging "why aren't solenoids firing on commanded notes" — you can
// see in the monitor whether dispatch was even reached. KEEP OFF in
// normal use; each printf is a synchronous USB CDC write that blocks
// for hundreds of microseconds and can contribute to firmware crashes
// during dense passages like Rush E.
#define DEBUG_DISPATCH          0

// --- Solenoid strike tuning ------------------------------------------------
// These are *defaults* — runtime values live in power_boards.cpp and can be
// tweaked live over the serial console (see "pwm" / "min" / "max" commands).
// 12-bit PCA9685 PWM: 0 = always-low, 4095 = always-high.
//
// SWEEP_STRIKE_PWM:  duty used by the boot self-test for each channel.
// MIN_STRIKE_PWM:    PWM value that velocity = 1 maps to. Floor it high
//                    enough that the slowest velocity still latches the
//                    solenoid plunger — typically ~35–40 % duty.
// MAX_STRIKE_PWM:    PWM value that velocity = 127 maps to. Cap below
//                    4095 if max strike is too violent on the bench.
// SWEEP_STRIKE_MS:   how long each sweep strike stays on, ms.
// SWEEP_RELEASE_MS:  gap between sweep strikes, ms.
#define SWEEP_STRIKE_PWM   2500
#define MIN_STRIKE_PWM     1500
#define MAX_STRIKE_PWM     4095
#define SWEEP_STRIKE_MS    150
#define SWEEP_RELEASE_MS   80

// PCA9685 PWM carrier frequency (Hz). 24..1526 range. Default 1500 Hz
// pushes the audible coil hum out of bass range into a high-pitched whine
// (much less obtrusive than the chip's factory default ~200 Hz).
#define DEFAULT_PWM_FREQ_HZ   1500

// Full-power mode: when enabled, every NoteOn drives the channel FULL_ON
// (constant HIGH, no PWM modulation) regardless of MIDI velocity. Loses
// velocity sensitivity but eliminates PWM hum completely AND gives max
// strike force. Toggle live with the "fullpower 0|1" serial command.
// Default ON — PWM mode causes solenoid EMI that crashes the ESP32 on
// some hardware configurations (back-EMF from rapid switching couples into
// ground and locks the CPU before the watchdog can recover). Full Power
// drives constant DC current with no switching, so no EMI. User can opt
// into PWM mode from the web UI if their hardware has proper flyback
// diodes + decoupling + ground separation.
#define DEFAULT_FULL_POWER_MODE  1

// Minimum time between a key's NoteOff and the next NoteOn for that same
// key. Mechanical solenoids need ~20-40 ms to retract; rapid passages in
// Synthesia can re-trigger faster than that and the second strike fails
// because the plunger hasn't reset. If the gap is too short, defer the
// NoteOn so the source music's rhythm "stretches" enough to be physically
// playable. Set 0 to disable (raw passthrough). Tune live via "gap <ms>".
#define DEFAULT_MIN_RETRIGGER_GAP_MS  30

// When a NoteOn is deferred AND the NoteOff arrives before the deferred
// fire time, the original note duration is shorter than what we can
// physically produce. We give the deferred strike this minimum playtime
// before auto-releasing, so the note is at least audible.
#define DEFERRED_STRIKE_MIN_MS  50

// Minimum time a solenoid must be energized before we honor a NoteOff.
// Real piano solenoids need ~30-60 ms of full power to fully pull the
// plunger in and produce an audible strike. If the source's NoteOff
// arrives before this elapses, the release is deferred to (onAt + this).
// Without this, very short MIDI notes (~10 ms) trigger NoteOn → NoteOff
// faster than the plunger can move and produce no sound at all.
// Set 0 to disable. Tune live via the "minstrike <ms>" serial command.
#define DEFAULT_MIN_STRIKE_MS  60
// Isolated-note strike boost: a lone short note (after > DEFAULT_ISO_GAP_MS of
// silence) is stretched to at least DEFAULT_ISO_STRIKE_MS so it's clearly
// audible; notes inside a run keep the shorter DEFAULT_MIN_STRIKE_MS.
#define DEFAULT_ISO_STRIKE_MS  95
#define DEFAULT_ISO_GAP_MS     180

// --- Soft release / anti-clank ---------------------------------------------
// On NoteOff, instead of cutting the coil to 0 instantly (the retract spring
// then SLAMS the plunger into its backstop → the "clank" you hear on the
// piano), hold a reduced "cushion" current briefly. The partial magnetic
// pull opposes the spring, easing the plunger back so it lands softly.
//
//   DEFAULT_SOFT_RELEASE = master on/off (1 = anti-clank enabled)
//   DEFAULT_RELEASE_PWM  = cushion current (12-bit). MUST be low enough that
//                          it does NOT hold the key pressed — just brakes the
//                          retract. ~25-30% of full (1000-1200) is typical.
//   DEFAULT_RELEASE_MS   = how long to apply the cushion before cutting to 0.
//                          Keep short (15-30 ms) so release still feels crisp.
// Tune live via "softrelease 0|1", "releasepwm <n>", "releasems <n>".
//
// NOTE: the cushion is a brief partial-PWM pulse, so on the non-isolated
// board it adds a little switching per release. If it worsens PWM-mode
// stability, disable with "softrelease 0".
#define DEFAULT_SOFT_RELEASE   1
#define DEFAULT_RELEASE_PWM    1100
#define DEFAULT_RELEASE_MS     22

// --- Auto re-strike (tremolo sustain) --------------------------------------
// A piano key only makes sound when STRUCK; holding it down is silent. So a
// long/sustained MIDI note strikes once and decays, sounding quieter than a
// rapidly-repeated key. Auto re-strike re-hits a held note every
// DEFAULT_RESTRIKE_MS so it stays present. The solenoid lifts for
// DEFAULT_RESTRIKE_LIFT_MS between hits so the hammer/jack resets.
// 0 = off (default). ~90-140 ms interval ≈ a musical tremolo.
#define DEFAULT_RESTRIKE_MS       0
#define DEFAULT_RESTRIKE_LIFT_MS  35
// NOTE: a held/tremoloing note's max continuous-drive window is bounded by
// the SAME runtime-clamped g_maxHoldMs (≤ MAX_HOLD_CEILING_MS = 4 s) as
// every other path — enforced in tickRestrike() from heldSinceMs. There is
// deliberately no separate, larger re-strike timeout (an earlier version had
// an 8 s constant that doubled the thermal budget — removed).

enum LedMode : uint8_t {
    LED_MODE_OFF = 0,
    LED_MODE_STATIC,
    LED_MODE_RAINBOW,
    LED_MODE_NOTE_REACTIVE,
    LED_MODE_COUNT
};

// Which note sources are allowed to drive the solenoids. Selectable from
// the pull-down "Input mode" dropdown on the AMOLED.
enum InputMode : uint8_t {
    INPUT_MODE_BLE_ONLY = 0,   // BLE MIDI only (default — touch ignored)
    INPUT_MODE_TOUCH_ONLY,     // touchscreen piano only (BLE ignored)
    INPUT_MODE_BOTH,           // both sources play
    INPUT_MODE_COUNT
};

struct AppState {
    bool bleConnected;
    String peerName;
    uint32_t notesReceived;
    uint8_t lastNote;
    uint8_t lastVelocity;

    LedMode ledMode;
    uint8_t ledBrightness;     // WS2812B strip, 0-255
    uint8_t screenBrightness;  // AMOLED panel, 0-255
    uint32_t ledStaticColor;   // 0xRRGGBB
    uint8_t rainbowSpeed;      // 1..20  (hue increment per frame)
    uint8_t noteDecayRate;     // 1..20  (heat fade per frame in note-reactive)

    // Physical strip layout + note-reactive palette (see DEFAULT_LED_* above)
    uint16_t ledCount;          // physical LEDs on the strip (<= LED_COUNT)
    int16_t  ledOffset;         // shift the note→LED mapping (+/-), fine per-light
    uint16_t ledScalePct;       // note→LED spread gain, 10..400 % (drift trim)
    uint8_t  ledTail;           // LEDs past the last solenoid key — kept dark
    bool     ledReverse;        // strip runs high-note → low-note
    uint8_t  ledReactivePalette;// 0..REACT_PALETTE_COUNT-1
    uint8_t  ledGlow;           // 0..10 neighbours lit each side of a note
    bool     ledVelBright;      // note brightness follows velocity

    InputMode inputMode;       // which sources are allowed to fire solenoids
    uint8_t   touchVelocity;   // 1..127, used when on-screen keys are tapped
};

extern AppState appState;

// Implemented in main.cpp — called by the UI when the screen brightness
// slider moves.
void apply_screen_brightness(uint8_t value);

// Single funnel for enqueueing note events from any source. UI taps and
// BLE MIDI callbacks both call this. Filters by appState.inputMode and
// (if accepted) pushes onto the same queue main loop drains. Defined in
// main.cpp.
//   on:       1 = NoteOn, 0 = NoteOff
//   note:     MIDI note number (21..108)
//   velocity: 1..127 for NoteOn; ignored for NoteOff
//   from_ble: true if originating from BLE MIDI callback, false if touch
void note_input_enqueue(uint8_t on, uint8_t note, uint8_t velocity, bool from_ble);
