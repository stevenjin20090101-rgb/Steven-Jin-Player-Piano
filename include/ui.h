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

void ui_init();
void ui_refresh_status();
// Puts `text` on the status line in colour 0xRRGGBB instead of the BLE state:
// a firmware update in progress, or its outcome for a minute after
// (docs/BLE_OTA.md sections 7 and 9). ui_refresh_status() puts the BLE state
// back. Like every ui_* call: inside loop()'s ui_lock block only.
void ui_set_status_text(const char *text, uint32_t rgb);
// The settings popup's "Let a new tablet pair" was tapped since the last call
// (docs/BLE_LOCK.md). Loop task, outside ui_lock: the LVGL callback only
// raises a flag; loop() opens the Bluetooth lock's pairing window.
bool ui_take_pair_request();
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
