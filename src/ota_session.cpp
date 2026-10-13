// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The update session's state machine (docs/BLE_OTA.md §5–§8). See
// include/ota_session.h for the contract and tools/ota-sim for the proof.
//
//   IDLE ──BEGIN passes checks──► PREPARING ──verified OFF + quiet──► RECEIVING
//   RECEIVING ──END with every byte──► VERIFYING ──digest, signature, version tag, flashEnd──► REBOOTING
//   any ERR / ABORT / lost link before END ──► IDLE (slot discarded, firmware unchanged)
//
// Deliberately plain C++11 on <stdint.h>, <stddef.h> and <string.h> only: no
// printf, no heap, no RTTI, nothing that differs between the ESP32 and the Mac.
#include "ota_session.h"
#include <string.h>

namespace ota {
namespace {

const size_t   kBeginBytes     = 118;     // opcode · size u32 · sha256[32] · sig[64] · version[16] · window u8
const uint8_t  kMaxWindow      = 32;
const uint16_t kMaxChunk       = 250;     // Data's max length 252 = seq u16 + 250
const uint32_t kPrepareMs      = 1000;    // stop → verified OFF, or ERR 1
const uint32_t kQuietMs        = 500;     // last energize → READY, or ERR 1
const uint32_t kFrameTimeoutMs = 10000;   // RECEIVING: no valid frame for this long → ERR 9
const uint16_t kRestartMs      = 1500;    // OK → restart (OK carries it: 83 DC 05)

const char *errName(uint8_t e) {
    switch (e) {
        case ERR_NOT_QUIET: return "not quiet";
        case ERR_TOO_LARGE: return "too large";
        case ERR_BUSY:      return "already updating";
        case ERR_BAD_FRAME: return "bad header or sequence gap";
        case ERR_HASH:      return "hash mismatch";
        case ERR_SIGNATURE: return "bad signature";
        case ERR_FLASH:     return "flash write failed";
        case ERR_IMAGE:     return "image invalid";
        case ERR_TIMEOUT:   return "timeout";
        case ERR_SAFETY:    return "refused by safety";
        case ERR_NOT_AUTHORISED: return "not authorised";
        case ERR_NOT_NEWER: return "not newer";
        default:            return "error";
    }
}

// The version tag's marker, every byte + 1 (see VersionTagScanner): the
// table holds "TKQW2;", never the marker itself, so the scanner cannot find
// its own pattern in the image it runs from.
const uint8_t kTagMarkerPlus1[6] = { 'S' + 1, 'J' + 1, 'P' + 1, 'V' + 1, '1' + 1, ':' + 1 };
const uint8_t kTagMarkerLen      = 6;
const uint8_t kTagVersionMax     = 16;

// MAJOR.MINOR.PATCH at the start of `s` (1..9 digits each), then the end of
// the string, '-' or '+'. False for anything else.
bool parseVersion(const char *s, uint32_t v[3]) {
    if (s == NULL) return false;
    for (int k = 0; k < 3; k++) {
        if (k > 0) {
            if (*s != '.') return false;
            s++;
        }
        int      digits = 0;
        uint32_t x      = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 9) return false;
            x = x * 10u + (uint32_t)(*s - '0');
            s++;
        }
        if (digits == 0) return false;
        v[k] = x;
    }
    return *s == '\0' || *s == '-' || *s == '+';
}

bool versionChar(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '.' || c == '+' || c == '-';
}

// BEGIN's version[16]: 1..16 characters of 0-9 A-Z a-z . + - up to the first
// NUL, and nothing but zeros after it.
bool versionOk(const uint8_t *v) {
    size_t len = 0;
    while (len < 16 && v[len] != 0) len++;
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) if (!versionChar(v[i])) return false;
    for (size_t i = len; i < 16; i++) if (v[i] != 0) return false;
    return true;
}

