// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The Bluetooth firmware-update session (docs/BLE_OTA.md §5–§8) as a pure
// state machine. No Arduino, NimBLE, ESP-IDF, FreeRTOS or crypto header: it
// reaches the device only through ota::Port, so the same code runs on the
// ESP32 (src/ble_ota.cpp implements the Port: queues, notifications, Update,
// mbedTLS SHA-256, libsodium Ed25519) and on the Mac, where tools/ota-sim
// implements it as a fake and replays the spec's worked example and every row
// of its error table (make check).
//
// Everything runs on the loop task: the glue copies each Control and Data
// write into its queue on the NimBLE task and hands them over from loop().
// No call ever waits — a wait (the verified-OFF stop, the 10 s frame timeout,
// the 1.5 s before the restart) spans tick() calls. No heap. Port callbacks
// must not call back into the Session.
//
// SAFETY (docs/SAFETY.md rule 9): enterUpdateMode() must set the dispatch
// layer's energize inhibit and stop the piano; READY is only sent once
// coilsVerifiedOff() is true and the last energize is >= 500 ms old. END is
// the point of no return: from VERIFYING on, every Control frame (ABORT and
// BEGIN too) and a lost link are ignored, and the piano restarts with or
// without the app.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace ota {

enum : uint8_t { OP_BEGIN = 0x01, OP_END = 0x02, OP_ABORT = 0x03,
                 OP_READY = 0x81, OP_VERIFYING = 0x82, OP_OK = 0x83, OP_ABORTED = 0x84, OP_ACK = 0x85, OP_ERR = 0xE1 };
enum Err : uint8_t { ERR_NOT_QUIET = 1, ERR_TOO_LARGE = 2, ERR_BUSY = 3, ERR_BAD_FRAME = 4, ERR_HASH = 5,
                     ERR_SIGNATURE = 6, ERR_FLASH = 7, ERR_IMAGE = 8, ERR_TIMEOUT = 9, ERR_SAFETY = 10 };
enum State : uint8_t { IDLE, PREPARING, RECEIVING, VERIFYING, REBOOTING };
static const uint16_t NO_CONN = 0xFFFF;

// The device, as the session sees it. The ESP32 glue implements it once.
struct Port {
    virtual ~Port() {}
    virtual uint32_t nowMs() = 0;
    virtual bool     linkAlive(uint16_t conn) = 0;
    virtual uint16_t mtu(uint16_t conn) = 0;                                  // 0 when the link is gone, 23 if never exchanged
    virtual bool     notify(uint16_t conn, const uint8_t *bytes, size_t n) = 0;  // false = not sent; the session keeps it and retries
    virtual bool     crashSafeMode() = 0;
    virtual bool     runningImagePending() = 0;
    virtual uint32_t slotBytes() = 0;
    virtual bool     signatureOk(const uint8_t sig[64], const uint8_t digest[32]) = 0;
    virtual void     enterUpdateMode() = 0;   // inhibit energize, flush note queue, allKeysOff, strip dark, pedal up
    virtual void     leaveUpdateMode() = 0;   // back to normal playing
    virtual bool     coilsVerifiedOff() = 0;
    virtual uint32_t lastEnergizeMs() = 0;    // 0 = never since boot
    virtual uint8_t  flashBegin(uint32_t size) = 0;                 // 0, or the Err to report
    virtual uint8_t  flashWrite(const uint8_t *bytes, size_t n) = 0;   // 0, or the Err to report
    virtual uint8_t  flashEnd() = 0;                                // 0, or the Err to report; activates the new slot
    virtual void     flashAbort() = 0;
    virtual void     shaStart() = 0;
    virtual void     shaUpdate(const uint8_t *bytes, size_t n) = 0;
    virtual void     shaFinish(uint8_t out[32]) = 0;
    virtual void     onProgress(State state, uint32_t received, uint32_t size) = 0;   // screen text; called when state or whole percent changes
    virtual void     onActivated(const char *version) = 0;          // verified and activated: save settings, remember slot + version
    virtual void     restart(uint16_t conn) = 0;                    // disconnect the owner, release coils, reboot
    virtual void     log(const char *line) = 0;                     // one serial line, already formatted
};

// One update at a time. Only the owner — the connection whose BEGIN was
// accepted — is heard while a session is active; other connections get no
// reply. Every return to IDLE calls flashAbort() and leaveUpdateMode() and
// logs "[ota] <reason> — update abandoned, running firmware unchanged".
class Session {
public:
    explicit Session(Port &port);
    void onControl(uint16_t conn, const uint8_t *bytes, size_t n);   // one dequeued Control write
    void onData(uint16_t conn, const uint8_t *bytes, size_t n);      // one dequeued Data write (seq u16 LE + payload)
    void onDataDropped();                                           // the glue's Data queue overflowed
    void tick();                                                    // every loop pass: outbox, waits, timeouts, the verify step, the restart
    State    state() const;
    bool     active() const;        // PREPARING..REBOOTING
    bool     acceptsData() const;   // RECEIVING only
    uint16_t owner() const;         // NO_CONN when idle
    uint32_t received() const;      // bytes hashed and handed to flash (kept after the session ends)
    uint32_t size() const;          // BEGIN's image size (kept after the session ends)

private:
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    enum { kOutboxDepth = 4, kNoteMax = 5 };            // the longest notification is ACK, 5 bytes
    struct Note { uint16_t conn; uint8_t len; uint8_t b[kNoteMax]; };

    void begin(uint16_t conn, const uint8_t *bytes, size_t n);
    void end();
    void verify();
    void tickPreparing(uint32_t now);
    void refuse(uint16_t conn, uint8_t err, const char *why);   // idle: ERR to the sender, nothing else changes
    void fail(uint8_t err, const char *why);                    // in a session: ERR to the owner, back to IDLE
    void toIdle(const char *reason);                            // every return to IDLE
    void send(uint16_t conn, const uint8_t *bytes, uint8_t n);
    void flushOutbox();
    void progress();
    void logSignatureTime(const char *when, uint32_t t0);

    Port    &port_;
    State    state_;
    uint16_t owner_;
    bool     updateMode_;      // enterUpdateMode() called, leaveUpdateMode() still owed
    bool     restarted_;       // restart() already called (REBOOTING)
    uint32_t size_;
    uint32_t received_;
    uint8_t  sha256_[32];      // BEGIN's digest of the image
    uint8_t  sig_[64];         // BEGIN's Ed25519 signature of that digest
    char     version_[17];     // BEGIN's version, NUL-terminated
    uint8_t  window_;          // Data frames per ACK
    uint8_t  inWindow_;        // Data frames since the last ACK
    uint16_t maxChunk_;        // READY's payload limit
    uint16_t expectSeq_;       // next Data sequence number (wraps at 65,536)
    uint32_t stateMs_;         // PREPARING: start of the 1 s wait; REBOOTING: when OK was queued
    uint32_t lastFrameMs_;     // RECEIVING: last valid frame from the owner
    State    shownState_;      // what onProgress() last reported
    uint8_t  shownPct_;
    Note     out_[kOutboxDepth];   // notifications the port could not send yet, oldest first
    uint8_t  outN_;
};

}  // namespace ota
