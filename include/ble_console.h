// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The serial console over Bluetooth: a Nordic UART Service (NUS) beside
// BLE-MIDI on the same NimBLE server, so the Steven Piano app can read and
// change the lighting, feel and pedal settings (docs/BLE_SETTINGS.md).
//
//   RX 6E400002-…  app -> piano, WRITE / WRITE_NR: console lines, "\n" ends one
//   TX 6E400003-…  piano -> app, NOTIFY: the replies to those lines
//
// THREADING. The write callback runs on the NimBLE host task (core 0) and
// only copies the bytes, tagged with the connection that wrote them, into a
// 1 KB FreeRTOS stream buffer (zero wait; full = dropped and counted). Lines
// are assembled, checked and run on the loop task (core 1), like serial
// input — solenoid dispatch, I²C, LEDs, LVGL and NVS never run on the
// Bluetooth task.
//
// SAFETY. Default deny: only the commands in the allow-list run over
// Bluetooth (every tunable `dump` lists except per-key force, plus status,
// dump, get, help, off, broadcast, panic, save, ledtest, testmin, testmax,
// note and the four presets). Everything else — known or not — gets
// BLE_REFUSAL, so a command added later stays bench-only until someone
// allows it here.

#pragma once

#include <Arduino.h>
#include "console_out.h"

class NimBLEServer;

// The reply to any line refused over Bluetooth.
#define BLE_REFUSAL "  refused over Bluetooth — use the USB console"

// The console's line handler (main.cpp's handleLine).
typedef void (*ConsoleLineHandler)(char *line, ConsoleSource source);

// setup(): create and start the NUS service on the shared server. Must run
// BEFORE MIDI.begin(), whose advertising start runs the one GATT server start
// (a service created after that is registered twice, the first time empty).
void ble_console_setup(NimBLEServer *srv);

// setup(), after MIDI.begin(): check that RX and TX got real attribute
// handles and log one [ble] line with the result.
void ble_console_check();

// loop(): run up to 4 complete lines that arrived over Bluetooth through
// `handle` (a refused line gets BLE_REFUSAL instead). A testmin, testmax or
// panic line ends the pass (they block). Replies are mirrored into the tee
// for the asker if it is subscribed to TX. Returns the lines handled.
int ble_console_poll(ConsoleLineHandler handle);

// loop(), once per pass near the end: send queued reply bytes to the asker as
// TX notifications (checked low-level send, at most 8 per pass).
void ble_console_flush();

// True if `line`'s command (its first word) may run over Bluetooth.
bool ble_console_allowed(const char *line);

// One indented `status` line: subscribers and dropped bytes.
void ble_console_print_status(Print &out);