uint32_t u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// One serial line, built without printf. Truncates rather than overflows.
struct Line {
    char   b[192];
    size_t n;
    Line() : n(0) { b[0] = '\0'; }
    Line &s(const char *t) {
        while (t && *t && n + 1 < sizeof b) b[n++] = *t++;
        b[n] = '\0';
        return *this;
    }
    Line &u(uint32_t v) {
        char d[10];
        int k = 0;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v != 0);
        while (k > 0 && n + 1 < sizeof b) b[n++] = d[--k];
        b[n] = '\0';
        return *this;
    }
    Line &x(uint8_t v) {
        static const char kHex[] = "0123456789ABCDEF";
        s("0x");
        if (n + 2 < sizeof b) { b[n++] = kHex[v >> 4]; b[n++] = kHex[v & 15]; }
        b[n] = '\0';
        return *this;
    }
};

}  // namespace

// ---- Versions --------------------------------------------------------------

bool versionNewer(const char *candidate, const char *running) {
    uint32_t a[3], b[3];
    if (!parseVersion(candidate, a) || !parseVersion(running, b)) return false;
    for (int k = 0; k < 3; k++) {
        if (a[k] != b[k]) return a[k] > b[k];
    }
    return false;   // equal is not newer
}

VersionTagScanner::VersionTagScanner() { reset(); }

void VersionTagScanner::reset() {
    match_     = 0;
    capturing_ = false;
    capLen_    = 0;
    found_     = false;
    memset(cap_, 0, sizeof cap_);
    memset(version_, 0, sizeof version_);
}

bool        VersionTagScanner::found() const   { return found_; }
const char *VersionTagScanner::version() const { return version_; }

void VersionTagScanner::feed(const uint8_t *bytes, size_t n) {
    if (bytes == NULL) return;
    for (size_t i = 0; i < n && !found_; i++) {
        const uint8_t c = bytes[i];

        // The marker matcher sees every byte, also while a version is being
        // read, so a marker that begins inside a broken candidate is still
        // found. The marker has no prefix that is also its suffix, so a
        // mismatch can only restart the match at this byte.
        bool marker = false;
        if (c == (uint8_t)(kTagMarkerPlus1[match_] - 1)) {
            if (++match_ == kTagMarkerLen) {
                match_ = 0;
                marker = true;
            }
        } else {
            match_ = (c == (uint8_t)(kTagMarkerPlus1[0] - 1)) ? 1 : 0;
        }

        if (capturing_) {
            if (c == 0) {                                   // the NUL ends the version
                if (capLen_ > 0) {
                    memcpy(version_, cap_, capLen_);
                    version_[capLen_] = '\0';
                    found_ = true;
                }
                capturing_ = false;
                capLen_    = 0;
                continue;
            }
            if (capLen_ < kTagVersionMax && versionChar(c)) {   // (the marker's last byte, ':',
                cap_[capLen_++] = (char)c;                       //  is no version character)
                continue;
            }
            capturing_ = false;                             // too long or a foreign byte: not the tag
            capLen_    = 0;
        }
        if (marker) {                                       // the version follows
            capturing_ = true;
            capLen_    = 0;
        }
    }
}

// ---- Session ---------------------------------------------------------------

Session::Session(Port &port)
    : port_(port), state_(IDLE), owner_(NO_CONN), updateMode_(false), restarted_(false),
      size_(0), received_(0), window_(0), inWindow_(0), maxChunk_(0), expectSeq_(0),
      stateMs_(0), lastFrameMs_(0), shownState_(IDLE), shownPct_(0), outN_(0) {
    memset(sha256_, 0, sizeof sha256_);
    memset(sig_, 0, sizeof sig_);
    memset(version_, 0, sizeof version_);
    memset(out_, 0, sizeof out_);
}

State    Session::state() const       { return state_; }
bool     Session::active() const      { return state_ != IDLE; }
bool     Session::acceptsData() const { return state_ == RECEIVING; }
uint16_t Session::owner() const       { return owner_; }
uint32_t Session::received() const    { return received_; }
uint32_t Session::size() const        { return size_; }

// ---- Control ---------------------------------------------------------------

