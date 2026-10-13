// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Firmware updates over Bluetooth (docs/BLE_OTA.md), on the shared NimBLE server:
//
//   Device Information 0x180A  0x2A29 "Steven Jin", 0x2A24 "Steven Piano",
//                              0x2A26 the version string ("2.0.0+a1b2c3d")
//   Update 7D0A0001-…          Control 7D0A0002-… WRITE + NOTIFY, at most 118 bytes
//                              Data    7D0A0003-… WRITE_NR, at most 252 bytes
//
// The protocol — BEGIN's checks in order, the safety stop, windows and ACKs,
// timeouts, END as the point of no return — is ota::Session (ota_session.h), a
// pure state machine proven on the Mac by tools/ota-sim. This is its glue: the
// ota::Port over NimBLE, Update, mbedTLS SHA-256 and libsodium Ed25519, plus
// what outlives one session: rollback, the self-test that confirms a new
// image, the boot report, and the status-line texts.
//
// THREADING. The two write callbacks run on the NimBLE host task (core 0) and
// only copy the frame, tagged with its connection, into a queue with zero
// wait. Everything else — parsing, hashing, flash, the signature, the
// notifications, NVS, the screen texts — runs in ble_ota_tick() on the loop
// task (core 1), at most one Control frame and 4,096 Data payload bytes a pass.
//
// SAFETY (docs/SAFETY.md rule 9). From an accepted BEGIN until the session ends
// or the piano restarts, the dispatch layer refuses every energize
// (set_energize_inhibit, set FIRST), notes are dropped at the input funnel,
// the console refuses its energize commands, the pedal is held up and the
// strip is dark. READY goes out only once coilsVerifiedOff() and the last
// energize is 500 ms old; nothing unsigned is ever activated.
//
// LOCK AND ANTI-ROLLBACK (2.1.0, docs/BLE_LOCK.md). On a claimed piano BEGIN
// needs a connection that proved a key (else ERR 11); BEGIN's version and the
// version tag inside the signed image must be newer than FW_VERSION (else
// ERR 12) — releases before 2.1.0 carry no tag, so none of them installs.

#pragma once

#include <Arduino.h>

class NimBLEServer;

// setup(), BEFORE MIDI.begin() (like ble_console_setup: a service created
// after the GATT server starts is registered twice, the first time empty):
// the Device Information and Update services on the shared server.
void ble_ota_setup(NimBLEServer *srv);

// setup(), after MIDI.begin(): check that Control, Data and 0x2A26 got real
// attribute handles and log one [ble] line. Until it passes, frames are dropped.
void ble_ota_check();

// setup(), right after the [fw] banner: reads the running image's state and
// NVS boardsSeen, and reports an update that did not take (NVS otaPart names
// another slot than the running one): one [ota] line, "Update to <version>
// rolled back" on the status line for 60 s, both keys erased.
void ble_ota_boot_report();

// loop(), after the safety ticks and the all-off services, before MIDI.read():
// the session's tick, at most one Control frame, Data frames up to 4,096
// payload bytes; NVS boardsSeen; the self-test that confirms a new image.
void ble_ota_tick();

// True from an accepted BEGIN until the session ends (or the piano restarts).
// Safe from any task: the note funnel asks it on the NimBLE and LVGL tasks.
bool ble_ota_session_active();

// The text the status line should show instead of the BLE state — the session's
// "Updating • 38 %" while one runs, then for 60 s an outcome ("Updated to
// 2.0.1", "Update to 2.0.1 rolled back") — with its colour; false when none.
// Loop task. No LVGL here: loop() shows it inside its ui_lock block.
bool ble_ota_status_text(char *buf, size_t len, uint32_t *rgb);

// Crash-loop safe mode is on for this boot (settings_boot_inc() >= 3): BEGIN
// gets ERR 10. Defined in main.cpp; set once in setup().
extern bool g_crashSafeMode;

// Implemented in main.cpp, which owns the note queue, the LED strip and the
// status line. Called by the update session on the loop task only.
//   stop:   after the energize inhibit is set — flush the note queue,
//           allKeysOff(), clear the reactive LEDs, pedal up, strip dark.
//   resume: after the inhibit is cleared — one full LED frame in the current
//           mode (STATIC caches its last frame), the BLE status line back.
void piano_stop_for_update();
void piano_resume_after_update();
