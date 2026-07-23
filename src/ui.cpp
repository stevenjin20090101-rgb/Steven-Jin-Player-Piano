// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#include "ui.h"
#include "config.h"
#include "power_boards.h"   // g_pwmFreqHz, g_fullPowerMode, setAllBoardsPWMFreq

#include <lvgl.h>
#include <Arduino.h>

extern AppState appState;

// ---- Layout constants ------------------------------------------------------
// Screen is 450w × 600h after setRotation(1) on the LilyGo T4-S3 AMOLED 2.41"
static constexpr int SCREEN_W       = 450;
static constexpr int SCREEN_H       = 600;
static constexpr int MAIN_PAD_SIDE  = 8;
static constexpr int MAIN_PAD_TOP   = 22;
static constexpr int MAIN_PAD_BOT   = 8;
static constexpr int MAIN_INNER_W   = SCREEN_W - 2 * MAIN_PAD_SIDE;          // 434
static constexpr int MAIN_INNER_H   = SCREEN_H - MAIN_PAD_TOP - MAIN_PAD_BOT;// 570

// Stripped-down panel: touchscreen owns brightness + Full Power switch only.
// Input mode, touch velocity, PWM frequency, retrigger gap, min strike,
// velocity multiplier, per-key force all moved to the web UI (see
// gui/piano-control.html). Touchscreen is for "while playing" toggles;
// the web UI is for sit-down tuning sessions on the Mac.
static constexpr int CTRL_PANEL_H = 240;
static constexpr int HANDLE_PEEK  = 14;

// ---- Widget handles --------------------------------------------------------
static lv_obj_t *scr;
static lv_obj_t *main_view;
static lv_obj_t *handle_tab;

static lv_obj_t *lbl_status;
static lv_obj_t *lbl_peer;
static lv_obj_t *lbl_notes;
static lv_obj_t *lbl_last_note;
static lv_obj_t *lbl_i2c;

static constexpr int NOTE_COUNT = MIDI_NOTE_MAX - MIDI_NOTE_MIN + 1;

// Piano keyboard
static lv_obj_t *key_objs[NOTE_COUNT];
static int16_t  key_x_arr[NOTE_COUNT];   // x offset within piano/waterfall
static int16_t  key_w_arr[NOTE_COUNT];

// Waterfall — pool of pre-allocated rectangles. NoteOn picks a free slot,
// NoteOff releases the slot's "held" flag and the bar floats off-screen.
static constexpr int MAX_BARS    = 16;                                // was 24
static constexpr int PIANO_H     = 80;
static constexpr int PIANO_BLACK_H = 50;
static constexpr int MODE_STRIP_H = 80;
// Bottom strip holds the 3 LED-mode icon buttons; piano sits directly above.
static constexpr int MODE_STRIP_Y = MAIN_INNER_H - MODE_STRIP_H;      // 490
static constexpr int PIANO_Y      = MODE_STRIP_Y - PIANO_H - 8;       // 402
// Shorter waterfall = smaller dirty rect per scroll tick = much less render
// work during fast passages. Bars also exit the pool quicker, so MAX_BARS
// can be smaller too.
static constexpr int WATERFALL_H  = 180;                              // was ~298
static constexpr int WATERFALL_Y  = PIANO_Y - WATERFALL_H;            // 222
static constexpr int SCROLL_PX_PER_TICK = 5;     // ~125 px/sec at 25fps
static constexpr uint32_t STUCK_KEY_MS = 5000;

struct Bar {
    bool      in_use;
    bool      held;
    uint8_t   note;
    uint32_t  start_ms;
    int16_t   x;
    int16_t   width;
    int16_t   y_top;
    int16_t   y_bottom;
};
static Bar bars[MAX_BARS];
static lv_obj_t *bar_objs[MAX_BARS];
static int8_t held_slot[NOTE_COUNT];   // -1 if note has no currently-held bar
static lv_obj_t *waterfall_box;

static bool is_white_key(uint8_t note) {
    static const bool white[12] = {
        true, false, true, false, true, true, false, true, false, true, false, true
    };
    return white[note % 12];
}

static lv_obj_t *mode_btn_objs[3];

static lv_obj_t *cfg_popup;
static lv_obj_t *cfg_title;
static lv_obj_t *cfg_color_row;
static lv_obj_t *cfg_speed_row;
static lv_obj_t *cfg_speed_lbl_caption;   // "Speed" / "Decay"
static lv_obj_t *cfg_speed_slider;
static lv_obj_t *cfg_speed_pct_lbl;
static lv_obj_t *cfg_led_slider;
static lv_obj_t *cfg_led_pct_lbl;
static lv_obj_t *cfg_colorwheel;
static lv_obj_t *cfg_color_preview;

static lv_obj_t *ctrl_panel;
static lv_obj_t *screen_slider;
static lv_obj_t *screen_pct_lbl;
static lv_obj_t *led_slider;
static lv_obj_t *led_pct_lbl;
// Tracked so ui_tick() can sync it to g_fullPowerMode after the web UI
// flips the global from a serial command.
static lv_obj_t *s_fullpower_switch = nullptr;

