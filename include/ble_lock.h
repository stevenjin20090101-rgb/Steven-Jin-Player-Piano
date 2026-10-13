// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Locking the piano's Bluetooth to the school tablet (docs/BLE_LOCK.md, 2.1.0).
//
// The piano keeps up to 4 device keys (32 bytes each; NVS namespace "piano",
// key "lockKeys": one blob, a count byte then the keys; read at boot, written
// only on claim and forget). With no key the piano is OPEN and behaves exactly
// as 2.0.x for every client. With a key it is CLAIMED: a connection may play,
// change settings or update only after it proves a key (`auth`, an HMAC over a
// fresh nonce). PAIRING is a 5-minute window, opened from the screen, the USB
// console or a trusted device, in which one more device may `claim`.
//
// RELEASING IS NEVER LOCKED: NoteOff, CC 120/121/123, CC64 below 64, `off`,
// `broadcast`, the disconnect stop and the hold watchdog work for everyone.
//
// Connections are told apart by (handle, generation): a GAP event listener on
// the NimBLE host task numbers every new connection, so a handle the stack
// reuses for a new central never inherits the old one's trust, nonce or
// lines. Trust ends with the connection. BLE-MIDI bytes carry no connection,
// so notes count as trusted only while exactly one central is connected and
// it has proved a key — or during guest play.
//
// THREADING. The GAP listener only counts and numbers connections (a few
// stores under a spinlock). Everything else — parsing, X25519, hashing, NVS,
// the screen text — runs on the loop task.
#pragma once

#include <Arduino.h>

// The reply to anything an unproven connection may not do on a claimed piano.
#define BLE_LOCK_REFUSAL "  refused — not authorised (send auth first)"

// setup(): after NimBLEDevice::init(), before any service is created and
// before MIDI.begin() starts advertising. Reads the keys, starts the listener.
void ble_lock_init();

// loop(), every pass: window and guest-play expiry, the disconnect after five
// failed auths, forgetting closed connections.
void ble_lock_tick();

bool ble_lock_claimed();                       // at least one key (or an unreadable key store)

// Any task: the generation of the connection that holds `conn` now; 0 when no
// connection holds it. (ble_console tags every RX write with it.)
uint16_t ble_lock_conn_gen(uint16_t conn);

// True when the piano is open, or when that connection (still open) proved a key.
bool ble_lock_trusted(uint16_t conn, uint16_t gen);
// The same for whichever connection holds `conn` now (the update's BEGIN).
bool ble_lock_trusted_conn(uint16_t conn);

// Loop task (the BLE-MIDI handlers): notes and pedal-down from Bluetooth may
// play — open, guest play, or exactly one central and it is trusted.
bool ble_lock_notes_allowed();

// Bluetooth console (loop task). `claim`, `auth` and `lock …` are the lock's:
// ble_lock_handle_ble() prints the reply lines on Console. On a claimed piano
// an untrusted connection may run only the commands ble_lock_untrusted_allowed()
// accepts (dump get status help off broadcast).
bool ble_lock_is_lock_command(const char *line);
void ble_lock_handle_ble(uint16_t conn, uint16_t gen, const char *line);
bool ble_lock_untrusted_allowed(const char *line);

// The USB console's `lock …` (status, open, reset, pairwindow, forget, guest
// <0..240>, selftest) and its answer to `claim` / `auth`. USB is never locked.
void ble_lock_handle_usb(const char *line);

// Which connection the console is serving while it runs one Bluetooth line
// (for !auth, !nonce and handleLine's own check); cleared afterwards. With
// none set (USB) the asker counts as trusted.
void ble_lock_set_asker(uint16_t conn, uint16_t gen);
void ble_lock_clear_asker();
bool ble_lock_asker_trusted();

// dump / get facts (docs/BLE_SETTINGS.md §4): !lock, !nonce, !guest (!auth is
// ble_lock_asker_trusted()). The nonce is the asker's, made on first ask; USB
// and a closed connection read 32 zeros.
const char *ble_lock_state_word();             // "open" | "claimed" | "pairing"
void        ble_lock_nonce_hex(char *buf, size_t len);
uint32_t    ble_lock_guest_seconds();          // 0 = guest play off

// The screen's "Let a new tablet pair" (loop task, never the LVGL task).
void ble_lock_open_window();

// The status line: "Pairing open • 4:59" or "Guest play • 59:12" and its
// colour; `keepAwake` is true for the pairing window only (guest play can run
// for hours and must not hold the AMOLED bright). False when neither runs.
bool ble_lock_status_text(char *buf, size_t len, uint32_t *rgb, bool *keepAwake);

// One indented `status` line.
void ble_lock_print_status(Print &out);

// `reset` cleared the whole NVS namespace: write the keys back (the lock is
// not a setting; `lock reset` is what clears it).
void ble_lock_after_settings_reset(Print &out);
