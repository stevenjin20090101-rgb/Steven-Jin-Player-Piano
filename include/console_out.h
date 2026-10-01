// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The console's output tee (docs/BLE_SETTINGS.md section 3).
//
// Everything a console COMMAND prints goes through `Console` instead of
// `Serial`: the reply lines in handleLine(), help, status, the "> line" echo,
// and the [settings] / [pedal] lines of the functions those commands call.
// Console always writes to Serial. While the mirror flag is set it also copies
// the bytes into an 8 KB ring that the Bluetooth console (ble_console.cpp)
// sends to the app as NUS TX notifications.
//
// The mirror flag is set only around handling a line that arrived over
// Bluetooth from a connection subscribed to TX, so replies go to the asker:
// USB commands (the web GUI's MIDI player streams `note` lines) are never
// mirrored, and the app never sees someone else's `dump`. Heartbeats, boot
// banners, [BLE], [midi], [hb] and debug lines stay on Serial alone.
//
// The ring is filled and drained on the loop task only, so it has no lock.
// When a write does not fit, it and the rest of that reply are dropped (and
// counted), never waited for; the reply's unfinished last line is taken back
// too, so the app never sees two lines run together.

#pragma once

#include <Arduino.h>

// Where a console line came from.
enum ConsoleSource : uint8_t {
    SRC_USB = 0,   // USB serial (console, web GUI)
    SRC_BLE = 1,   // Nordic UART over Bluetooth (the app)
};

class ConsoleOut : public Print {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;                 // keep write(const char *) and friends

    // Mirror into the TX ring from now on (true) or stop (false). Each call
    // starts a new reply as far as the overflow rule is concerned.
    void   setMirror(bool on) { m_mirror = on; m_overflow = false; }
    bool   mirroring() const  { return m_mirror; }

    // The TX ring, for the Bluetooth console's flush (loop task only).
    size_t   pending() const  { return m_count; }
    size_t   peek(uint8_t *out, size_t max) const;   // copies, consumes nothing
    void     consume(size_t n);                       // drops n bytes from the head
    void     discard()        { m_head = m_tail = m_count = 0; }
    uint32_t dropped() const  { return m_dropped; }  // bytes lost to a full ring

    static const size_t kRingBytes = 8192;

private:
    void mirror(const uint8_t *buf, size_t size);

    bool     m_mirror   = false;
    bool     m_overflow = false;   // this reply already lost bytes: drop the rest
    size_t   m_head     = 0;       // next byte to send
    size_t   m_tail     = 0;       // next free slot
    size_t   m_count    = 0;
    uint32_t m_dropped  = 0;
};

extern ConsoleOut Console;