// Waterfall removed — flag retained for NVS-compat only, never read.
bool g_waterfallEnabled = false;
// Piano keyboard highlight on/off. Each NoteOn/Off recolors a key —
// cheap individually but accumulates under dense MIDI. Toggle off to
// skip all key recolors and save the corresponding LVGL invalidations.
bool g_keyboardVizEnabled = true;

struct ModeInfo {
    const char *symbol;
    const char *name;
    LedMode mode;
};
static const ModeInfo kModes[3] = {
    { LV_SYMBOL_TINT,    "Static",        LED_MODE_STATIC        },
    { LV_SYMBOL_REFRESH, "Rainbow",       LED_MODE_RAINBOW       },
    { LV_SYMBOL_AUDIO,   "Note-reactive", LED_MODE_NOTE_REACTIVE },
};

static bool ctrl_panel_open = false;

// ---- Palette ---------------------------------------------------------------
static const struct {
    const char *name;
    uint32_t rgb;
} kPalette[] = {
    {"Red",    0xFF2020}, {"Orange", 0xFF8000}, {"Yellow", 0xFFD000},
    {"Green",  0x20FF40}, {"Cyan",   0x00E0FF}, {"Blue",   0x2040FF},
    {"Purple", 0xA020FF}, {"White",  0xFFFFFF},
};
static const int kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

// ---- Helpers ---------------------------------------------------------------
static const char *noteName(uint8_t note) {
    static const char *names[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    snprintf(buf, sizeof(buf), "%s%d", names[note % 12], (note / 12) - 1);
    return buf;
}

static void slide_panel_to(int target_y) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctrl_panel);
    lv_anim_set_time(&a, 260);
    lv_anim_set_values(&a, lv_obj_get_y(ctrl_panel), target_y);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void set_ctrl_panel_open(bool open) {
    if (ctrl_panel_open == open) return;
    ctrl_panel_open = open;
    int shown = 0;
    int hidden = -(CTRL_PANEL_H - HANDLE_PEEK);
    slide_panel_to(open ? shown : hidden);
}

// ---- Event callbacks -------------------------------------------------------
static void handle_tab_cb(lv_event_t *e) {
    set_ctrl_panel_open(!ctrl_panel_open);
}

static void screen_gesture_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_BOTTOM && !ctrl_panel_open) {
        set_ctrl_panel_open(true);
    } else if (dir == LV_DIR_TOP && ctrl_panel_open) {
        set_ctrl_panel_open(false);
    }
}

static void screen_slider_cb(lv_event_t *e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    apply_screen_brightness((uint8_t)v);
    lv_label_set_text_fmt(screen_pct_lbl, "%d%%", (v * 100) / 255);
}

static void led_slider_cb(lv_event_t *e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    appState.ledBrightness = (uint8_t)v;
    lv_label_set_text_fmt(led_pct_lbl, "%d%%", (v * 100) / 255);
}