void Session::onControl(uint16_t conn, const uint8_t *bytes, size_t n) {
    if (conn == NO_CONN) return;
    if (bytes == NULL) n = 0;
    if (state_ != IDLE && conn != owner_) return;   // only the owner is heard; others get no reply
    const uint8_t op = n ? bytes[0] : 0;

    switch (state_) {
    case IDLE:
        if (n == 0) {
            refuse(conn, ERR_BAD_FRAME, "empty Control frame");
        } else if (op == OP_BEGIN) {
            begin(conn, bytes, n);
        } else if (op == OP_ABORT) {
            const uint8_t f[1] = { OP_ABORTED };     // nothing to abort: say so all the same
            send(conn, f, 1);
            port_.log(Line().s("[ota] ABORT while idle (conn ").u(conn).s(") — answered ABORTED").b);
        } else if (op == OP_END) {
            // nothing to end: ignored
        } else {
            refuse(conn, ERR_BAD_FRAME, Line().s("unknown Control opcode ").x(op).b);
        }
        return;

    case PREPARING:
    case RECEIVING:
        if (n == 0) {
            fail(ERR_BAD_FRAME, "empty Control frame");
        } else if (op == OP_BEGIN) {
            fail(ERR_BUSY, "a second BEGIN from the app");
        } else if (op == OP_ABORT) {
            const uint8_t f[1] = { OP_ABORTED };
            send(owner_, f, 1);
            toIdle("ABORT from the app");
        } else if (op == OP_END) {
            if (state_ == RECEIVING) end();   // before READY: ignored
        } else {
            fail(ERR_BAD_FRAME, Line().s("unknown Control opcode ").x(op).b);
        }
        return;

    case VERIFYING:
    case REBOOTING:
        return;   // END was the point of no return: nothing the app sends changes it now
    }
}

// BEGIN while idle. Checks that touch no hardware come first, so a refused
// BEGIN never interrupts the music (BLE_OTA.md §6); each refusal answers the
// sender and changes nothing else. Order: pending (3) > frame (4) > not
// authorised (11) > safe mode (10) > not newer (12) > size (2) > signature (6).
void Session::begin(uint16_t conn, const uint8_t *b, size_t n) {
    if (port_.runningImagePending()) {
        refuse(conn, ERR_BUSY, "BEGIN while the running firmware is not confirmed yet");
        return;
    }
    if (n != kBeginBytes) {
        refuse(conn, ERR_BAD_FRAME, Line().s("BEGIN is ").u((uint32_t)n).s(" bytes, not 118").b);
        return;
    }
    const uint32_t size   = u32le(b + 1);
    const uint8_t  window = b[117];
    if (size == 0) {
        refuse(conn, ERR_BAD_FRAME, "BEGIN size 0");
        return;
    }
    if (window < 1 || window > kMaxWindow) {
        refuse(conn, ERR_BAD_FRAME, Line().s("BEGIN window ").u(window).s(", not 1..32").b);
        return;
    }
    if (!versionOk(b + 101)) {
        refuse(conn, ERR_BAD_FRAME, "BEGIN version is not 1..16 of 0-9 A-Z a-z . + -, zero-padded");
        return;
    }
    // The Bluetooth lock (docs/BLE_LOCK.md): only a connection that proved a
    // key may update a claimed piano. Before anything that costs time.
    if (!port_.authorised(conn)) {
        refuse(conn, ERR_NOT_AUTHORISED, "this connection is not authorised (the piano is locked; send auth first)");
        return;
    }
    if (port_.crashSafeMode()) {
        refuse(conn, ERR_SAFETY, "crash-loop safe mode is on this boot — switch the piano off and on");
        return;
    }
    // Updates never go backwards. BEGIN's version is not signed, so this only
    // spares an honest app a minute of transfer; the version tag inside the
    // signed image is checked after END.
    char beginVersion[17];
    memcpy(beginVersion, b + 101, 16);
    beginVersion[16] = '\0';
    const char *running = port_.runningVersion();
    if (!versionNewer(beginVersion, running)) {
        refuse(conn, ERR_NOT_NEWER, Line().s("BEGIN version ").s(beginVersion).s(" is not newer than the running ")
                                          .s(running).b);
        return;
    }
    const uint32_t slot = port_.slotBytes();
    if (size > slot) {
        refuse(conn, ERR_TOO_LARGE, Line().s("BEGIN size ").u(size).s(" > update slot ").u(slot).b);
        return;
    }
    // The early signature check, over BEGIN's own digest: a wrong key costs a
    // second here, not a minute of transfer. The real check is after END.
    const uint32_t t0 = port_.nowMs();
    const bool signed_ = port_.signatureOk(b + 37, b + 5);
    logSignatureTime("at BEGIN", t0);
    if (!signed_) {
        refuse(conn, ERR_SIGNATURE, "BEGIN signature does not verify");
        return;
    }

    // Accepted: this connection owns the session until it returns to IDLE.
    owner_     = conn;
    size_      = size;
    received_  = 0;
    window_    = window;
    inWindow_  = 0;
    maxChunk_  = 0;
    expectSeq_ = 0;
    restarted_ = false;
    memcpy(sha256_, b + 5, sizeof sha256_);
    memcpy(sig_, b + 37, sizeof sig_);
    memset(version_, 0, sizeof version_);
    memcpy(version_, b + 101, 16);            // zero-padded (checked above); version_[16] stays 0
    state_ = PREPARING;
    updateMode_ = true;
    port_.enterUpdateMode();                  // energize inhibited, piano stopped, strip dark, pedal up
    stateMs_ = port_.nowMs();                 // the 1 s verified-OFF wait starts at the stop
    progress();
    port_.log(Line().s("[ota] BEGIN from conn ").u(conn).s(": ").s(version_).s(", ").u(size_)
                    .s(" bytes, window ").u(window_).s(" — piano stopped, waiting for verified OFF").b);
}

