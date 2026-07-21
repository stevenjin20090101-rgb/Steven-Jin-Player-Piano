#pragma once
#include <Arduino.h>

void ui_init();
void ui_refresh_status();
void ui_refresh_note(uint8_t note, uint8_t velocity);
void ui_set_key(uint8_t note, bool pressed);
void ui_set_i2c_info(int count, const uint8_t *addrs);
void ui_tick();

// Waterfall visualization removed. Flag retained for NVS-compat but unused.
extern bool g_waterfallEnabled;

// Piano keyboard highlight toggle. When false, ui_set_key() returns
// immediately and the 88-key visual stays static (white/black colors only,
// no red highlight). Removes ~1 LVGL invalidation per NoteOn/Off — direct
// CPU + Wire savings during heavy MIDI dispatch. Default ON.
extern bool g_keyboardVizEnabled;