static void apply_mode_visuals() {
    for (int i = 0; i < 3; ++i) {
        if (!mode_btn_objs[i]) continue;
        bool sel = (appState.ledMode == kModes[i].mode);
        lv_obj_set_style_border_width(mode_btn_objs[i], sel ? 3 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(mode_btn_objs[i], lv_color_hex(0x80C0FF), LV_PART_MAIN);
        lv_obj_set_style_bg_color(mode_btn_objs[i],
            sel ? lv_color_hex(0x383848) : lv_color_hex(0x282830), LV_PART_MAIN);
    }
}

static void show_cfg_popup(int mode_idx) {
    if (!cfg_popup) return;
    lv_label_set_text(cfg_title, kModes[mode_idx].name);
    LedMode m = kModes[mode_idx].mode;
    if (m == LED_MODE_STATIC) {
        lv_obj_clear_flag(cfg_color_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cfg_speed_row, LV_OBJ_FLAG_HIDDEN);
        if (cfg_colorwheel) {
            lv_obj_clear_flag(cfg_colorwheel, LV_OBJ_FLAG_HIDDEN);
            lv_color_t cur = lv_color_hex(appState.ledStaticColor);
            lv_colorwheel_set_rgb(cfg_colorwheel, cur);
            if (cfg_color_preview) {
                lv_obj_set_style_bg_color(cfg_color_preview, cur, LV_PART_MAIN);
            }
        }
    } else {
        lv_obj_add_flag(cfg_color_row, LV_OBJ_FLAG_HIDDEN);
        if (cfg_colorwheel) lv_obj_add_flag(cfg_colorwheel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cfg_speed_row, LV_OBJ_FLAG_HIDDEN);
        if (m == LED_MODE_RAINBOW) {
            lv_label_set_text(cfg_speed_lbl_caption, "Speed");
            lv_slider_set_value(cfg_speed_slider, appState.rainbowSpeed, LV_ANIM_OFF);
            lv_label_set_text_fmt(cfg_speed_pct_lbl, "%d", appState.rainbowSpeed);
        } else {
            lv_label_set_text(cfg_speed_lbl_caption, "Decay");
            lv_slider_set_value(cfg_speed_slider, appState.noteDecayRate, LV_ANIM_OFF);
            lv_label_set_text_fmt(cfg_speed_pct_lbl, "%d", appState.noteDecayRate);
        }
    }
    lv_slider_set_value(cfg_led_slider, appState.ledBrightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(cfg_led_pct_lbl, "%d%%", (appState.ledBrightness * 100) / 255);
    lv_obj_clear_flag(cfg_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(cfg_popup);
}

static bool popup_open() {
    return cfg_popup && !lv_obj_has_flag(cfg_popup, LV_OBJ_FLAG_HIDDEN);
}

static void mode_btn_clicked_cb(lv_event_t *e) {
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    appState.ledMode = kModes[idx].mode;
    apply_mode_visuals();
    // If the config popup is already open, swap its contents to the new mode
    // so the user can't end up looking at config for a mode that isn't active.
    if (popup_open()) show_cfg_popup(idx);
}

static void mode_btn_longpress_cb(lv_event_t *e) {
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    appState.ledMode = kModes[idx].mode;
    apply_mode_visuals();
    show_cfg_popup(idx);
}

static void cfg_colorwheel_cb(lv_event_t *e) {
    lv_color_t c = lv_colorwheel_get_rgb(lv_event_get_target(e));
    appState.ledStaticColor = lv_color_to32(c) & 0xFFFFFF;
    if (cfg_color_preview) {
        lv_obj_set_style_bg_color(cfg_color_preview, c, LV_PART_MAIN);
    }
}

static void cfg_close_cb(lv_event_t *e) {
    lv_obj_add_flag(cfg_popup, LV_OBJ_FLAG_HIDDEN);
}

static void cfg_speed_slider_cb(lv_event_t *e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (appState.ledMode == LED_MODE_RAINBOW) {
        appState.rainbowSpeed = (uint8_t)v;
    } else {
        appState.noteDecayRate = (uint8_t)v;
    }
    lv_label_set_text_fmt(cfg_speed_pct_lbl, "%d", v);
}

static void cfg_led_slider_cb(lv_event_t *e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    appState.ledBrightness = (uint8_t)v;
    lv_label_set_text_fmt(cfg_led_pct_lbl, "%d%%", (v * 100) / 255);
    // Mirror the change into the Control Center slider so both stay in sync
    if (led_slider) lv_slider_set_value(led_slider, v, LV_ANIM_OFF);
    if (led_pct_lbl) lv_label_set_text_fmt(led_pct_lbl, "%d%%", (v * 100) / 255);
}

static void color_btn_cb(lv_event_t *e) {
    uint32_t rgb = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    appState.ledStaticColor = rgb;
    lv_color_t c = lv_color_hex(rgb);
    if (cfg_color_preview) {
        lv_obj_set_style_bg_color(cfg_color_preview, c, LV_PART_MAIN);
    }
    if (cfg_colorwheel) {
        lv_colorwheel_set_rgb(cfg_colorwheel, c);
    }
}

// ---- Build the main view ---------------------------------------------------
static lv_obj_t *make_plain_container(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_status_block(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Steven's Piano");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_status = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 0, 42);

    lbl_peer = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_peer, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(lbl_peer, LV_ALIGN_TOP_LEFT, 0, 62);

    lbl_last_note = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_last_note, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_label_set_text(lbl_last_note, "Last: --");
    lv_obj_align(lbl_last_note, LV_ALIGN_TOP_LEFT, 0, 82);

    lbl_i2c = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_i2c, lv_color_hex(0x80B0E0), LV_PART_MAIN);
    lv_label_set_long_mode(lbl_i2c, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_i2c, MAIN_INNER_W);
    lv_label_set_text(lbl_i2c, "I2C: scanning...");
    lv_obj_align(lbl_i2c, LV_ALIGN_TOP_LEFT, 0, 102);

    lbl_notes = nullptr;   // removed from UI — kept in AppState for future use
}

// Common width — both the piano and the waterfall live in containers of
// this exact width so bar X positions can index into key_x_arr directly.
static int g_keyboard_w = 0;

static void compute_key_geometry() {
    const int total_w = MAIN_INNER_W;   // 434 on the 2.41" AMOLED in portrait
    int white_count = 0;
    for (int n = MIDI_NOTE_MIN; n <= MIDI_NOTE_MAX; ++n) {
        if (is_white_key(n)) ++white_count;
    }
    const int white_w = total_w / white_count;
    const int black_w = (white_w * 60) / 100;
    g_keyboard_w = white_w * white_count;

    int wi = 0;
    for (int n = MIDI_NOTE_MIN; n <= MIDI_NOTE_MAX; ++n) {
        int idx = n - MIDI_NOTE_MIN;
        if (is_white_key(n)) {
            key_x_arr[idx] = wi * white_w;
            key_w_arr[idx] = white_w - 1;
            ++wi;
        } else {
            // black key sits centered over the boundary between the previous
            // white key (wi-1) and the next (wi)
            key_x_arr[idx] = wi * white_w - black_w / 2;
            key_w_arr[idx] = black_w;
        }
    }
}

// ---- Swipe-to-play piano ---------------------------------------------------
//
// Old: each key was an individual clickable LVGL object. That meant when you
// pressed key A and slid your finger onto key B, B never received a press
// event (the original press was "owned" by A), so you could only play one
// key at a time with discrete taps.
//
// New: the piano BOX itself is the touch target. Individual key objects
// still exist for visual highlighting but they're non-clickable. On every
// PRESSING tick we read the absolute touch coords, figure out which key is
// under the finger, and fire NoteOn/Off transitions as the active note
// changes. Sliding across the keys → notes play in sequence (glissando).
//
// Limitation: single touch only (LVGL default). For chords by tapping
// multiple keys simultaneously, route through BLE/Synthesia.

static lv_obj_t *s_piano_box       = nullptr;
static int       s_active_touch_note = -1;   // -1 = no key held

// Given (rel_x, rel_y) inside the piano box, return MIDI note under it,
// or -1 if outside any key. Black keys are checked first because they
// overlay white keys in the top portion (rel_y < PIANO_BLACK_H).
static int point_to_note(int rel_x, int rel_y) {
    if (rel_y < 0 || rel_y >= PIANO_H) return -1;
    if (rel_y < PIANO_BLACK_H) {
        for (int n = MIDI_NOTE_MIN; n <= MIDI_NOTE_MAX; ++n) {
            if (is_white_key(n)) continue;
            int idx = n - MIDI_NOTE_MIN;
            if (rel_x >= key_x_arr[idx] &&
                rel_x <  key_x_arr[idx] + key_w_arr[idx]) return n;
        }
    }
    for (int n = MIDI_NOTE_MIN; n <= MIDI_NOTE_MAX; ++n) {
        if (!is_white_key(n)) continue;
        int idx = n - MIDI_NOTE_MIN;
        if (rel_x >= key_x_arr[idx] &&
            rel_x <  key_x_arr[idx] + key_w_arr[idx]) return n;
    }
    return -1;
}

static void piano_touch_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev || !s_piano_box) return;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);

        lv_area_t a;
        lv_obj_get_coords(s_piano_box, &a);
        int rel_x = pt.x - a.x1;
        int rel_y = pt.y - a.y1;

        int note = point_to_note(rel_x, rel_y);
        if (note != s_active_touch_note) {
            if (s_active_touch_note != -1) {
                note_input_enqueue(0, (uint8_t)s_active_touch_note, 0, /*from_ble=*/false);
            }
            if (note != -1) {
                note_input_enqueue(1, (uint8_t)note,
                                   appState.touchVelocity, /*from_ble=*/false);
            }
            s_active_touch_note = note;
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_active_touch_note != -1) {
            note_input_enqueue(0, (uint8_t)s_active_touch_note, 0, /*from_ble=*/false);
            s_active_touch_note = -1;
        }
    }
}