void Session::end() {
    if (received_ != size_) {
        fail(ERR_BAD_FRAME, Line().s("END after ").u(received_).s(" of ").u(size_).s(" bytes").b);
        return;
    }
    // VERIFYING goes out first; the check itself runs on the next tick(),
    // whether or not this notification could be sent yet.
    state_ = VERIFYING;
    const uint8_t f[1] = { OP_VERIFYING };
    send(owner_, f, 1);
    progress();
    port_.log(Line().s("[ota] END — all ").u(size_).s(" bytes in, verifying").b);
}

// ---- Data ------------------------------------------------------------------

void Session::onData(uint16_t conn, const uint8_t *bytes, size_t n) {
    if (state_ != RECEIVING || conn != owner_ || conn == NO_CONN) return;   // stale or foreign: no reply
    if (bytes == NULL || n < 3) {
        fail(ERR_BAD_FRAME, "Data frame shorter than 3 bytes");
        return;
    }
    const uint16_t seq = (uint16_t)(bytes[0] | (bytes[1] << 8));
    const size_t   len = n - 2;
    if (seq != expectSeq_) {
        fail(ERR_BAD_FRAME, Line().s("Data seq ").u(seq).s(", expected ").u(expectSeq_).b);
        return;
    }
    if (len > maxChunk_) {
        fail(ERR_BAD_FRAME, Line().s("Data payload ").u((uint32_t)len).s(" > maxChunk ").u(maxChunk_).b);
        return;
    }
    if (len > size_ - received_) {
        fail(ERR_BAD_FRAME, Line().s("Data past the image's ").u(size_).s(" bytes").b);
        return;
    }
    port_.shaUpdate(bytes + 2, len);
    tag_.feed(bytes + 2, len);
    const uint8_t e = port_.flashWrite(bytes + 2, len);
    if (e != 0) {
        fail(e, Line().s("flash write at byte ").u(received_).b);
        return;
    }
    received_  += (uint32_t)len;
    expectSeq_  = (uint16_t)(expectSeq_ + 1);
    lastFrameMs_ = port_.nowMs();
    if (++inWindow_ >= window_ || received_ == size_) {   // a full window, or the last partial one
        inWindow_ = 0;
        const uint8_t ack[5] = { OP_ACK, (uint8_t)received_, (uint8_t)(received_ >> 8),
                                 (uint8_t)(received_ >> 16), (uint8_t)(received_ >> 24) };
        send(owner_, ack, 5);
    }
    progress();
}

void Session::onDataDropped() {
    if (state_ == RECEIVING) fail(ERR_BAD_FRAME, "the Data queue overflowed");
}

// ---- Every loop pass -------------------------------------------------------

