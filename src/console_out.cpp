// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#include "console_out.h"

ConsoleOut Console;

// The TX ring itself. Static so it costs no heap and no stack; touched only
// by the loop task (Console.write while a Bluetooth line is handled, and the
// flush in ble_console.cpp).
static uint8_t s_ring[ConsoleOut::kRingBytes];

size_t ConsoleOut::write(uint8_t c) {
    return write(&c, 1);
}

size_t ConsoleOut::write(const uint8_t *buf, size_t size) {
    if (buf == nullptr || size == 0) return 0;
    // Serial first, always. Non-blocking: setup() gives it a zero TX timeout,
    // so a full USB buffer drops bytes rather than stalling the loop.
    Serial.write(buf, size);
    if (m_mirror) mirror(buf, size);
    return size;
}

void ConsoleOut::mirror(const uint8_t *buf, size_t size) {
    if (m_overflow || size > kRingBytes - m_count) {
        if (!m_overflow) {
            // First write of this reply that does not fit: from here on the
            // reply is cut. Take back its unfinished last line as well, so
            // the ring ends on a line boundary.
            m_overflow = true;
            while (m_count > 0 &&
                   s_ring[(m_tail + kRingBytes - 1) % kRingBytes] != '\n') {
                m_tail = (m_tail + kRingBytes - 1) % kRingBytes;
                m_count--;
                m_dropped++;
            }
        }
        m_dropped += (uint32_t)size;
        return;
    }
    for (size_t i = 0; i < size; i++) {
        s_ring[m_tail] = buf[i];
        m_tail = (m_tail + 1) % kRingBytes;
    }
    m_count += size;
}

size_t ConsoleOut::peek(uint8_t *out, size_t max) const {
    size_t n = (max < m_count) ? max : m_count;
    size_t at = m_head;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[at];
        at = (at + 1) % kRingBytes;
    }
    return n;
}

void ConsoleOut::consume(size_t n) {
    if (n > m_count) n = m_count;
    m_head = (m_head + n) % kRingBytes;
    m_count -= n;
    if (m_count == 0) m_head = m_tail = 0;
}