static void build_piano(lv_obj_t *parent) {
    s_piano_box = lv_obj_create(parent);
    lv_obj_set_size(s_piano_box, g_keyboard_w, PIANO_H);
    lv_obj_align(s_piano_box, LV_ALIGN_TOP_MID, 0, PIANO_Y);
    lv_obj_set_style_bg_color(s_piano_box, lv_color_hex(0x303038), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_piano_box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_piano_box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_piano_box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_piano_box, LV_OBJ_FLAG_SCROLLABLE);

    // Box owns the touch — children below are non-clickable so events fall
    // through here. PRESSING fires continuously while finger is down so we
    // can detect when it crosses from one key to another.
    lv_obj_add_flag(s_piano_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_piano_box, piano_touch_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(s_piano_box, piano_touch_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(s_piano_box, piano_touch_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(s_piano_box, piano_touch_cb, LV_EVENT_PRESS_LOST, NULL);

    // White keys first so black keys overlay them. Keys are non-clickable
    // visual elements — color changes when ui_set_key() is called.
    for (int n = MIDI_NOTE_MIN; n <= MIDI_NOTE_MAX; ++n) {
        if (!is_white_key(n)) continue;
        int idx = n - MIDI_NOTE_MIN;
        lv_obj_t *k = lv_obj_create(s_piano_box);
        lv_obj_set_size(k, key_w_arr[idx], PIANO_H);
        lv_obj_set_pos(k, key_x_arr[idx], 0);
        lv_obj_set_style_bg_color(k, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(k, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(k, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(k, 0, LV_PART_MAIN);
        lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        key_objs[idx] = k;
    }
    for (int n = MIDI_NOTE_MIN; n <= MIDI_NOTE_MAX; ++n) {
        if (is_white_key(n)) continue;
        int idx = n - MIDI_NOTE_MIN;
        lv_obj_t *k = lv_obj_create(s_piano_box);
        lv_obj_set_size(k, key_w_arr[idx], PIANO_BLACK_H);
        lv_obj_set_pos(k, key_x_arr[idx], 0);
        lv_obj_set_style_bg_color(k, lv_color_hex(0x101015), LV_PART_MAIN);
        lv_obj_set_style_border_width(k, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(k, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(k, 0, LV_PART_MAIN);
        lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        key_objs[idx] = k;
    }
}

// ---- MIDI text log (replaces the old waterfall) ---------------------------
//
// Cheap log of the last 6 NoteOn events as plain text in the area the
// waterfall used to occupy. Single LVGL label that gets its text rewritten
// each time a note arrives — no per-frame animation, no allocations, no
// background ticking, no per-bar object pool. Render cost is ~1 label
// invalidation per NoteOn, vs the old waterfall's MAX_BARS animations
// per LVGL frame.

static lv_obj_t *s_midi_log_lbl = nullptr;
static constexpr int MIDI_LOG_LEN = 6;
static char s_midi_log_buf[MIDI_LOG_LEN][12] = {{0}};
static int  s_midi_log_head = 0;

static const char *midi_note_name(uint8_t note) {
    static const char *names[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    snprintf(buf, sizeof(buf), "%s%d", names[note % 12], (note / 12) - 1);
    return buf;
}

static void midi_log_append(uint8_t note, uint8_t velocity) {
    if (!s_midi_log_lbl) return;
    snprintf(s_midi_log_buf[s_midi_log_head], sizeof(s_midi_log_buf[0]),
             "%s v%u", midi_note_name(note), (unsigned)velocity);
    s_midi_log_head = (s_midi_log_head + 1) % MIDI_LOG_LEN;

    // Rebuild the display string: newest first, top-down
    char display[MIDI_LOG_LEN * 14 + 1];
    int pos = 0;
    for (int i = 0; i < MIDI_LOG_LEN; ++i) {
        int idx = (s_midi_log_head - 1 - i + MIDI_LOG_LEN) % MIDI_LOG_LEN;
        if (s_midi_log_buf[idx][0] == 0) continue;
        pos += snprintf(display + pos, sizeof(display) - pos,
                        "%s%s", (pos > 0 ? "\n" : ""), s_midi_log_buf[idx]);
    }
    lv_label_set_text(s_midi_log_lbl, display);
}

static void build_midi_log(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, g_keyboard_w, WATERFALL_H);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, WATERFALL_Y);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x080810), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 8, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Last notes");
    lv_obj_set_style_text_color(title, lv_color_hex(0x6080A0), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_midi_log_lbl = lv_label_create(box);
    lv_label_set_text(s_midi_log_lbl, "—");
    lv_obj_set_style_text_color(s_midi_log_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_midi_log_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(s_midi_log_lbl, LV_ALIGN_TOP_LEFT, 0, 20);
}

// Old waterfall hooks kept as no-op stubs so ui_set_key compiles unchanged.
// All the bars[], held_slot[], waterfall_box, animation, and bar pool
// management has been deleted — zero background processing now.
static inline void waterfall_spawn(uint8_t note)   { (void)note; }
static inline void waterfall_release(uint8_t note) { (void)note; }

static void build_mode_strip(lv_obj_t *parent) {
    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_set_size(strip, MAIN_INNER_W, MODE_STRIP_H);
    lv_obj_align(strip, LV_ALIGN_TOP_MID, 0, MODE_STRIP_Y);
    lv_obj_set_style_bg_color(strip, lv_color_hex(0x18181E), LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(strip, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(strip, 6, LV_PART_MAIN);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    const int btn_w = 124;
    const int btn_h = 64;
    const int gap   = 8;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *btn = lv_obj_create(strip);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_align(btn, LV_ALIGN_CENTER, (i - 1) * (btn_w + gap), 0);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, mode_btn_clicked_cb,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(btn, mode_btn_longpress_cb,
                            LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)i);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, kModes[i].symbol);
        lv_obj_set_style_text_color(icon, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -8);

        lv_obj_t *name = lv_label_create(btn);
        lv_label_set_text(name, kModes[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(0xC0C0CC), LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);

        mode_btn_objs[i] = btn;
    }
    apply_mode_visuals();
}

// ---- Slider row helper (used in Control Center + config popup) -------------
// Thicker bar + bigger knob so it's comfortable on touchscreen.
enum SliderFmt { SLIDER_FMT_PCT, SLIDER_FMT_INT };

static lv_obj_t *make_slider_row(lv_obj_t *parent, int y,
                                 const char *icon, int min_v, int max_v,
                                 int initial, SliderFmt fmt,
                                 lv_event_cb_t cb,
                                 lv_obj_t **out_slider,
                                 lv_obj_t **out_pct_lbl) {
    lv_obj_t *row = make_plain_container(parent);
    lv_obj_set_size(row, lv_pct(100), 64);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *icon_lbl = lv_label_create(row);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_slider_set_range(slider, min_v, max_v);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);
    lv_obj_set_size(slider, lv_pct(65), 24);             // thicker bar
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 100, 0);
    lv_obj_set_style_radius(slider, 14, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 14, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 18, LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x40404A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB); // bigger thumb
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *pct = lv_label_create(row);
    if (fmt == SLIDER_FMT_PCT) {
        lv_label_set_text_fmt(pct, "%d%%", (initial * 100) / (max_v ? max_v : 1));
    } else {
        lv_label_set_text_fmt(pct, "%d", initial);
    }
    lv_obj_set_style_text_color(pct, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_RIGHT_MID, 0, 0);

    *out_slider = slider;
    *out_pct_lbl = pct;
    return row;
}

// Backwards-compatible wrapper for the existing 0..255 brightness rows
static lv_obj_t *make_brightness_row(lv_obj_t *parent, int y,
                                     const char *icon, uint8_t initial,
                                     lv_event_cb_t cb,
                                     lv_obj_t **out_slider,
                                     lv_obj_t **out_pct_lbl) {
    return make_slider_row(parent, y, icon, 8, 255, initial,
                           SLIDER_FMT_PCT, cb, out_slider, out_pct_lbl);
}

static void build_cfg_popup() {
    cfg_popup = lv_obj_create(scr);
    lv_obj_set_size(cfg_popup, 410, 540);     // fits within 450x600 screen
    lv_obj_center(cfg_popup);
    lv_obj_set_style_bg_color(cfg_popup, lv_color_hex(0x1A1A24), LV_PART_MAIN);
    lv_obj_set_style_border_width(cfg_popup, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(cfg_popup, lv_color_hex(0x404048), LV_PART_MAIN);
    lv_obj_set_style_radius(cfg_popup, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cfg_popup, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cfg_popup, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cfg_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cfg_popup, LV_OBJ_FLAG_HIDDEN);

    cfg_title = lv_label_create(cfg_popup);
    lv_label_set_text(cfg_title, "Mode");
    lv_obj_set_style_text_color(cfg_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cfg_title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(cfg_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close_btn = lv_obj_create(cfg_popup);
    lv_obj_set_size(close_btn, 44, 44);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_radius(close_btn, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x303038), LV_PART_MAIN);
    lv_obj_set_style_border_width(close_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(close_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, cfg_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(close_btn);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(xl);

    // Color palette row (visible only for Static)
    cfg_color_row = make_plain_container(cfg_popup);
    lv_obj_set_size(cfg_color_row, lv_pct(100), 68);
    lv_obj_align(cfg_color_row, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_set_flex_flow(cfg_color_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cfg_color_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < kPaletteSize; ++i) {
        lv_obj_t *btn = lv_btn_create(cfg_color_row);
        lv_obj_set_size(btn, 50, 50);
        lv_obj_set_style_radius(btn, 25, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kPalette[i].rgb), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, color_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)kPalette[i].rgb);
    }

    // Speed/Decay slider (visible for Rainbow/Note-reactive)
    cfg_speed_row = make_slider_row(cfg_popup, 56, "Speed",
                                    1, 20, appState.rainbowSpeed,
                                    SLIDER_FMT_INT, cfg_speed_slider_cb,
                                    &cfg_speed_slider, &cfg_speed_pct_lbl);
    cfg_speed_lbl_caption = lv_obj_get_child(cfg_speed_row, 0);

    // Color wheel (Static only) — sits below the palette row
    cfg_colorwheel = lv_colorwheel_create(cfg_popup, true);
    lv_obj_set_size(cfg_colorwheel, 220, 220);
    lv_obj_align(cfg_colorwheel, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_add_event_cb(cfg_colorwheel, cfg_colorwheel_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    // Center swatch showing the currently selected color
    cfg_color_preview = lv_obj_create(cfg_colorwheel);
    lv_obj_set_size(cfg_color_preview, 90, 90);
    lv_obj_center(cfg_color_preview);
    lv_obj_set_style_radius(cfg_color_preview, 45, LV_PART_MAIN);
    lv_obj_set_style_border_width(cfg_color_preview, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(cfg_color_preview, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(cfg_color_preview,
                              lv_color_hex(appState.ledStaticColor), LV_PART_MAIN);
    lv_obj_set_style_pad_all(cfg_color_preview, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cfg_color_preview,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // LED brightness slider (always)
    lv_obj_t *bri_row = make_brightness_row(cfg_popup, 380, "LEDs",
                                            appState.ledBrightness,
                                            cfg_led_slider_cb,
                                            &cfg_led_slider, &cfg_led_pct_lbl);
    (void)bri_row;

    // Hint line at the bottom
    lv_obj_t *hint = lv_label_create(cfg_popup);
    lv_label_set_text(hint, "Tap X to close");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x707080), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void build_control_panel() {
    ctrl_panel = lv_obj_create(scr);
    lv_obj_set_size(ctrl_panel, lv_pct(100), CTRL_PANEL_H);
    lv_obj_set_pos(ctrl_panel, 0, -(CTRL_PANEL_H - HANDLE_PEEK));
    lv_obj_set_style_bg_color(ctrl_panel, lv_color_hex(0x202028), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctrl_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctrl_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ctrl_panel, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctrl_panel, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ctrl_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ctrl_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(ctrl_panel);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    make_brightness_row(ctrl_panel, 36, "Screen",
                        appState.screenBrightness, screen_slider_cb,
                        &screen_slider, &screen_pct_lbl);
    make_brightness_row(ctrl_panel, 100, "LEDs",
                        appState.ledBrightness, led_slider_cb,
                        &led_slider, &led_pct_lbl);

    // ---- Full Power switch ------------------------------------------------
    // Only persistent on-touchscreen toggle. Everything else (input mode,
    // touch velocity, PWM freq, retrigger gap, min strike, per-key force,
    // velocity multiplier) is controlled from gui/piano-control.html over
    // serial. ui_tick() polls g_fullPowerMode each frame and syncs this
    // switch when the web UI changes it.
    lv_obj_t *fp_lbl = lv_label_create(ctrl_panel);
    lv_label_set_text(fp_lbl, "Full power (no PWM, max force)");
    lv_obj_set_style_text_color(fp_lbl, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_align(fp_lbl, LV_ALIGN_TOP_LEFT, 0, 168);

    s_fullpower_switch = lv_switch_create(ctrl_panel);
    lv_obj_set_size(s_fullpower_switch, 60, 32);
    lv_obj_align(s_fullpower_switch, LV_ALIGN_TOP_RIGHT, 0, 164);
    if (g_fullPowerMode) lv_obj_add_state(s_fullpower_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_fullpower_switch, [](lv_event_t *e) {
        lv_obj_t *s = lv_event_get_target(e);
        g_fullPowerMode = lv_obj_has_state(s, LV_STATE_CHECKED);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Pull-tab at the bottom edge of the panel — always visible (peeks below
    // the panel when collapsed). Tap to toggle.
    handle_tab = lv_obj_create(ctrl_panel);
    lv_obj_set_size(handle_tab, 80, 8);
    lv_obj_align(handle_tab, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(handle_tab, lv_color_hex(0x707080), LV_PART_MAIN);
    lv_obj_set_style_radius(handle_tab, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(handle_tab, 0, LV_PART_MAIN);
    lv_obj_clear_flag(handle_tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(handle_tab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(handle_tab, handle_tab_cb, LV_EVENT_CLICKED, NULL);
}

// ---- Public API ------------------------------------------------------------
// ---- Splash screen ---------------------------------------------------------
//
// Black overlay shown on top of the main UI for ~3 seconds at boot. Title
// "Player Piano" reveals one character at a time (staggered 60 ms each)
// with a fade-in + slide-up — Material You-style. Subtitle "made by
// Steven Jin" fades in after the title finishes. Whole overlay fades to
// transparent at t=2.5 s and gets deleted at t=3.0 s, revealing the
// already-built main UI underneath.

static lv_obj_t *s_splash = nullptr;

static void splash_anim_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (uint8_t)v, LV_PART_MAIN);
}
static void splash_anim_y(void *obj, int32_t v) {
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void splash_finish_cb(lv_anim_t *) {
    if (s_splash) { lv_obj_del(s_splash); s_splash = nullptr; }
}

static void splash_show(lv_obj_t *parent) {
    s_splash = lv_obj_create(parent);
    lv_obj_set_size(s_splash, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_style_bg_color(s_splash, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa  (s_splash, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_splash, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_splash, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Title container — flex-row so per-character labels lay out naturally
    lv_obj_t *title_row = lv_obj_create(s_splash);
    lv_obj_set_size(title_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(title_row, LV_ALIGN_CENTER, 0, -28);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(title_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(title_row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    const char *title = "Player Piano";
    for (int i = 0; title[i]; ++i) {
        char buf[2] = { title[i], 0 };
        lv_obj_t *lbl = lv_label_create(title_row);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(lbl, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(lbl, LV_OPA_TRANSP, LV_PART_MAIN);

        // Fade in + slide up. Stagger 60 ms per character.
        const uint32_t delay = (uint32_t)i * 60;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, lbl);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_time(&a, 400);
        lv_anim_set_delay(&a, delay);
        lv_anim_set_exec_cb(&a, splash_anim_opa);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    // Subtitle
    lv_obj_t *sub = lv_label_create(s_splash);
    lv_label_set_text(sub, "made by Steven Jin");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xA0A8B8), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_opa(sub, LV_OPA_TRANSP, LV_PART_MAIN);

    // Subtitle fades in after the title's last character settles
    const uint32_t subDelay = (uint32_t)strlen(title) * 60 + 300;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sub);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, 500);
    lv_anim_set_delay(&a, subDelay);
    lv_anim_set_exec_cb(&a, splash_anim_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    // Whole-overlay fade out: bg goes transparent so the live main UI shows
    // through. Animation completion callback deletes the overlay.
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_splash);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 500);
    lv_anim_set_delay(&a, 2500);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_style_bg_opa((lv_obj_t *)obj, (uint8_t)v, LV_PART_MAIN);
    });
    lv_anim_set_ready_cb(&a, splash_finish_cb);
    lv_anim_start(&a);
}

void ui_init() {
    scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101015), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    main_view = make_plain_container(scr);
    lv_obj_set_size(main_view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_left  (main_view, MAIN_PAD_SIDE, LV_PART_MAIN);
    lv_obj_set_style_pad_right (main_view, MAIN_PAD_SIDE, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(main_view, MAIN_PAD_BOT,  LV_PART_MAIN);
    lv_obj_set_style_pad_top   (main_view, MAIN_PAD_TOP,  LV_PART_MAIN);

    compute_key_geometry();
    build_status_block(main_view);
    // Waterfall removed entirely. The area now hosts a cheap text log of
    // the last few MIDI notes — see midi_log_append() / build_midi_log().
    build_midi_log(main_view);
    build_piano(main_view);
    build_mode_strip(main_view);

    build_control_panel();
    build_cfg_popup();

    // Whole-screen swipe down / up to toggle the control panel.
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    // Splash sits on top of the fully-built main UI and removes itself
    // after ~3 s. Main UI is already live underneath — when the splash
    // bg fades out it reveals the real piano.
    splash_show(scr);
}

void ui_refresh_status() {
    if (!lbl_status) return;
    if (appState.bleConnected) {
        lv_label_set_text(lbl_status, "BLE MIDI: CONNECTED");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x60FF60), LV_PART_MAIN);
        lv_label_set_text_fmt(lbl_peer, "Client: %s", appState.peerName.c_str());
    } else {
        lv_label_set_text(lbl_status, "BLE MIDI: advertising...");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFFB060), LV_PART_MAIN);
        lv_label_set_text_fmt(lbl_peer, "Pair with \"%s\"", BLE_MIDI_DEVICE_NAME);
    }
}

void ui_refresh_note(uint8_t note, uint8_t velocity) {
    if (lbl_last_note) {
        lv_label_set_text_fmt(lbl_last_note, "Last: %s  vel=%u", noteName(note), velocity);
    }
    // Append to the text log (replaces the old waterfall visualization)
    midi_log_append(note, velocity);
}

void ui_set_i2c_info(int count, const uint8_t *addrs) {
    if (!lbl_i2c) return;
    char buf[160];
    int pos = snprintf(buf, sizeof(buf), "I2C (%d): ", count);
    for (int i = 0; i < count && pos < (int)sizeof(buf) - 6; ++i) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02X ", addrs[i]);
    }
    if (count == 0) {
        snprintf(buf, sizeof(buf), "I2C: no devices");
    }
    lv_label_set_text(lbl_i2c, buf);
}

void ui_set_key(uint8_t note, bool pressed) {
    // Keyboard-viz disabled → skip all per-key recolors. Direct CPU + Wire
    // savings under heavy MIDI dispatch.
    if (!g_keyboardVizEnabled) return;
    if (note < MIDI_NOTE_MIN || note > MIDI_NOTE_MAX) return;
    int idx = note - MIDI_NOTE_MIN;
    lv_obj_t *k = key_objs[idx];
    if (!k) return;
    // Track per-key pressed state and skip the LVGL recolor when nothing
    // actually changed. Saves invalidations on duplicate NoteOns (e.g.
    // running-status pedal pumps from some apps).
    static uint8_t state[NOTE_COUNT] = {0};
    if (state[idx] == (pressed ? 1 : 0)) return;
    state[idx] = pressed ? 1 : 0;

    lv_color_t c = pressed ? lv_color_hex(0xFF2030)
                           : (is_white_key(note) ? lv_color_white()
                                                  : lv_color_hex(0x101015));
    lv_obj_set_style_bg_color(k, c, LV_PART_MAIN);
}

void ui_tick() {
    // Live-sync the Full Power switch from g_fullPowerMode. If the web UI
    // sent "fullpower 1" over serial, the global flipped but the switch
    // widget still shows its old state. Reconcile here once per tick so the
    // touchscreen always mirrors the actual setting regardless of source.
    if (s_fullpower_switch) {
        bool sw = lv_obj_has_state(s_fullpower_switch, LV_STATE_CHECKED);
        if (sw != g_fullPowerMode) {
            if (g_fullPowerMode) lv_obj_add_state(s_fullpower_switch, LV_STATE_CHECKED);
            else                 lv_obj_clear_state(s_fullpower_switch, LV_STATE_CHECKED);
        }
    }

    // Waterfall and its per-frame animation deleted entirely. ui_tick is
    // now just the fullpower switch sync above — sub-microsecond per call.
}