void Session::tick() {
    flushOutbox();
    const uint32_t now = port_.nowMs();
    switch (state_) {
    case IDLE:
        return;
    case PREPARING:
        tickPreparing(now);
        return;
    case RECEIVING:
        if (!port_.linkAlive(owner_)) {
            toIdle("the app's link dropped");
            return;
        }
        if ((uint32_t)(now - lastFrameMs_) >= kFrameTimeoutMs) {
            fail(ERR_TIMEOUT, "no valid frame from the app for 10 s");
        }
        return;
    case VERIFYING:
        verify();
        return;
    case REBOOTING:
        if (!restarted_ && (uint32_t)(now - stateMs_) >= kRestartMs) {
            restarted_ = true;
            port_.log(Line().s("[ota] restarting into ").s(version_).b);
            port_.restart(owner_);
        }
        return;
    }
}

// The safety sequence of BLE_OTA.md §7, one poll per pass: the normal safety
// ticks do the releasing; READY waits for every coil verified OFF.
void Session::tickPreparing(uint32_t now) {
    if (!port_.linkAlive(owner_)) {
        toIdle("the app's link dropped");
        return;
    }
    if (port_.coilsVerifiedOff()) {
        const uint32_t last = port_.lastEnergizeMs();
        if (last != 0 && (uint32_t)(now - last) < kQuietMs) {
            fail(ERR_NOT_QUIET, Line().s("a coil was energized ").u(now - last)
                                      .s(" ms before the check (needs 500)").b);
            return;
        }
        const uint8_t e = port_.flashBegin(size_);
        if (e != 0) {
            fail(e, "the update slot could not be opened");
            return;
        }
        const uint16_t mtu = port_.mtu(owner_);
        if (mtu <= 5) {                        // 0: the link went in the meantime
            toIdle("the app's link dropped");
            return;
        }
        maxChunk_    = ((uint16_t)(mtu - 5) > kMaxChunk) ? kMaxChunk : (uint16_t)(mtu - 5);   // min(MTU − 5, 250)
        port_.shaStart();
        tag_.reset();
        expectSeq_   = 0;
        received_    = 0;
        inWindow_    = 0;
        state_       = RECEIVING;
        lastFrameMs_ = port_.nowMs();          // the 10 s frame timeout starts at READY
        const uint8_t ready[4] = { OP_READY, (uint8_t)maxChunk_, (uint8_t)(maxChunk_ >> 8), window_ };
        send(owner_, ready, 4);
        progress();
        port_.log(Line().s("[ota] coils verified off — READY, maxChunk ").u(maxChunk_)
                        .s(", window ").u(window_).b);
        return;
    }
    if ((uint32_t)(now - stateMs_) >= kPrepareMs) {
        fail(ERR_NOT_QUIET, "coils not verified off within 1 s of the stop");
    }
}

// After END, in one pass, in this order (BLE_OTA.md §8): the digest of the
// bytes received, the signature over THAT digest, the version tag found in
// those bytes (ERR 12 unless newer than the running firmware), then the slot
// switch.
void Session::verify() {
    uint8_t digest[32];
    port_.shaFinish(digest);
    if (memcmp(digest, sha256_, sizeof digest) != 0) {
        fail(ERR_HASH, "the received image's SHA-256 is not BEGIN's");
        return;
    }
    const uint32_t t0 = port_.nowMs();
    const bool signed_ = port_.signatureOk(sig_, digest);
    logSignatureTime("after END", t0);
    if (!signed_) {
        fail(ERR_SIGNATURE, "the signature does not verify over the received image");
        return;
    }
    // Updates never go backwards: the version tag inside the signed bytes —
    // found as they streamed in — must be there and newer than this firmware.
    // Before flashEnd(), which is what switches the boot slot.
    const char *running = port_.runningVersion();
    if (!tag_.found()) {
        fail(ERR_NOT_NEWER, "the image carries no version tag (releases before 2.1.0 have none)");
        return;
    }
    if (!versionNewer(tag_.version(), running)) {
        fail(ERR_NOT_NEWER, Line().s("the image's version tag ").s(tag_.version())
                                  .s(" is not newer than the running ").s(running).b);
        return;
    }
    const uint8_t e = port_.flashEnd();
    if (e != 0) {
        fail(e, "the new slot could not be activated");
        return;
    }
    port_.onActivated(version_);
    state_     = REBOOTING;
    stateMs_   = port_.nowMs();
    restarted_ = false;
    const uint8_t ok[3] = { OP_OK, (uint8_t)kRestartMs, (uint8_t)(kRestartMs >> 8) };
    send(owner_, ok, 3);
    progress();
    port_.log(Line().s("[ota] ").s(version_).s(" verified and activated — restarting in ").u(kRestartMs).s(" ms").b);
}

// ---- Endings ---------------------------------------------------------------

void Session::refuse(uint16_t conn, uint8_t err, const char *why) {
    const uint8_t f[2] = { OP_ERR, err };
    send(conn, f, 2);
    port_.log(Line().s("[ota] refused — ERR ").u(err).s(" (").s(errName(err)).s("): ").s(why).b);
}

void Session::fail(uint8_t err, const char *why) {
    const uint8_t f[2] = { OP_ERR, err };
    send(owner_, f, 2);
    toIdle(Line().s("ERR ").u(err).s(" (").s(errName(err)).s("): ").s(why).b);
}

// Every return to IDLE: the session reads as idle from here on (also inside
// the port callbacks below), the slot is discarded — Update.abort() erases
// nothing, and the old firmware's slot was never touched — and normal playing
// resumes. Notifications still in the outbox keep going out.
void Session::toIdle(const char *reason) {
    state_ = IDLE;
    owner_ = NO_CONN;
    port_.flashAbort();
    memset(sha256_, 0, sizeof sha256_);       // forget the hash state
    memset(sig_, 0, sizeof sig_);
    tag_.reset();
    inWindow_  = 0;
    expectSeq_ = 0;
    if (updateMode_) {
        updateMode_ = false;
        port_.leaveUpdateMode();
    }
    progress();
    port_.log(Line().s("[ota] ").s(reason).s(" — update abandoned, running firmware unchanged").b);
}

// ---- Notifications ---------------------------------------------------------

// Never an empty notification: NimBLE would send the characteristic's stored
// value instead (BLE_OTA.md §4). A link with older notifications still queued
// queues behind them, so each link sees its notifications in order.
void Session::send(uint16_t conn, const uint8_t *bytes, uint8_t n) {
    if (conn == NO_CONN || bytes == NULL || n == 0 || n > kNoteMax) return;
    bool waiting = false;
    for (uint8_t i = 0; i < outN_; i++) {
        if (out_[i].conn == conn) { waiting = true; break; }
    }
    if (!waiting && port_.notify(conn, bytes, n)) return;
    if (outN_ == kOutboxDepth) {               // full: drop the oldest, the newest says more
        memmove(&out_[0], &out_[1], sizeof(Note) * (kOutboxDepth - 1));
        outN_--;
        port_.log("[ota] notification outbox full — oldest dropped");
    }
    Note &q = out_[outN_++];
    q.conn = conn;
    q.len  = n;
    memcpy(q.b, bytes, n);
}

// Start of every tick(): retry in order; a link that refuses again keeps the
// rest of its queue, and a link that is gone loses its entries.
void Session::flushOutbox() {
    uint16_t blocked[kOutboxDepth];
    uint8_t  nBlocked = 0, kept = 0;
    for (uint8_t i = 0; i < outN_; i++) {
        const Note q = out_[i];
        if (!port_.linkAlive(q.conn)) continue;
        bool keep = false;
        for (uint8_t j = 0; j < nBlocked; j++) {
            if (blocked[j] == q.conn) { keep = true; break; }
        }
        if (!keep && !port_.notify(q.conn, q.b, q.len)) {
            keep = true;
            blocked[nBlocked++] = q.conn;
        }
        if (keep) out_[kept++] = q;
    }
    outN_ = kept;
}

// ---- Screen and log --------------------------------------------------------

void Session::progress() {
    const uint8_t pct = size_ ? (uint8_t)(((uint64_t)received_ * 100u) / size_) : 0;
    if (state_ == shownState_ && pct == shownPct_) return;
    shownState_ = state_;
    shownPct_   = pct;
    port_.onProgress(state_, received_, size_);
}

void Session::logSignatureTime(const char *when, uint32_t t0) {
    port_.log(Line().s("[ota] signature checked ").s(when).s(" in ").u(port_.nowMs() - t0).s(" ms").b);
}

}  // namespace ota
