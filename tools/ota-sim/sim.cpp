// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Host-side simulator for the Bluetooth update session.
//
// Compiles the REAL src/ota_session.cpp with the device's language settings
// (-std=gnu++11 -fno-rtti) against a fake ota::Port — a fake clock and links,
// scripted failures, a record of every notification and port call, and a
// SHA-256 written here (no library) — and plays the app's side of BLE_OTA.md
// §5–§8: the worked example byte for byte, every row of the error table, the
// aborts, lost links, foreign connections and refused notifications.
//
//   make check                     every scenario; non-zero exit on any failure
//   ./sim err5_hash abort_idle     only these
//   SIM_VERBOSE=1 ./sim err9_timeout   also the session's log and notifications
//
// One loop pass of the glue = Session::tick() = 1 ms of fake time. See
// docs/BLE_OTA.md and docs/SAFETY.md rule 9. Add a scenario whenever you
// touch src/ota_session.cpp.
//
// From 2.1.0 (docs/BLE_LOCK.md) the fake runs firmware "2.0.2", trusts every
// connection unless told otherwise (ERR 11), and every test image carries a
// version tag "2.1.0" inside its signed bytes — except §5's worked-example
// image, whose documented bytes predate the tag: it is now refused after END
// with ERR 12 (hash and signature pass, no tag). `worked_example_tagged` runs
// the same flow with a tagged image of the same size and ends in OK.
//
//   ./sim --scan <file>            report the version tag the session finds in a file
//                                  (e.g. ../../.pio/build/lilygo-t4-s3/firmware.bin)
#include "ota_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using ota::Session;
using ota::State;
typedef std::vector<uint8_t> Bytes;

static bool g_verbose = getenv("SIM_VERBOSE") != NULL;

// ---- SHA-256 (FIPS 180-4), small and slow, for checking only ---------------
static const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
struct Sha256 {
    uint32_t h[8];
    uint8_t  buf[64];
    uint64_t bytes;
    size_t   fill;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void start() {
        static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h, iv, sizeof h);
        bytes = 0;
        fill = 0;
    }
    void block() {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)buf[4 * i] << 24 | (uint32_t)buf[4 * i + 1] << 16 | (uint32_t)buf[4 * i + 2] << 8 | buf[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }
    void update(const uint8_t *p, size_t n) {
        bytes += n;
        while (n--) { buf[fill++] = *p++; if (fill == 64) { block(); fill = 0; } }
    }
    void finish(uint8_t out[32]) {
        const uint64_t bits = bytes * 8;
        buf[fill++] = 0x80;
        if (fill > 56) { while (fill < 64) buf[fill++] = 0; block(); fill = 0; }
        while (fill < 56) buf[fill++] = 0;
        for (int i = 7; i >= 0; i--) buf[fill++] = (uint8_t)(bits >> (8 * i));
        block();
        for (int i = 0; i < 8; i++) {
            out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16);
            out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i];
        }
    }
};
static void sha256(const uint8_t *p, size_t n, uint8_t out[32]) { Sha256 s; s.start(); s.update(p, n); s.finish(out); }

// ---- bytes and text --------------------------------------------------------
static std::string hex(const uint8_t *b, size_t n, bool spaced = true) {
    std::string s;
    char t[4];
    for (size_t i = 0; i < n; i++) {
        snprintf(t, sizeof t, spaced ? "%02X" : "%02x", b[i]);
        if (spaced && i) s += ' ';
        s += t;
    }
    return s;
}
static std::string hex(const Bytes &b) { return hex(b.data(), b.size()); }
static Bytes unhex(const char *s) {
    Bytes out;
    while (*s) {
        while (*s == ' ' || *s == '\n') s++;
        if (!*s) break;
        unsigned v = 0;
        sscanf(s, "%2x", &v);
        out.push_back((uint8_t)v);
        s += 2;
    }
    return out;
}
static Bytes base64(const char *s) {
    Bytes out;
    uint32_t acc = 0;
    int bits = 0;
    for (; *s; s++) {
        int v;
        if (*s >= 'A' && *s <= 'Z') v = *s - 'A';
        else if (*s >= 'a' && *s <= 'z') v = *s - 'a' + 26;
        else if (*s >= '0' && *s <= '9') v = *s - '0' + 52;
        else if (*s == '+') v = 62;
        else if (*s == '/') v = 63;
        else continue;   // '=' padding
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return out;
}
static std::string join(const std::vector<std::string> &v, size_t maxItems = 8) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i == maxItems && v.size() > maxItems + 1) { s += " | … " + std::to_string(v.size() - maxItems - 1) + " more … | " + v.back(); break; }
        if (i) s += " | ";
        s += v[i];
    }
    return "[" + s + "]";
}
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// ---- BLE_OTA.md §5's worked example, as printed there -----------------------
static const char kDocBegin[] =
    "01 00 20 0F 00 6D 87 2B 96 45 D7 19 95 87 1A 34 "
    "6D 90 6E 64 C1 B4 EA 4D 92 DD 7F 88 CC CE A7 5F "
    "27 92 B2 12 C6 27 78 01 1C 44 97 C2 CC 29 C4 08 "
    "5B 46 68 99 0D 2E 58 F1 86 CB 93 5E B3 C6 04 41 "
    "26 F0 9A 79 93 BB 38 EC 4F 3C 63 E6 06 3B 81 92 "
    "88 74 BF 7C AD 29 2F F0 F4 F7 93 0C 93 41 8E 87 "
    "B8 CA 2D E6 0A 32 2E 31 2E 30 00 00 00 00 00 00 "
    "00 00 00 00 00 10";
static const char kDocSha[] = "6d872b9645d71995871a346d906e64c1b4ea4d92dd7f88cccea75f2792b212c6";
static const char kDocSigB64[] =   // §10's manifest "sig": the same 64 bytes as BEGIN's 37..100
    "J3gBHESXwswpxAhbRmiZDS5Y8YbLk16zxgRBJvCaeZO7OOxPPGPmBjuBkoh0v3ytKS/w9PeTDJNBjoe4yi3mCg==";
static const uint32_t kDocSize = 991232;

// An image and what the app would put in BEGIN for it. The fake verifier
// accepts §5's real Ed25519 signature for §5's digest, and fakeSign(digest)
// for anything else — so a signature still binds exactly one digest.
struct Image { Bytes bytes; uint8_t sha[32]; uint8_t sig[64]; };
static void fakeSign(const uint8_t digest[32], uint8_t sig[64]) {
    for (int i = 0; i < 64; i++) sig[i] = (uint8_t)(digest[i % 32] ^ (0xA5 + 3 * i));
}
// The version tag as the firmware builds it ("SJPV1:" FW_VERSION, NUL ended).
static Bytes tagBytes(const char *version) {
    std::string t = std::string("SJPV1:") + version;
    Bytes b(t.begin(), t.end());
    b.push_back(0);
    return b;
}
static void reseal(Image &im) {                    // hash and fake-sign the bytes as they are now
    sha256(im.bytes.data(), im.bytes.size(), im.sha);
    fakeSign(im.sha, im.sig);
}
// §5's pattern byte[i] = (0xE9 + 7·i) & 0xFF, then `extra` written at `at`.
static Image patternImage(size_t n, const Bytes &extra = Bytes(), size_t at = 0) {
    Image im;
    im.bytes.resize(n);
    for (size_t i = 0; i < n; i++) im.bytes[i] = (uint8_t)((0xE9 + 7 * i) & 0xFF);
    for (size_t i = 0; i < extra.size() && at + i < n; i++) im.bytes[at + i] = extra[i];
    reseal(im);
    return im;
}
// A test image: §5's pattern with the tag `version` a quarter of the way in.
static Image makeImage(size_t n, const char *version = "2.1.0") {
    return patternImage(n, tagBytes(version), n / 4);
}
// §5's worked-example image exactly as documented: no tag, the real signature.
static Image docImage() {
    Image im = patternImage(kDocSize);
    memcpy(im.sig, &unhex(kDocBegin)[37], 64);
    return im;
}
static Bytes beginFrame(uint32_t size, const uint8_t sha[32], const uint8_t sig[64], const char *version, uint8_t window) {
    Bytes f(118, 0);
    f[0] = 0x01;
    f[1] = (uint8_t)size; f[2] = (uint8_t)(size >> 8); f[3] = (uint8_t)(size >> 16); f[4] = (uint8_t)(size >> 24);
    memcpy(&f[5], sha, 32);
    memcpy(&f[37], sig, 64);
    size_t vl = strlen(version);
    memcpy(&f[101], version, vl > 16 ? 16 : vl);
    f[117] = window;
    return f;
}
static Bytes beginFor(const Image &im, uint8_t window = 16, const char *version = "2.1.0") {
    return beginFrame((uint32_t)im.bytes.size(), im.sha, im.sig, version, window);
}
static Bytes dataFrame(uint32_t seq, const Bytes &img, size_t at, size_t len) {
    Bytes f(2 + len);
    f[0] = (uint8_t)seq;
    f[1] = (uint8_t)(seq >> 8);
    if (len) memcpy(&f[2], &img[at], len);
    return f;
}

// ---- the fake device -------------------------------------------------------
struct Fake : ota::Port {
    uint32_t t = 100000;   // fake millis(), far from 0 so every "now - then" is real
    struct Link { uint16_t conn; bool alive; uint16_t mtu; bool refusing; };   // refusing: its notifications fail
    std::vector<Link> links;

    // scripted behaviour
    bool     safeMode = false, pendingImage = false;
    std::string running = "2.0.2";               // runningVersion(): the firmware the piano runs
    std::vector<uint16_t> untrusted;             // connections the lock does not trust (ERR 11)
    int      authCalls = 0;
    uint32_t slot = 6553600;                     // app0/app1 in default_16MB.csv
    bool     coilsOff = true;                    // coilsVerifiedOff()'s answer ...
    uint32_t coilsOffFrom = 0;                   // ... from this time on
    uint32_t lastEnergize = 0;
    int      failSigCall = 0;                    // the N-th signatureOk() answers false (1-based; 0 never)
    uint8_t  failBegin = 0, failEnd = 0, failWriteCode = 0;
    uint32_t failWriteAtByte = 0xFFFFFFFF;       // the flashWrite() covering this offset fails
    uint32_t refuseFrom = 1, refuseTo = 0;       // notify() refuses while t is in [from, to)
    int      refuseNext = 0;                     // ... or the next N attempts

    // recorded
    struct Sent { uint16_t conn; Bytes b; uint32_t t; };
    std::vector<Sent> sent;
    int notifyCalls = 0, notifyRefused = 0, emptyNotifies = 0;
    int enter = 0, leave = 0, modeViolations = 0;
    bool inMode = false;
    int fBegin = 0, fWrites = 0, fEnd = 0, fAbort = 0, flashViolations = 0;
    bool fOpen = false;
    uint32_t fBytes = 0;
    Sha256 fSha;                                  // what flash received, independently hashed
    int shaStarts = 0, shaFinishes = 0, shaViolations = 0;
    bool shaOn = false;
    Sha256 sha;
    int sigCalls = 0;
    std::vector<Bytes> sigDigests;
    bool lastCoils = false, readyChecked = false;
    uint32_t lastCoilsAt = 0;
    int readyViolations = 0;
    std::vector<std::pair<int, int> > progress;   // (state, whole percent)
    int activated = 0;
    std::string version;
    int restarts = 0;
    uint32_t restartAt = 0;
    uint16_t restartConn = 0;
    std::vector<std::string> logs;
    uint8_t docSig[64], docSha[32];

    Fake() {
        links.push_back(Link{1, true, 255, false});
        Bytes d = unhex(kDocBegin);
        memcpy(docSig, &d[37], 64);
        memcpy(docSha, &d[5], 32);
    }
    Link *link(uint16_t c) { for (size_t i = 0; i < links.size(); i++) if (links[i].conn == c) return &links[i]; return NULL; }

    uint32_t nowMs() override { return t; }
    bool linkAlive(uint16_t c) override { Link *l = link(c); return l && l->alive; }
    uint16_t mtu(uint16_t c) override { Link *l = link(c); return (l && l->alive) ? l->mtu : 0; }
    bool notify(uint16_t c, const uint8_t *b, size_t n) override {
        notifyCalls++;
        if (b == NULL || n == 0) { emptyNotifies++; return false; }
        if (b[0] == ota::OP_READY && !readyChecked) {   // READY only right after a verified-OFF, quiet answer
            readyChecked = true;
            bool quiet = lastEnergize == 0 || t - lastEnergize >= 500;
            if (!(lastCoils && lastCoilsAt == t && quiet && inMode)) readyViolations++;
        }
        if (!linkAlive(c)) { notifyRefused++; return false; }
        if (link(c)->refusing) { notifyRefused++; return false; }
        if ((t >= refuseFrom && t < refuseTo) || refuseNext > 0) {
            if (refuseNext > 0) refuseNext--;
            notifyRefused++;
            return false;
        }
        sent.push_back(Sent{c, Bytes(b, b + n), t});
        if (g_verbose) printf("    t=%u  conn %u <- %s\n", t, c, hex(b, n).c_str());
        return true;
    }
    bool crashSafeMode() override { return safeMode; }
    bool runningImagePending() override { return pendingImage; }
    bool authorised(uint16_t c) override {
        authCalls++;
        for (size_t i = 0; i < untrusted.size(); i++) if (untrusted[i] == c) return false;
        return true;
    }
    const char *runningVersion() override { return running.c_str(); }
    uint32_t slotBytes() override { return slot; }
    bool signatureOk(const uint8_t s[64], const uint8_t d[32]) override {
        sigCalls++;
        sigDigests.push_back(Bytes(d, d + 32));
        if (failSigCall == sigCalls) return false;
        if (memcmp(s, docSig, 64) == 0 && memcmp(d, docSha, 32) == 0) return true;
        uint8_t want[64];
        fakeSign(d, want);
        return memcmp(s, want, 64) == 0;
    }
    void enterUpdateMode() override { enter++; if (inMode) modeViolations++; inMode = true; readyChecked = false; }
    void leaveUpdateMode() override { leave++; if (!inMode) modeViolations++; inMode = false; }
    bool coilsVerifiedOff() override {
        if (!inMode) modeViolations++;   // only ever polled while the piano is stopped
        lastCoilsAt = t;
        lastCoils = coilsOff && t >= coilsOffFrom;
        return lastCoils;
    }
    uint32_t lastEnergizeMs() override { return lastEnergize; }
    uint8_t flashBegin(uint32_t) override {
        fBegin++;
        if (fOpen || !inMode) flashViolations++;
        if (failBegin) return failBegin;
        fOpen = true;
        fBytes = 0;
        fSha.start();
        return 0;
    }
    uint8_t flashWrite(const uint8_t *b, size_t n) override {
        fWrites++;
        if (!fOpen || !shaOn) flashViolations++;
        if (failWriteCode && failWriteAtByte >= fBytes && failWriteAtByte < fBytes + n) return failWriteCode;
        fSha.update(b, n);
        fBytes += (uint32_t)n;
        return 0;
    }
    uint8_t flashEnd() override {
        fEnd++;
        if (!fOpen) flashViolations++;
        if (failEnd) return failEnd;
        fOpen = false;
        return 0;
    }
    void flashAbort() override { fAbort++; fOpen = false; }
    void shaStart() override { shaStarts++; sha.start(); shaOn = true; }
    void shaUpdate(const uint8_t *b, size_t n) override { if (!shaOn) shaViolations++; sha.update(b, n); }
    void shaFinish(uint8_t out[32]) override { shaFinishes++; if (!shaOn) shaViolations++; sha.finish(out); shaOn = false; }
    void onProgress(State s, uint32_t received, uint32_t size) override {
        progress.push_back(std::make_pair((int)s, size ? (int)((uint64_t)received * 100 / size) : 0));
    }
    void onActivated(const char *v) override { activated++; version = v; }
    void restart(uint16_t c) override { restarts++; restartAt = t; restartConn = c; }
    void log(const char *line) override {
        logs.push_back(line);
        if (g_verbose) printf("    t=%u  %s\n", t, line);
    }

    std::vector<std::string> heard(uint16_t c) const {
        std::vector<std::string> v;
        for (size_t i = 0; i < sent.size(); i++) if (sent[i].conn == c) v.push_back(hex(sent[i].b));
        return v;
    }
    int logsWith(const char *s) const {
        int n = 0;
        for (size_t i = 0; i < logs.size(); i++) if (logs[i].find(s) != std::string::npos) n++;
        return n;
    }
};

// The fake device plus the session under test. One tick() = one loop pass = 1 ms.
struct Rig {
    Fake    p;
    Session s;
    Rig() : s(p) {}
    void tick() { p.t++; s.tick(); }
    void ticks(uint32_t n) { while (n--) tick(); }
    void control(uint16_t c, const Bytes &f) { s.onControl(c, f.empty() ? NULL : f.data(), f.size()); }
    void data(uint16_t c, const Bytes &f) { s.onData(c, f.empty() ? NULL : f.data(), f.size()); }
};

// The app's side (BLE_OTA.md §11): BEGIN, wait for READY, then window by
// window — send `window` Data frames, wait for the ACK — then END.
struct App {
    Rig &r;
    const Image &im;
    uint16_t conn;
    uint16_t chunk = 0;
    uint8_t  window = 0;
    size_t   at = 0;        // image bytes sent
    uint32_t seq = 0;       // frames sent (the wire carries it mod 65,536)
    size_t   cursor = 0;    // into r.p.sent
    size_t   lastLen = 0;
    App(Rig &r_, const Image &im_, uint16_t conn_ = 1) : r(r_), im(im_), conn(conn_) {}

    bool poll(Bytes &out) {
        for (; cursor < r.p.sent.size(); cursor++) {
            if (r.p.sent[cursor].conn == conn) { out = r.p.sent[cursor++].b; return true; }
        }
        return false;
    }
    // The next notification for this app, ticking the piano while it waits (empty: none within maxMs).
    Bytes wait(uint32_t maxMs = 30000) {
        Bytes n;
        for (uint32_t i = 0;; i++) {
            if (poll(n)) return n;
            if (i >= maxMs) return Bytes();
            r.tick();
        }
    }
    void begin(uint8_t w = 16, const char *version = "2.1.0") { r.control(conn, beginFor(im, w, version)); }
    bool ready() {
        Bytes n = wait();
        if (n.size() != 4 || n[0] != ota::OP_READY) return false;
        chunk = (uint16_t)(n[1] | n[2] << 8);
        window = n[3];
        return true;
    }
    void frame(uint16_t payload = 0) {
        size_t len = payload ? payload : chunk;
        if (len > im.bytes.size() - at) len = im.bytes.size() - at;
        r.data(conn, dataFrame(seq++, im.bytes, at, len));
        at += len;
        lastLen = len;
    }
    void sendWindow(uint16_t payload = 0) { for (int k = 0; k < window && at < im.bytes.size(); k++) frame(payload); }
    bool ack() {
        Bytes a = wait();
        return a.size() == 5 && a[0] == ota::OP_ACK && u32(&a[1]) == at;
    }
    // `windows` windows (all when < 0), each ACK checked; a loop pass ends after each.
    bool send(int windows = -1, uint16_t payload = 0) {
        for (int w = 0; (windows < 0 || w < windows) && at < im.bytes.size(); w++) {
            sendWindow(payload);
            if (!ack()) return false;
            r.tick();
        }
        return true;
    }
    void end() { r.control(conn, Bytes(1, ota::OP_END)); }
    void abort() { r.control(conn, Bytes(1, ota::OP_ABORT)); }
    // BEGIN … OK; true when READY, every ACK, VERIFYING and OK came as they should.
    bool full(uint8_t w = 16, uint16_t payload = 0, const char *version = "2.1.0") {
        begin(w, version);
        if (!ready() || !send(-1, payload)) return false;
        end();
        Bytes v = wait(), ok = wait();
        return hex(v) == "82" && hex(ok) == "83 DC 05";
    }
};

// The ACKs a clean transfer of `size` bytes produces for the first `windows` windows (all when < 0).
static std::vector<std::string> acksFor(uint32_t size, uint16_t chunk, uint8_t window, int windows = -1) {
    std::vector<std::string> v;
    uint32_t perWindow = (uint32_t)chunk * window, at = 0;
    for (int w = 0; (windows < 0 || w < windows) && at < size; w++) {
        at = (size - at > perWindow) ? at + perWindow : size;
        uint8_t a[5] = {ota::OP_ACK, (uint8_t)at, (uint8_t)(at >> 8), (uint8_t)(at >> 16), (uint8_t)(at >> 24)};
        v.push_back(hex(a, 5));
    }
    return v;
}
static std::vector<std::string> cat(std::vector<std::string> a, const std::vector<std::string> &b) { a.insert(a.end(), b.begin(), b.end()); return a; }
static std::vector<std::string> L(std::initializer_list<const char *> l) { std::vector<std::string> v; for (const char *s : l) v.push_back(s); return v; }

// ---- checking --------------------------------------------------------------
struct Check {
    const char *name;
    int fails = 0;
    std::string summary;
    void expect(bool ok, const std::string &what) {
        if (!ok) { fails++; printf("  FAIL %s: %s\n", name, what.c_str()); }
    }
};

// What every session must satisfy, whatever the scenario: never an empty
// notification; update mode entered and left in pairs; flash and hash used
// only in order; READY only right after a verified-OFF, quiet answer; every
// return to IDLE costs exactly one flashAbort(), one leaveUpdateMode() and one
// "update abandoned" line; at most one activation and one restart.
static void invariants(Check &c, const Rig &r) {
    const Fake &p = r.p;
    c.expect(p.emptyNotifies == 0, "a NULL or empty notification was attempted");
    c.expect(p.modeViolations == 0, "update mode entered/left out of pairs, or coils polled outside it");
    c.expect(p.flashViolations == 0, "flash used out of order");
    c.expect(p.shaViolations == 0, "SHA-256 used out of order");
    c.expect(p.readyViolations == 0, "READY without a verified-OFF, quiet answer that pass");
    c.expect(p.fAbort == p.leave, "flashAbort() count " + std::to_string(p.fAbort) + " != leaveUpdateMode() count " + std::to_string(p.leave));
    c.expect(p.logsWith("— update abandoned, running firmware unchanged") == p.leave, "one \"update abandoned\" line per return to idle");
    c.expect(p.activated <= 1 && p.restarts <= 1, "activated or restarted more than once");
    c.expect(p.enter >= p.leave && p.enter - p.leave <= 1, "leaveUpdateMode() without enterUpdateMode()");
}
// The outcome: what `conn` heard, the final state, and how often the session entered and left update mode.
static void outcome(Check &c, const Rig &r, const std::vector<std::string> &want, State st, int enter, int leave, uint16_t conn = 1) {
    std::vector<std::string> got = r.p.heard(conn);
    c.expect(got == want, "conn " + std::to_string(conn) + " heard " + join(got) + ", want " + join(want));
    c.expect(r.s.state() == st, "final state " + std::to_string((int)r.s.state()) + ", want " + std::to_string((int)st));
    c.expect(r.p.enter == enter && r.p.leave == leave && r.p.fAbort == leave,
             "enter/leave/flashAbort " + std::to_string(r.p.enter) + "/" + std::to_string(r.p.leave) + "/" + std::to_string(r.p.fAbort) +
             ", want " + std::to_string(enter) + "/" + std::to_string(leave) + "/" + std::to_string(leave));
    if (st == ota::IDLE) c.expect(r.s.owner() == ota::NO_CONN && !r.s.active() && !r.s.acceptsData(), "idle reads idle");
    invariants(c, r);
}
static std::string tail(const Rig &r, uint16_t conn = 1, size_t n = 3) {
    std::vector<std::string> v = r.p.heard(conn);
    std::string s;
    for (size_t i = v.size() > n ? v.size() - n : 0; i < v.size(); i++) s += (s.empty() ? "" : " · ") + v[i];
    return (v.size() > n ? "… " : "") + s;
}

// ---- scenarios -------------------------------------------------------------
// §5's image (no tag); the same size tagged "2.1.0" (fake-signed) for transfers that must
// complete; a 4,000-byte tagged one (one window) for the quick refusals.
static Image g_doc, g_docTagged, g_small;

static void scSha(Check &c) {
    uint8_t d[32];
    sha256((const uint8_t *)"", 0, d);
    c.expect(hex(d, 32, false) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256(\"\")");
    sha256((const uint8_t *)"abc", 3, d);
    c.expect(hex(d, 32, false) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(\"abc\")");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256((const uint8_t *)m, strlen(m), d);
    c.expect(hex(d, 32, false) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 of the 448-bit message");
    Bytes a(1000000, 'a');
    Sha256 s;
    s.start();
    for (size_t i = 0; i < a.size(); i += 777) s.update(&a[i], a.size() - i < 777 ? a.size() - i : 777);
    s.finish(d);
    c.expect(hex(d, 32, false) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a million 'a' in odd pieces");
    c.summary = "FIPS 180-4 vectors: \"\", \"abc\", 448-bit, 10^6 x 'a'";
}

// §5's flow for a 991,232-byte image: BEGIN, READY 81 FA 00 10, 3,965 frames in
// windows of 16 with every ACK, END, 82. Data 0, 1 and 3964 are checked as §5
// prints them (the tagged image differs only at byte 247,808, in frame 991).
static void workedFlow(Check &c, Rig &r, App &a, const Bytes &begin) {
    const Image &im = a.im;
    r.control(1, begin);
    c.expect(r.s.state() == ota::PREPARING && r.p.enter == 1 && r.p.sent.empty(), "BEGIN: PREPARING, piano stopped, nothing sent yet");
    c.expect(a.ready() && r.p.heard(1)[0] == "81 FA 00 10", "READY 81 FA 00 10");
    std::vector<std::string> acks;
    int frames = 0;
    while (a.at < im.bytes.size()) {
        for (int k = 0; k < a.window && a.at < im.bytes.size(); k++) {
            size_t len = im.bytes.size() - a.at < 250 ? im.bytes.size() - a.at : 250;
            Bytes f = dataFrame(a.seq, im.bytes, a.at, len);
            if (a.seq == 0) c.expect(f.size() == 252 && hex(&f[0], 8) == "00 00 E9 F0 F7 FE 05 0C" && hex(&f[250], 2) == "B1 B8", "Data 0 as §5 prints it");
            if (a.seq == 1) c.expect(hex(&f[0], 4) == "01 00 BF C6", "Data 1 as §5 prints it");
            if (a.seq == 3964) c.expect(f.size() == 234 && hex(&f[0], 8) == "7C 0F 91 98 9F A6 AD B4" && hex(&f[232], 2) == "DB E2", "Data 3964 as §5 prints it");
            r.data(1, f);
            a.seq++;
            a.at += len;
            a.lastLen = len;
            frames++;
        }
        acks.push_back(hex(a.wait()));
        r.tick();
    }
    c.expect(frames == 3965 && a.lastLen == 232, "3,965 frames, the last carrying 232 bytes");
    c.expect(acks.size() == 248 && acks[0] == "85 A0 0F 00 00" && acks[246] == "85 60 13 0F 00" && acks.back() == "85 00 20 0F 00",
             "ACKs: after seq 15 85 A0 0F 00 00, after seq 3951 85 60 13 0F 00, last 85 00 20 0F 00");
    c.expect(acks == acksFor(kDocSize, 250, 16), "every ACK is the bytes so far");
    a.end();
    c.expect(hex(a.wait(0)) == "82" && r.s.state() == ota::VERIFYING && r.p.shaFinishes == 0 && r.p.fEnd == 0,
             "END: 82 at once, the check waits for the next pass");
    r.tick();
    c.expect(r.p.fBytes == kDocSize, "flash got every byte");
    uint8_t d[32];
    Sha256 fs = r.p.fSha;
    fs.finish(d);
    c.expect(memcmp(d, im.sha, 32) == 0, "flash got exactly the image, in order");
    c.expect(r.p.sigCalls == 2 && r.p.sigDigests[0] == Bytes(im.sha, im.sha + 32) && r.p.sigDigests[1] == Bytes(im.sha, im.sha + 32),
             "signature checked at BEGIN over BEGIN's digest, after END over the digest of the bytes received");
}

// §5 byte for byte. Its image predates the version tag, so a 2.1.0 piano
// takes every frame, checks the hash and the (real) signature, then refuses
// it: 82, E1 0C (ERR 12, no tag) — the slot is never activated.
static void scWorkedExample(Check &c) {
    const Image &im = g_doc;
    c.expect(hex(im.sha, 32, false) == kDocSha, "the simulator's SHA-256 of the image is §5's 6d872b96…12c6");
    Bytes sigB64 = base64(kDocSigB64);
    c.expect(sigB64.size() == 64 && memcmp(sigB64.data(), im.sig, 64) == 0, "§10's manifest sig is §5's BEGIN bytes 37..100");
    Bytes begin = beginFor(im, 16, "2.1.0");
    c.expect(begin == unhex(kDocBegin), "BEGIN built from the fields is §5's 118 bytes");

    Rig r;                                        // the piano runs 2.0.2: BEGIN's 2.1.0 is newer
    App a(r, im);
    workedFlow(c, r, a, begin);
    c.expect(hex(a.wait(0)) == "E1 0C" && r.s.state() == ota::IDLE, "then E1 0C: the image has no version tag");
    c.expect(r.p.fEnd == 0 && r.p.activated == 0, "slot never activated");
    r.ticks(5000);
    c.expect(r.p.restarts == 0, "no restart");
    c.expect(r.p.logsWith("ERR 12 (not newer): the image carries no version tag") == 1, "serial: no version tag");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16)), L({"82", "E1 0C"})), ota::IDLE, 1, 1);
    c.summary = "§5's frames unchanged: 81 FA 00 10 · 3965 frames · 248 ACKs … 85 00 20 0F 00 · 82 · E1 0C (hash + real sig pass, no version tag) · sha 6d872b96…12c6";
}

// The same flow with an image of §5's size that carries the tag "2.1.0":
// OK 83 DC 05 on the pass after END, restart 1,500 ms later.
static void scWorkedExampleTagged(Check &c) {
    const Image &im = g_docTagged;
    Rig r;
    App a(r, im);
    workedFlow(c, r, a, beginFor(im, 16, "2.1.0"));
    c.expect(hex(a.wait(0)) == "83 DC 05" && r.s.state() == ota::REBOOTING, "OK 83 DC 05 on the next pass");
    c.expect(r.p.activated == 1 && r.p.version == "2.1.0" && r.p.fEnd == 1, "flashEnd() then onActivated(\"2.1.0\")");
    const uint32_t okAt = r.p.t;
    r.ticks(1499);
    c.expect(r.p.restarts == 0, "no restart before 1,500 ms");
    r.tick();
    c.expect(r.p.restarts == 1 && r.p.restartAt == okAt + 1500 && r.p.restartConn == 1, "restart(owner) 1,500 ms after OK");
    r.ticks(5000);
    c.expect(r.p.restarts == 1, "restart() called once");
    std::vector<std::pair<int, int> > want;
    want.push_back(std::make_pair((int)ota::PREPARING, 0));
    for (int pct = 0; pct <= 100; pct++) want.push_back(std::make_pair((int)ota::RECEIVING, pct));
    want.push_back(std::make_pair((int)ota::VERIFYING, 100));
    want.push_back(std::make_pair((int)ota::REBOOTING, 100));
    c.expect(r.p.progress == want, "onProgress: PREPARING, every whole percent 0..100 once, VERIFYING, REBOOTING");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16)), L({"82", "83 DC 05"})), ota::REBOOTING, 1, 0);
    c.summary = "tag 2.1.0 at byte 247,808, piano on 2.0.2: same frames and ACKs · 82 · 83 DC 05 · restart +1500 ms";
}

static void scErr1NotVerified(Check &c) {
    Rig r;
    r.p.coilsOff = false;                        // a coil that never verifies off
    App a(r, g_small);
    a.begin();
    r.ticks(999);
    c.expect(r.s.state() == ota::PREPARING && r.p.sent.empty(), "still waiting at +999 ms");
    r.tick();
    c.expect(r.p.fBegin == 0, "the slot is never opened");
    c.expect(r.p.logsWith("coils not verified off within 1 s") == 1, "logged");
    outcome(c, r, L({"E1 01"}), ota::IDLE, 1, 1);

    Rig r2;                                      // verified OFF at +999 ms: just in time
    r2.p.coilsOffFrom = r2.p.t + 999;
    App a2(r2, g_small);
    a2.begin();
    c.expect(a2.ready(), "verified at +999 ms: READY");
    c.summary = "E1 01 at +1000 ms (READY when verified at +999) · slot never opened · leave 1 · flashAbort 1";
}

static void scErr1NotQuiet(Check &c) {
    Rig r;
    r.p.lastEnergize = r.p.t + 1 - 499;          // a coil energized 499 ms before the first check
    App a(r, g_small);
    a.begin();
    r.tick();
    c.expect(r.p.fBegin == 0, "the slot is never opened");
    outcome(c, r, L({"E1 01"}), ota::IDLE, 1, 1);

    Rig r2;                                      // 500 ms: quiet enough
    r2.p.lastEnergize = r2.p.t + 1 - 500;
    App a2(r2, g_small);
    a2.begin();
    c.expect(a2.ready(), "500 ms after the last energize: READY");
    Rig r3;                                      // nothing energized since boot
    App a3(r3, g_small);
    a3.begin();
    c.expect(a3.ready(), "lastEnergizeMs() 0: READY");
    c.summary = "energized 499 ms before the check: E1 01 · 500 ms or never: READY · leave 1 · flashAbort 1";
}

static void scErr2TooLarge(Check &c) {
    Rig r;
    Image big;                                   // §13 run 6: size 7,000,000
    memcpy(big.sha, g_small.sha, 32);
    memcpy(big.sig, g_small.sig, 64);
    r.control(1, beginFrame(7000000, big.sha, big.sig, "2.1.0", 16));
    c.expect(r.p.sigCalls == 0, "refused before the signature check");
    outcome(c, r, L({"E1 02"}), ota::IDLE, 0, 0);

    Rig r2;                                      // exactly the slot: accepted
    uint8_t sha[32] = {7}, sig[64];
    fakeSign(sha, sig);
    r2.control(1, beginFrame(6553600, sha, sig, "2.1.0", 16));
    c.expect(r2.s.state() == ota::PREPARING, "size == slot (6,553,600): accepted");
    c.summary = "size 7,000,000: E1 02 before the signature, nothing touched · 6,553,600 accepted";
}

static void scErr2FlashBegin(Check &c) {
    Rig r;
    r.p.failBegin = ota::ERR_TOO_LARGE;          // Update.begin(): UPDATE_ERROR_SIZE
    App a(r, g_small);
    a.begin();
    r.tick();
    c.expect(r.p.fBegin == 1 && r.p.shaStarts == 0, "flashBegin() refused, no hash started");
    outcome(c, r, L({"E1 02"}), ota::IDLE, 1, 1);
    c.summary = "flashBegin() -> 2: E1 02 · leave 1 · flashAbort 1";
}

static void scErr3Pending(Check &c) {
    Rig r;
    r.p.pendingImage = true;                     // §13 run 9: BEGIN while PENDING_VERIFY
    App a(r, g_small);
    a.begin();
    r.control(1, Bytes(5, ota::OP_BEGIN));       // checked before the frame itself
    outcome(c, r, L({"E1 03", "E1 03"}), ota::IDLE, 0, 0);
    c.summary = "running image pending: E1 03 (before any other check), nothing touched";
}

static void scErr3SecondBeginPreparing(Check &c) {
    Rig r;
    r.p.coilsOff = false;
    App a(r, g_small);
    a.begin();
    r.tick();
    a.begin();                                   // §13 run 6: BEGIN twice
    r.p.coilsOff = true;
    r.ticks(50);
    outcome(c, r, L({"E1 03"}), ota::IDLE, 1, 1);
    c.summary = "second BEGIN in PREPARING: E1 03, session over · leave 1 · flashAbort 1";
}

static void scErr3SecondBeginReceiving(Check &c) {
    Rig r;
    App a(r, g_docTagged);
    a.begin();
    c.expect(a.ready() && a.send(2), "two windows in");
    a.begin();
    a.frame();                                   // the session is over: no reply
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16, 2)), L({"E1 03"})), ota::IDLE, 1, 1);
    c.summary = "second BEGIN in RECEIVING: " + tail(r) + " · leave 1 · flashAbort 1";
}

static void scErr4BeginLength(Check &c) {
    Rig r;
    Bytes b = beginFor(g_small);
    r.control(1, Bytes(b.begin(), b.end() - 1));   // 117
    Bytes longer = b;
    longer.push_back(0);
    r.control(1, longer);                          // 119
    r.control(1, Bytes(1, ota::OP_BEGIN));         // opcode only
    c.expect(r.p.enter == 0 && r.p.sigCalls == 0, "nothing touched");
    r.control(1, b);                               // and the next good BEGIN is taken
    c.expect(r.s.state() == ota::PREPARING, "a good BEGIN afterwards is accepted");
    outcome(c, r, L({"E1 04", "E1 04", "E1 04"}), ota::PREPARING, 1, 0);
    c.summary = "117, 119 and 1 bytes: E1 04 each, nothing touched";
}

static void scErr4BeginSize0(Check &c) {
    Rig r;
    r.control(1, beginFrame(0, g_small.sha, g_small.sig, "2.1.0", 16));
    outcome(c, r, L({"E1 04"}), ota::IDLE, 0, 0);
    c.summary = "size 0: E1 04, nothing touched";
}

static void scErr4BeginWindow(Check &c) {
    Rig r;
    r.control(1, beginFor(g_small, 0));
    r.control(1, beginFor(g_small, 33));
    r.control(1, beginFor(g_small, 255));
    outcome(c, r, L({"E1 04", "E1 04", "E1 04"}), ota::IDLE, 0, 0);
    Rig r1, r32;
    App a1(r1, g_small), a32(r32, g_small);
    a1.begin(1);
    a32.begin(32);
    c.expect(a1.ready() && a32.ready() && r1.p.heard(1)[0] == "81 FA 00 01" && r32.p.heard(1)[0] == "81 FA 00 20",
             "windows 1 and 32 accepted, READY carries them");
    c.summary = "window 0, 33, 255: E1 04 · 1 and 32 accepted";
}

static void scErr4BeginVersion(Check &c) {
    Rig r;
    const char *bad[] = {"", "2.1 0", "2.1.0_rc", "\xC3\xA9", "v2/1"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) r.control(1, beginFor(g_small, 16, bad[i]));
    Bytes junk = beginFor(g_small, 16, "2.1.0");
    junk[101 + 6] = 'x';                          // "2.1.0\0x": a byte after the first NUL
    r.control(1, junk);
    Bytes lead = beginFor(g_small, 16, "2.1.0");
    lead[101] = 0;                                // "\0.1.0": empty, then junk
    r.control(1, lead);
    outcome(c, r, L({"E1 04", "E1 04", "E1 04", "E1 04", "E1 04", "E1 04", "E1 04"}), ota::IDLE, 0, 0);

    Rig r16;                                      // 16 characters, no NUL at all: the whole field
    App a16(r16, g_small);
    c.expect(a16.full(16, 0, "2.1.0-0123456789") && r16.p.version == "2.1.0-0123456789", "16-character version kept whole");
    Rig rp;
    App ap(rp, g_small);
    c.expect(ap.full(16, 0, "2.1.0-rc.1+ab") && rp.p.version == "2.1.0-rc.1+ab", "every allowed punctuation");
    Rig rn;                                       // a well-formed field that is no version: past the frame
    rn.control(1, beginFor(g_small, 16, "0123456789abcDEF"));   // check (4), refused as not newer (12)
    outcome(c, rn, L({"E1 0C"}), ota::IDLE, 0, 0);
    c.summary = "empty, space, '_', non-ASCII, '/', byte after NUL, leading NUL: E1 04 · 16 chars and . + - accepted · \"0123456789abcDEF\": E1 0C";
}

static void scErr4SeqGap(Check &c) {
    Rig r;                                        // §13 run 6: Data seq 5 first
    App a(r, g_docTagged);
    a.begin();
    a.ready();
    r.data(1, dataFrame(5, g_docTagged.bytes, 0, 250));
    c.expect(r.p.fWrites == 0, "nothing written");
    outcome(c, r, L({"81 FA 00 10", "E1 04"}), ota::IDLE, 1, 1);

    Rig r2;                                       // a gap after a full window
    App a2(r2, g_docTagged);
    a2.begin();
    a2.ready();
    a2.send(1);
    r2.data(1, dataFrame(17, g_docTagged.bytes, 4000, 250));
    c.expect(r2.p.heard(1) == L({"81 FA 00 10", "85 A0 0F 00 00", "E1 04"}), "seq 17 after 15: E1 04");
    Rig r3;                                       // a repeat
    App a3(r3, g_docTagged);
    a3.begin();
    a3.ready();
    a3.send(1);
    r3.data(1, dataFrame(15, g_docTagged.bytes, 4000, 250));
    c.expect(r3.p.heard(1) == L({"81 FA 00 10", "85 A0 0F 00 00", "E1 04"}), "seq 15 again: E1 04");
    c.summary = "seq 5 first: E1 04 · a skipped and a repeated seq: E1 04";
}

static void scErr4ShortData(Check &c) {
    const size_t lens[] = {2, 1, 0};              // payload 0, half a seq, nothing
    for (size_t i = 0; i < 3; i++) {
        Rig r;
        App a(r, g_small);
        a.begin();
        a.ready();
        r.data(1, Bytes(lens[i], 0));
        outcome(c, r, L({"81 FA 00 10", "E1 04"}), ota::IDLE, 1, 1);
    }
    c.summary = "Data of 2, 1 and 0 bytes: E1 04";
}

static void scErr4PayloadTooBig(Check &c) {
    Rig r;
    App a(r, g_docTagged);
    a.begin();
    a.ready();
    a.frame(250);                                 // the limit itself is fine
    r.data(1, dataFrame(1, g_docTagged.bytes, 250, 251));
    outcome(c, r, L({"81 FA 00 10", "E1 04"}), ota::IDLE, 1, 1);

    Rig r2;
    r2.p.links[0].mtu = 23;
    App a2(r2, g_docTagged);
    a2.begin();
    a2.ready();
    a2.frame(18);
    r2.data(1, dataFrame(1, g_docTagged.bytes, 18, 19));
    c.expect(r2.p.heard(1) == L({"81 12 00 10", "E1 04"}), "MTU 23: 18 fine, 19 E1 04");
    c.summary = "MTU 255: 250 fine, 251 E1 04 · MTU 23: 18 fine, 19 E1 04";
}

static void scErr4PastSize(Check &c) {
    Rig r;
    Image im = makeImage(900);
    App a(r, im);
    a.begin();
    a.ready();
    a.frame(); a.frame(); a.frame();              // 750 of 900
    r.data(1, dataFrame(3, Bytes(1000, 0), 0, 250));   // 250 more: 100 past the end
    c.expect(r.p.fBytes == 750, "the frame past the end is not written");
    outcome(c, r, L({"81 FA 00 10", "E1 04"}), ota::IDLE, 1, 1);
    c.summary = "a frame running past size: E1 04, not written";
}

static void scErr4DataDropped(Check &c) {
    Rig r;
    r.p.coilsOff = false;
    App a(r, g_docTagged);
    r.s.onDataDropped();                          // idle: nothing
    a.begin();
    r.s.onDataDropped();                          // PREPARING: nothing
    r.p.coilsOff = true;
    a.ready();
    a.send(3);
    r.s.onDataDropped();                          // RECEIVING: the session is over
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16, 3)), L({"E1 04"})), ota::IDLE, 1, 1);
    c.summary = "queue overflow in RECEIVING: E1 04 (ignored while idle or preparing)";
}

static void scErr4EarlyEnd(Check &c) {
    Rig r;
    App a(r, g_docTagged);
    a.begin();
    a.ready();
    a.send(2);
    a.end();
    r.tick();
    c.expect(r.p.fEnd == 0 && r.p.shaFinishes == 0, "nothing verified");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16, 2)), L({"E1 04"})), ota::IDLE, 1, 1);
    Rig r2;
    App a2(r2, g_small);
    a2.begin();
    a2.ready();
    a2.end();
    c.expect(r2.p.heard(1) == L({"81 FA 00 10", "E1 04"}), "END right after READY: E1 04");
    c.summary = "END after 8,000 of 991,232 bytes: E1 04 · END with 0 bytes: E1 04";
}

static void scErr4UnknownOpcode(Check &c) {
    Rig r;
    r.control(1, Bytes(1, 0x07));                 // idle: answered, nothing else
    outcome(c, r, L({"E1 04"}), ota::IDLE, 0, 0);
    Rig r2;
    App a2(r2, g_docTagged);
    a2.begin();
    a2.ready();
    a2.send(1);
    r2.control(1, Bytes(1, 0x86));                // in RECEIVING: the session is over
    outcome(c, r2, L({"81 FA 00 10", "85 A0 0F 00 00", "E1 04"}), ota::IDLE, 1, 1);
    Rig r3;
    App a3(r3, g_small);
    a3.begin();
    a3.ready();
    a3.send();
    a3.end();
    r3.control(1, Bytes(1, 0x07));                // after END: ignored
    r3.tick();
    outcome(c, r3, L({"81 FA 00 10", "85 A0 0F 00 00", "82", "83 DC 05"}), ota::REBOOTING, 1, 0);
    c.summary = "unknown opcode: E1 04 to the sender while idle · ends the session in RECEIVING · ignored after END";
}

static void scErr4EmptyControl(Check &c) {
    Rig r;
    r.control(1, Bytes());
    outcome(c, r, L({"E1 04"}), ota::IDLE, 0, 0);
    Rig r2;
    r2.p.coilsOff = false;
    App a2(r2, g_small);
    a2.begin();
    r2.control(1, Bytes());                       // PREPARING: the session is over
    outcome(c, r2, L({"E1 04"}), ota::IDLE, 1, 1);
    Rig r3;
    App a3(r3, g_small);
    c.expect(a3.full(), "a clean transfer");
    r3.control(1, Bytes());                       // REBOOTING: ignored
    r3.ticks(1500);
    c.expect(r3.p.restarts == 1, "the restart still comes");
    outcome(c, r3, L({"81 FA 00 10", "85 A0 0F 00 00", "82", "83 DC 05"}), ota::REBOOTING, 1, 0);
    c.summary = "empty Control: E1 04 while idle · ends PREPARING · ignored after END";
}

static void scErr5Hash(Check &c) {
    Rig r;                                        // §13 run 1: one image byte flipped, original sha/sig
    Image bent = g_docTagged;
    bent.bytes[500000] ^= 1;
    memcpy(bent.sha, g_docTagged.sha, 32);
    memcpy(bent.sig, g_docTagged.sig, 64);
    App a(r, bent);
    c.expect(!a.full() , "the transfer runs to END");
    c.expect(r.p.sigCalls == 1 && r.p.fEnd == 0 && r.p.activated == 0 && r.p.restarts == 0,
             "digest checked first: no second signature check, slot never activated, no restart");
    c.expect(r.p.logsWith("ERR 5 (hash mismatch)") == 1, "serial: hash mismatch");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16)), L({"82", "E1 05"})), ota::IDLE, 1, 1);
    c.summary = tail(r) + " · slot never activated · leave 1 · flashAbort 1";
}

static void scErr6Begin(Check &c) {
    Rig r;                                        // §13 run 2: one bit of sig flipped
    Image im = g_docTagged;
    im.sig[10] ^= 0x04;
    App a(r, im);
    a.begin();
    r.ticks(100);
    c.expect(r.p.sigCalls == 1 && r.p.sigDigests[0] == Bytes(im.sha, im.sha + 32), "checked once, over BEGIN's digest");
    outcome(c, r, L({"E1 06"}), ota::IDLE, 0, 0);
    c.summary = "E1 06 straight after BEGIN, no READY · enterUpdateMode() never called: the music never stopped";
}

static void scErr6AfterEnd(Check &c) {
    Rig r;
    r.p.failSigCall = 2;                          // the check after END fails
    App a(r, g_docTagged);
    c.expect(!a.full(), "the transfer runs to END");
    c.expect(r.p.sigCalls == 2 && r.p.sigDigests[1] == Bytes(g_docTagged.sha, g_docTagged.sha + 32), "second check over the received digest");
    c.expect(r.p.fEnd == 0 && r.p.activated == 0 && r.p.restarts == 0, "slot never activated, no restart");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16)), L({"82", "E1 06"})), ota::IDLE, 1, 1);
    c.summary = tail(r) + " · slot never activated · leave 1 · flashAbort 1";
}

// ERR 7 or 8 from each of the three flash calls.
enum FlashCall { BEGIN_CALL, WRITE_CALL, END_CALL };
static void flashErr(Check &c, FlashCall call, uint8_t code) {
    Rig r;
    char e[8];
    snprintf(e, sizeof e, "E1 %02X", code);
    App a(r, g_docTagged);
    if (call == BEGIN_CALL) {
        r.p.failBegin = code;
        a.begin();
        r.tick();
        c.expect(r.p.shaStarts == 0, "no hash started");
        outcome(c, r, L({e}), ota::IDLE, 1, 1);
        c.summary = std::string("flashBegin() -> ") + std::to_string(code) + ": " + e + " in PREPARING · leave 1 · flashAbort 1";
    } else if (call == WRITE_CALL) {
        r.p.failWriteCode = code;
        r.p.failWriteAtByte = 100000;             // frame 400, the first of window 26
        a.begin();
        a.ready();
        a.send(25);
        a.sendWindow();
        c.expect(r.p.fBytes == 100000, "nothing written past the failure");
        outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16, 25)), L({e})), ota::IDLE, 1, 1);
        c.summary = std::string("flashWrite() -> ") + std::to_string(code) + " at byte 100,000: " + tail(r) + " · leave 1 · flashAbort 1";
    } else {
        r.p.failEnd = code;
        c.expect(!a.full(), "the transfer runs to END");
        c.expect(r.p.activated == 0 && r.p.restarts == 0, "not activated, no restart");
        outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16)), L({"82", e})), ota::IDLE, 1, 1);
        c.summary = std::string("flashEnd() -> ") + std::to_string(code) + ": " + tail(r) + " · leave 1 · flashAbort 1";
    }
}
static void scErr7FlashBegin(Check &c) { flashErr(c, BEGIN_CALL, ota::ERR_FLASH); }
static void scErr7FlashWrite(Check &c) { flashErr(c, WRITE_CALL, ota::ERR_FLASH); }
static void scErr7FlashEnd(Check &c)   { flashErr(c, END_CALL, ota::ERR_FLASH); }
static void scErr8FlashBegin(Check &c) { flashErr(c, BEGIN_CALL, ota::ERR_IMAGE); }
static void scErr8FlashWrite(Check &c) { flashErr(c, WRITE_CALL, ota::ERR_IMAGE); }
static void scErr8FlashEnd(Check &c)   { flashErr(c, END_CALL, ota::ERR_IMAGE); }

static void scErr9Timeout(Check &c) {
    Rig r;                                        // §13 run 5: stop sending after 5 windows
    App a(r, g_docTagged);
    a.begin();
    a.ready();
    a.send(5);
    const uint32_t last = r.p.t - 1;              // the last frame arrived before that pass's tick
    r.ticks(9999 - 1);
    c.expect(r.s.state() == ota::RECEIVING, "still receiving at +9,999 ms");
    r.tick();
    c.expect(r.p.sent.back().t == last + 10000, "E1 09 exactly 10,000 ms after the last valid frame");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16, 5)), L({"E1 09"})), ota::IDLE, 1, 1);

    Rig r2;                                       // a frame at +9,999 ms keeps it alive
    App a2(r2, g_docTagged);
    a2.begin();
    a2.ready();
    a2.send(1);
    r2.ticks(9998);
    a2.frame();
    r2.ticks(9999);
    c.expect(r2.s.state() == ota::RECEIVING, "a frame at +9,999 ms restarts the 10 s");
    r2.tick();
    c.expect(r2.s.state() == ota::IDLE && hex(r2.p.sent.back().b) == "E1 09", "then E1 09 10 s after it");
    c.summary = "5 windows then silence: E1 09 at +10,000 ms · a frame at +9,999 ms restarts the clock";
}

static void scErr10SafeMode(Check &c) {
    Rig r;
    r.p.safeMode = true;
    App a(r, g_small);
    a.begin();
    outcome(c, r, L({"E1 0A"}), ota::IDLE, 0, 0);
    c.summary = "crash-loop safe mode: E1 0A, nothing touched";
}

static void scBeginCheckOrder(Check &c) {
    struct Case { const char *what; bool pending, untrusted, safe; const char *version; uint32_t size; uint8_t window; bool badSig; const char *want; };
    const Case cases[] = {
        {"pending + bad window + untrusted + safe mode", true, true, true, "2.0.1", 4000, 0, true, "E1 03"},
        {"bad window + untrusted + safe mode", false, true, true, "2.0.1", 4000, 0, false, "E1 04"},
        {"untrusted + safe mode + older + too large", false, true, true, "2.0.1", 7000000, 16, true, "E1 0B"},
        {"safe mode + older + too large + bad sig", false, false, true, "2.0.1", 7000000, 16, true, "E1 0A"},
        {"older + too large + bad sig", false, false, false, "2.0.1", 7000000, 16, true, "E1 0C"},
        {"too large + bad sig", false, false, false, "2.1.0", 7000000, 16, true, "E1 02"},
        {"bad sig only", false, false, false, "2.1.0", 4000, 16, true, "E1 06"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const Case &k = cases[i];
        Rig r;
        r.p.pendingImage = k.pending;
        r.p.safeMode = k.safe;
        if (k.untrusted) r.p.untrusted.push_back(1);
        uint8_t sig[64];
        memcpy(sig, g_small.sig, 64);
        if (k.badSig) sig[0] ^= 1;
        r.control(1, beginFrame(k.size, g_small.sha, sig, k.version, k.window));
        c.expect(r.p.heard(1) == L({k.want}), std::string(k.what) + ": want " + k.want + ", heard " + join(r.p.heard(1)));
        c.expect(r.p.sigCalls == (strcmp(k.want, "E1 06") == 0 ? 1 : 0), std::string(k.what) + ": signature checked only when everything else passed");
        invariants(c, r);
    }
    c.summary = "pending (3) > frame (4) > not authorised (11) > safe mode (10) > not newer (12) > size (2) > signature (6)";
}

static void scAbortIdle(Check &c) {
    Rig r;
    r.control(1, Bytes(1, ota::OP_ABORT));
    c.expect(r.p.logsWith("ABORT while idle") == 1, "logged");
    outcome(c, r, L({"84"}), ota::IDLE, 0, 0);
    c.summary = "84 to the sender, nothing touched";
}

static void scAbortPreparing(Check &c) {
    Rig r;
    r.p.coilsOff = false;
    App a(r, g_small);
    a.begin();
    r.ticks(10);
    a.abort();
    r.p.coilsOff = true;
    r.ticks(100);
    c.expect(r.p.fBegin == 0, "the slot was never opened");
    outcome(c, r, L({"84"}), ota::IDLE, 1, 1);
    c.summary = "84, no READY afterwards · leave 1 · flashAbort 1";
}

static void scAbortReceiving(Check &c) {
    Rig r;                                        // §13 run 3: ABORT after 20 windows
    App a(r, g_docTagged);
    a.begin();
    a.ready();
    a.send(20);
    a.abort();
    a.frame();                                    // a stale frame after it: no reply
    r.ticks(100);
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16, 20)), L({"84"})), ota::IDLE, 1, 1);
    c.summary = "after 20 windows: " + tail(r) + " · stale frame ignored · leave 1 · flashAbort 1";
}

static void scAbortAfterEnd(Check &c) {
    Rig r;
    App a(r, g_small);
    a.begin();
    a.ready();
    a.send();
    a.end();
    a.abort();                                    // VERIFYING: ignored
    r.tick();
    a.abort();                                    // REBOOTING: ignored
    r.ticks(1500);
    c.expect(r.p.restarts == 1 && r.p.activated == 1, "verified, activated and restarted anyway");
    outcome(c, r, L({"81 FA 00 10", "85 A0 0F 00 00", "82", "83 DC 05"}), ota::REBOOTING, 1, 0);
    c.summary = "ABORT in VERIFYING and REBOOTING ignored: 82 · 83 DC 05 · restart";
}

static void scOwnerGonePreparing(Check &c) {
    Rig r;
    r.p.coilsOff = false;
    App a(r, g_small);
    a.begin();
    r.ticks(5);
    r.p.links[0].alive = false;
    r.tick();
    c.expect(r.p.logsWith("the app's link dropped — update abandoned") == 1, "serial: update abandoned");
    outcome(c, r, L({}), ota::IDLE, 1, 1);
    c.summary = "link lost in PREPARING: no notification · leave 1 · flashAbort 1";
}

static void scOwnerGoneReceiving(Check &c) {
    Rig r;                                        // §13 run 4: disconnect after 20 windows
    App a(r, g_docTagged);
    a.begin();
    a.ready();
    a.send(20);
    r.p.links[0].alive = false;
    r.tick();
    outcome(c, r, cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16, 20)), ota::IDLE, 1, 1);
    r.p.links.push_back(Fake::Link{2, true, 255, false}); // reconnect: a new update succeeds
    App b(r, g_docTagged, 2);
    c.expect(b.full(), "a new update over the new link succeeds");
    c.expect(r.p.enter == 2 && r.p.leave == 1 && r.p.activated == 1, "second session activated");
    c.summary = "link lost after 20 windows: no notification · leave 1 · flashAbort 1 · next update succeeds";
}

static void scOwnerGoneAfterEnd(Check &c) {
    Rig r;
    App a(r, g_small);
    a.begin();
    a.ready();
    a.send();
    a.end();                                      // 82 goes out ...
    r.p.links[0].alive = false;                   // ... then the link drops
    r.tick();                                     // verifies anyway
    r.ticks(1500);
    c.expect(r.p.activated == 1 && r.p.restarts == 1 && r.p.restartConn == 1, "verified, activated, restarted");
    c.expect(r.p.notifyRefused == 1, "OK tried once, then dropped with its link — not retried every pass");
    outcome(c, r, L({"81 FA 00 10", "85 A0 0F 00 00", "82"}), ota::REBOOTING, 1, 0);
    c.summary = "link lost after END: still verifies, activates, restarts at +1500 ms (OK dropped with the link)";
}

static void scForeignConn(Check &c) {
    Rig r;
    r.p.links.push_back(Fake::Link{2, true, 255, false});
    r.control(2, Bytes(1, ota::OP_ABORT));        // idle: conn 2 is answered
    r.p.coilsOff = false;
    App a(r, g_docTagged);
    a.begin();
    const Bytes begin2 = beginFor(g_docTagged);
    std::vector<Bytes> noise;
    noise.push_back(begin2);
    noise.push_back(Bytes(1, ota::OP_END));
    noise.push_back(Bytes(1, ota::OP_ABORT));
    noise.push_back(Bytes(1, 0x07));
    noise.push_back(Bytes());
    for (size_t i = 0; i < noise.size(); i++) r.control(2, noise[i]);   // PREPARING
    r.p.coilsOff = true;
    a.ready();
    a.send(3);
    for (size_t i = 0; i < noise.size(); i++) r.control(2, noise[i]);   // RECEIVING
    r.data(2, dataFrame(a.seq, g_docTagged.bytes, a.at, 250));               // the expected seq, but not the owner's
    r.data(2, dataFrame(0, g_docTagged.bytes, 0, 250));
    c.expect(r.s.received() == 3 * 4000, "foreign Data neither counted nor written");
    c.expect(a.send(), "the owner's next frame is still the expected one");
    a.end();
    for (size_t i = 0; i < noise.size(); i++) r.control(2, noise[i]);   // VERIFYING
    r.tick();
    for (size_t i = 0; i < noise.size(); i++) r.control(2, noise[i]);   // REBOOTING
    r.ticks(1500);
    c.expect(r.p.heard(2) == L({"84"}), "conn 2 heard only its idle ABORTED: " + join(r.p.heard(2)));
    c.expect(r.p.restarts == 1 && r.p.restartConn == 1, "restart for the owner");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16)), L({"82", "83 DC 05"})), ota::REBOOTING, 1, 0);
    c.summary = "another connection's BEGIN/END/ABORT/unknown/empty/Data ignored in every state, no reply; owner completes";
}

static void scNotifyRetry(Check &c) {
    Rig r;
    App a(r, g_docTagged);
    r.p.refuseFrom = r.p.t;                       // READY refused for 50 ms
    r.p.refuseTo = r.p.t + 50;
    a.begin();
    c.expect(a.ready() && r.p.sent[0].t == r.p.refuseTo, "READY delivered when the link takes it (+50 ms)");
    a.send(100);
    r.p.refuseNext = 3;                           // window 101's ACK refused three times
    c.expect(a.send(1), "ACK delivered on the 4th try");
    r.p.refuseFrom = r.p.t;                       // a 2 ms hiccup every 64 ms for the rest of the transfer
    r.p.refuseTo = r.p.t + 2;
    while (a.at < g_docTagged.bytes.size()) {
        if (r.p.t % 64 == 0) { r.p.refuseFrom = r.p.t; r.p.refuseTo = r.p.t + 2; }
        a.sendWindow();
        if (!a.ack()) { c.expect(false, "an ACK lost"); break; }
        r.tick();
    }
    r.p.refuseTo = 0;                             // (no hiccup from here on)
    r.p.refuseNext = 2;                           // END: 82 refused at once and at the next pass's retry,
    a.end();                                      // so OK is made while 82 still waits — and the link takes
    r.tick();                                     // the very next attempt: OK must still queue behind 82
    c.expect(r.s.state() == ota::REBOOTING && r.p.heard(1).size() == 249, "verified on the next pass though 82 is still waiting");
    const uint32_t okQueued = r.p.t;
    Bytes v = a.wait(), ok = a.wait();
    c.expect(hex(v) == "82" && hex(ok) == "83 DC 05", "82 then 83 DC 05, in order, once the link takes them");
    r.ticks(1500);
    c.expect(r.p.restarts == 1 && r.p.restartAt == okQueued + 1500, "restart 1,500 ms after OK was queued");
    c.expect(r.p.notifyRefused > 0, "notifications were refused");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16)), L({"82", "83 DC 05"})), ota::REBOOTING, 1, 0);
    c.summary = std::to_string(r.p.notifyRefused) + " refusals: every notification delivered once, in order; transfer completes";
}

static void scOutboxFull(Check &c) {
    Rig r;
    r.p.refuseFrom = r.p.t;
    r.p.refuseTo = r.p.t + 100;
    Bytes b = beginFor(g_small);
    r.control(1, Bytes(b.begin(), b.end() - 1));   // E1 04
    r.control(1, Bytes(1, ota::OP_ABORT));         // 84
    r.p.pendingImage = true;
    r.control(1, b);                               // E1 03
    r.p.pendingImage = false;
    r.p.safeMode = true;
    r.control(1, b);                               // E1 0A
    r.p.safeMode = false;
    r.control(1, beginFrame(7000000, g_small.sha, g_small.sig, "2.1.0", 16));   // E1 02: drops E1 04
    r.control(1, Bytes(1, ota::OP_ABORT));         // 84: drops the first 84
    r.ticks(150);
    c.expect(r.p.logsWith("outbox full") == 2, "two drops logged");
    outcome(c, r, L({"E1 03", "E1 0A", "E1 02", "84"}), ota::IDLE, 0, 0);
    c.summary = "6 replies into 4 slots: the 2 oldest dropped, the newest 4 delivered in order";
}

static void scOutboxPerLink(Check &c) {
    Rig r;
    r.p.links.push_back(Fake::Link{2, true, 255, true});   // connected, but takes no notification
    r.control(2, Bytes(1, ota::OP_ABORT));                  // its 84 waits in the outbox
    App a(r, g_docTagged);
    r.p.refuseNext = 1;                                     // the owner's READY queues behind it once
    c.expect(a.full(), "the owner's update completes while another link's reply is stuck");
    r.p.links[1].refusing = false;
    r.tick();
    c.expect(r.p.heard(2) == L({"84"}), "the stuck reply goes out once its link takes it");
    outcome(c, r, cat(cat(L({"81 FA 00 10"}), acksFor(kDocSize, 250, 16)), L({"82", "83 DC 05"})), ota::REBOOTING, 1, 0);
    c.summary = "a link that refuses notifications holds up only its own: the owner's go straight out";
}

static void scMtu517(Check &c) {
    Rig r;
    r.p.links[0].mtu = 517;                       // more than NimBLE grants: the cap still holds
    App a(r, g_small);
    a.begin();
    c.expect(a.ready() && r.p.heard(1)[0] == "81 FA 00 10", "READY 81 FA 00 10");
    r.data(1, dataFrame(0, g_small.bytes, 0, 251));
    outcome(c, r, L({"81 FA 00 10", "E1 04"}), ota::IDLE, 1, 1);
    c.summary = "MTU 517: maxChunk stays 250 (Data's 252-byte limit); 251 E1 04";
}

static void scMtuZero(Check &c) {
    Rig r;
    r.p.links[0].mtu = 0;                         // the link went between the liveness check and the MTU read
    App a(r, g_small);
    a.begin();
    r.ticks(50);
    c.expect(r.p.fBegin == 1 && r.p.shaStarts == 0, "slot opened, then given back; no hash started");
    outcome(c, r, L({}), ota::IDLE, 1, 1);
    c.summary = "MTU 0 when READY is due: abandoned like a lost link, no READY · leave 1 · flashAbort 1";
}

static void scMtu23(Check &c) {
    Rig r;
    r.p.links[0].mtu = 23;
    Image im = makeImage(50000);
    App a(r, im);
    c.expect(a.full(), "transfer at 18 bytes a frame");
    c.expect(r.p.heard(1)[0] == "81 12 00 10" && a.seq == 2778 && a.lastLen == 14, "READY 81 12 00 10, 2,778 frames");
    outcome(c, r, cat(cat(L({"81 12 00 10"}), acksFor(50000, 18, 16)), L({"82", "83 DC 05"})), ota::REBOOTING, 1, 0);
    c.summary = "READY 81 12 00 10 (maxChunk 18) · 2,778 frames · " + tail(r);
}

static void scWrap(Check &c) {
    Rig r;
    r.p.links[0].mtu = 23;
    Image im = makeImage(18u * 65536u + 1000u);   // 65,592 frames of 18 bytes
    App a(r, im);
    c.expect(a.full(32), "transfer completes");
    c.expect(a.seq == 65592 && a.lastLen == 10, "65,592 frames: frame 65,536 went out as seq 0, the last as seq 55");
    c.expect(r.p.fBytes == im.bytes.size(), "every byte written");
    outcome(c, r, cat(cat(L({"81 12 00 20"}), acksFor((uint32_t)im.bytes.size(), 18, 32)), L({"82", "83 DC 05"})), ota::REBOOTING, 1, 0);
    c.summary = "1,180,648 bytes at 18 a frame: seq wraps 65535 -> 0 and the transfer completes";
}

static void scWindow1(Check &c) {
    Rig r;
    App a(r, g_docTagged);
    c.expect(a.full(1), "transfer completes");
    outcome(c, r, cat(cat(L({"81 FA 00 01"}), acksFor(kDocSize, 250, 1)), L({"82", "83 DC 05"})), ota::REBOOTING, 1, 0);
    c.summary = "READY 81 FA 00 01 · an ACK after every one of 3,965 frames · 83 DC 05";
}

static void scWindow32(Check &c) {
    Rig r;
    App a(r, g_docTagged);
    c.expect(a.full(32), "transfer completes");
    std::vector<std::string> acks = acksFor(kDocSize, 250, 32);
    c.expect(acks.size() == 124 && acks[122] == "85 C0 03 0F 00", "123 full windows (984,000 bytes), then 29 frames");
    outcome(c, r, cat(cat(L({"81 FA 00 20"}), acks), L({"82", "83 DC 05"})), ota::REBOOTING, 1, 0);
    c.summary = "READY 81 FA 00 20 · 124 ACKs, 123rd 85 C0 03 0F 00, last 85 00 20 0F 00 · 83 DC 05";
}

static void scPartialLastWindow(Check &c) {
    Rig r;
    Image im = makeImage(9327);                   // 2 windows of 16 x 250, then 5 x 250 + 77
    App a(r, im);
    c.expect(a.full(16), "transfer completes");
    outcome(c, r, L({"81 FA 00 10", "85 A0 0F 00 00", "85 40 1F 00 00", "85 6F 24 00 00", "82", "83 DC 05"}), ota::REBOOTING, 1, 0);
    c.summary = "9,327 bytes: ACK 4,000 · 8,000 · 9,327 after a 6-frame window ending in 77 bytes";
}

static void scIgnored(Check &c) {
    Rig r;
    r.control(1, Bytes(1, ota::OP_END));          // idle: nothing to end
    r.data(1, dataFrame(0, g_small.bytes, 0, 250));
    r.p.coilsOff = false;
    App a(r, g_small);
    a.begin();
    a.end();                                      // PREPARING: ignored
    r.data(1, dataFrame(0, g_small.bytes, 0, 250));
    c.expect(r.s.state() == ota::PREPARING && r.p.sent.empty() && r.p.fWrites == 0, "END and Data before READY: no reply, no change");
    r.p.coilsOff = true;
    a.ready();
    a.send();
    a.end();
    r.data(1, dataFrame(16, g_small.bytes, 0, 250));    // VERIFYING
    r.tick();
    r.data(1, dataFrame(17, g_small.bytes, 0, 250));    // REBOOTING
    a.end();
    r.ticks(1500);
    c.expect(r.p.fWrites == 16, "no Data written outside RECEIVING");
    outcome(c, r, L({"81 FA 00 10", "85 A0 0F 00 00", "82", "83 DC 05"}), ota::REBOOTING, 1, 0);
    c.summary = "END while idle or preparing, Data outside RECEIVING: no reply, nothing changes";
}

static void scSecondSession(Check &c) {
    Rig r;
    Image bent = g_docTagged;
    bent.bytes[1234] ^= 0x80;
    memcpy(bent.sha, g_docTagged.sha, 32);
    App a(r, bent);
    c.expect(!a.full(), "first session: E1 05");
    App b(r, g_docTagged);
    b.cursor = r.p.sent.size();
    c.expect(b.full(), "second session from the same connection completes");
    c.expect(r.p.enter == 2 && r.p.leave == 1 && r.p.fAbort == 1 && r.p.shaStarts == 2 && r.p.activated == 1, "fresh hash, one activation");
    invariants(c, r);
    c.summary = "after an E1 05 the next BEGIN starts clean and completes: 83 DC 05";
}

// ---- 2.1.0: the lock (ERR 11) and updates that never go backwards (ERR 12) --

static void scErr11NotAuthorised(Check &c) {
    Rig r;
    r.p.untrusted.push_back(1);                  // the lock does not trust conn 1
    App a(r, g_small);
    a.begin();
    r.ticks(100);
    c.expect(r.p.authCalls == 1, "asked once, at BEGIN");
    c.expect(r.p.sigCalls == 0 && r.p.fBegin == 0, "no signature check, slot never opened");
    outcome(c, r, L({"E1 0B"}), ota::IDLE, 0, 0);
    r.control(1, Bytes(1, ota::OP_ABORT));       // the idle answers stay open to everyone
    c.expect(r.p.heard(1) == L({"E1 0B", "84"}), "ABORT while idle still answered 84");
    r.p.links.push_back(Fake::Link{2, true, 255, false});
    App b(r, g_small, 2);                        // a trusted connection updates as before
    c.expect(b.full(), "conn 2 (trusted) completes");
    c.expect(r.p.heard(2) == L({"81 FA 00 10", "85 A0 0F 00 00", "82", "83 DC 05"}), "conn 2 heard READY … OK");
    invariants(c, r);
    c.summary = "untrusted BEGIN: E1 0B, enterUpdateMode() never called, nothing checked or opened · a trusted conn then completes";
}

static void scErr12BeginNotNewer(Check &c) {
    struct Case { const char *running, *begin, *want; };
    const Case cases[] = {
        {"2.0.2", "2.0.2", "E1 0C"},             // equal
        {"2.0.2", "2.0.1", "E1 0C"},             // older
        {"2.0.2", "1.9.9", "E1 0C"},
        {"2.1.0", "2.1.0", "E1 0C"},             // a 2.1.0 piano offered 2.1.0 again
        {"2.1.0", "2.1.0+abc1234", "E1 0C"},     // the build suffix is not a version
        {"2.1.0", "2.1.0-rc.2", "E1 0C"},
        {"2.1.0", "2.0.2", "E1 0C"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Rig r;
        r.p.running = cases[i].running;
        r.control(1, beginFor(g_small, 16, cases[i].begin));
        r.ticks(10);
        c.expect(r.p.heard(1) == L({cases[i].want}), std::string("running ") + cases[i].running + ", BEGIN " + cases[i].begin +
                 ": heard " + join(r.p.heard(1)));
        c.expect(r.p.sigCalls == 0 && r.p.enter == 0, std::string("BEGIN ") + cases[i].begin + ": nothing stopped, no signature check");
        invariants(c, r);
    }
    Rig rn;                                      // numeric, not text: 2.0.10 is newer than 2.0.2
    rn.control(1, beginFor(g_small, 16, "2.0.10"));
    c.expect(rn.s.state() == ota::PREPARING, "BEGIN 2.0.10 on 2.0.2: accepted");
    c.summary = "BEGIN equal (2.0.2, 2.1.0, 2.1.0+build, 2.1.0-rc.2) or older (2.0.1, 1.9.9, 2.0.2 on 2.1.0): E1 0C, music untouched · 2.0.10 > 2.0.2";
}

// A full transfer of `im` (BEGIN says `begin`) that must end 82, E1 0C after END.
static void refusedAfterEnd(Check &c, const Image &im, const char *what, const char *running = "2.0.2",
                            const char *begin = "2.1.0") {
    Rig r;
    r.p.running = running;
    App a(r, im);
    c.expect(!a.full(16, 0, begin), std::string(what) + ": the transfer runs to END");
    c.expect(r.p.sigCalls == 2, std::string(what) + ": hash and signature checked first");
    c.expect(r.p.fEnd == 0 && r.p.activated == 0, std::string(what) + ": slot never activated");
    r.ticks(2000);
    c.expect(r.p.restarts == 0, std::string(what) + ": no restart");
    outcome(c, r, L({"81 FA 00 10", "85 A0 0F 00 00", "82", "E1 0C"}), ota::IDLE, 1, 1);
}

static void scErr12AfterEnd(Check &c) {
    refusedAfterEnd(c, patternImage(4000), "no tag");
    refusedAfterEnd(c, makeImage(4000, "2.0.2"), "tag equal (2.0.2)");
    refusedAfterEnd(c, makeImage(4000, "2.0.1"), "tag older (2.0.1)");
    // BEGIN claims 2.1.1 (unsigned, so it passes the early check); the signed tag decides.
    refusedAfterEnd(c, makeImage(4000, "2.1.0"), "tag 2.1.0 on a 2.1.0 piano, BEGIN says 2.1.1", "2.1.0", "2.1.1");
    refusedAfterEnd(c, makeImage(4000, "2.1.0-rc.9"), "tag 2.1.0-rc.9 on a 2.1.0 piano, BEGIN says 2.1.1", "2.1.0", "2.1.1");
    Bytes bad = tagBytes("2.1.0");
    bad.insert(bad.end() - 1, '_');              // "…2.1.0_" then NUL: a foreign character
    refusedAfterEnd(c, patternImage(4000, bad, 1000), "malformed tag (2.1.0_)");
    Bytes longTag = tagBytes("2.1.0-0123456789a");   // 17 characters before the NUL
    refusedAfterEnd(c, patternImage(4000, longTag, 1000), "17-character tag version");
    Bytes empty = tagBytes("");
    refusedAfterEnd(c, patternImage(4000, empty, 1000), "empty tag version");
    c.summary = "no tag · tag equal · tag older · equal on 2.1.0 under a BEGIN saying 2.1.1 · rc of the running version · malformed · 17 chars · empty: 82, E1 0C after the signature, never activated";
}

static void scTagSplit(Check &c) {
    // The 12 bytes "SJPV1:2.1.0" + NUL, starting at 250 - k: frame 0 ends
    // after the k-th tag byte (k = 12: the tag ends exactly at the boundary).
    for (size_t k = 1; k <= 12; k++) {
        Image im = patternImage(4000, tagBytes("2.1.0"), 250 - k);
        Rig r;
        App a(r, im);
        c.expect(a.full(), "split after tag byte " + std::to_string(k) + ": OK");
        invariants(c, r);
    }
    Rig r23;                                     // MTU 23: 18-byte frames, the tag over three of them
    r23.p.links[0].mtu = 23;
    Image im = patternImage(4000, tagBytes("2.1.0-0123456789"), 17);
    App a23(r23, im);
    c.expect(a23.full(), "MTU 23, a 23-byte tag over frames 0, 1 and 2: OK");
    c.expect(r23.p.version == "2.1.0", "(onActivated reports BEGIN's version)");
    invariants(c, r23);
    c.summary = "tag split after each of its 12 bytes across Data 0/1: OK every time · over 3 frames at MTU 23: OK";
}

static void scTagDecoy(Check &c) {
    // The scanner's own table, written into an image: never mistaken for the tag.
    Bytes decoy;
    const char *shifted = "TKQW2;";              // each marker byte + 1
    for (const char *q = shifted; *q; q++) decoy.push_back((uint8_t)*q);
    const char *v = "2.1.0";
    for (const char *q = v; *q; q++) decoy.push_back((uint8_t)*q);
    decoy.push_back(0);
    refusedAfterEnd(c, patternImage(4000, decoy, 1000), "decoy only");

    Image both = patternImage(4000, decoy, 500);   // decoy and a real tag: the tag counts
    Bytes t = tagBytes("2.1.0");
    for (size_t i = 0; i < t.size(); i++) both.bytes[2000 + i] = t[i];
    reseal(both);
    Rig r;
    App a(r, both);
    c.expect(a.full(), "decoy + real tag: OK");
    invariants(c, r);

    Bytes restart;                               // "SJPV" then a whole tag: the match restarts
    const char *pre = "SJPVSJPV1:2.1.0";
    for (const char *q = pre; *q; q++) restart.push_back((uint8_t)*q);
    restart.push_back(0);
    Rig r2;
    const Image restartImage = patternImage(4000, restart, 1000);
    App a2(r2, restartImage);
    c.expect(a2.full(), "\"SJPV\" + a tag: OK");
    invariants(c, r2);

    Bytes nested;                                // a marker inside a broken candidate
    const char *nest = "SJPV1:SJPV1:2.1.0";
    for (const char *q = nest; *q; q++) nested.push_back((uint8_t)*q);
    nested.push_back(0);
    Rig r3;
    const Image nestedImage = patternImage(4000, nested, 1000);
    App a3(r3, nestedImage);
    c.expect(a3.full(), "a marker inside a broken candidate: OK");
    invariants(c, r3);

    Image first = makeImage(4000, "2.0.1");      // two tags: the first well-formed one wins
    Bytes newer = tagBytes("9.9.9");
    for (size_t i = 0; i < newer.size(); i++) first.bytes[3000 + i] = newer[i];
    reseal(first);
    refusedAfterEnd(c, first, "older tag first, newer later");
    c.summary = "the shifted table \"TKQW2;2.1.0\" alone: E1 0C · with a real tag: OK · \"SJPV\"+tag and a nested marker: OK · first tag wins";
}

static void scVersionCompare(Check &c) {
    struct Case { const char *a, *b; bool newer; };
    const Case cases[] = {
        {"2.1.0", "2.0.2", true}, {"2.0.2", "2.1.0", false}, {"2.1.0", "2.1.0", false},
        {"2.0.10", "2.0.9", true}, {"10.0.0", "9.9.9", true}, {"2.1.1+abc1234", "2.1.0", true},
        {"2.1.0-rc.1", "2.1.0", false}, {"2.1.0", "2.1.0-rc.1", false}, {"3.0.0", "2.99.99", true},
        {"2.1", "2.0.0", false}, {"v2.1.0", "2.0.0", false}, {"2.1.0.1", "2.0.0", false}, {"", "2.0.0", false},
        {"2.1.0", "", false}, {"2.1.0", "x", false}, {"1234567890.0.0", "1.0.0", false}, {"999999999.0.0", "1.0.0", true},
        {"02.1.0", "2.0.9", true}, {"2..0", "1.0.0", false}, {"2.1.0 ", "2.0.0", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        bool got = ota::versionNewer(cases[i].a, cases[i].b);
        c.expect(got == cases[i].newer, std::string("versionNewer(\"") + cases[i].a + "\", \"" + cases[i].b + "\") = " + (got ? "true" : "false"));
    }
    c.expect(!ota::versionNewer(NULL, "2.0.0") && !ota::versionNewer("2.1.0", NULL), "NULL: not newer");

    ota::VersionTagScanner sc;                   // byte by byte, then reset
    Bytes t = tagBytes("2.1.0");
    for (size_t i = 0; i < t.size(); i++) sc.feed(&t[i], 1);
    c.expect(sc.found() && std::string(sc.version()) == "2.1.0", "fed one byte at a time: found 2.1.0");
    sc.reset();
    c.expect(!sc.found() && std::string(sc.version()).empty(), "reset forgets it");
    Bytes cut(t.begin(), t.end() - 1);           // no NUL yet: not found
    sc.feed(cut.data(), cut.size());
    c.expect(!sc.found(), "no NUL yet: not found");
    c.summary = std::to_string(sizeof cases / sizeof cases[0]) + " version pairs (numeric, suffix ignored, unparseable = not newer) · scanner byte by byte, reset";
}

// ---- driver ----------------------------------------------------------------
struct Scenario { const char *name; void (*fn)(Check &); };
static const Scenario kScenarios[] = {
    {"sha256_selftest", scSha},
    {"worked_example", scWorkedExample},
    {"worked_example_tagged", scWorkedExampleTagged},
    {"err1_not_verified", scErr1NotVerified},
    {"err1_not_quiet", scErr1NotQuiet},
    {"err2_too_large", scErr2TooLarge},
    {"err2_flash_begin", scErr2FlashBegin},
    {"err3_pending_image", scErr3Pending},
    {"err3_second_begin_prep", scErr3SecondBeginPreparing},
    {"err3_second_begin_recv", scErr3SecondBeginReceiving},
    {"err4_begin_length", scErr4BeginLength},
    {"err4_begin_size0", scErr4BeginSize0},
    {"err4_begin_window", scErr4BeginWindow},
    {"err4_begin_version", scErr4BeginVersion},
    {"err4_seq_gap", scErr4SeqGap},
    {"err4_short_data", scErr4ShortData},
    {"err4_payload_too_big", scErr4PayloadTooBig},
    {"err4_past_size", scErr4PastSize},
    {"err4_data_dropped", scErr4DataDropped},
    {"err4_early_end", scErr4EarlyEnd},
    {"err4_unknown_opcode", scErr4UnknownOpcode},
    {"err4_empty_control", scErr4EmptyControl},
    {"err5_hash", scErr5Hash},
    {"err6_begin", scErr6Begin},
    {"err6_after_end", scErr6AfterEnd},
    {"err7_flash_begin", scErr7FlashBegin},
    {"err7_flash_write", scErr7FlashWrite},
    {"err7_flash_end", scErr7FlashEnd},
    {"err8_flash_begin", scErr8FlashBegin},
    {"err8_flash_write", scErr8FlashWrite},
    {"err8_flash_end", scErr8FlashEnd},
    {"err9_timeout", scErr9Timeout},
    {"err10_safe_mode", scErr10SafeMode},
    {"begin_check_order", scBeginCheckOrder},
    {"abort_idle", scAbortIdle},
    {"abort_preparing", scAbortPreparing},
    {"abort_receiving", scAbortReceiving},
    {"abort_after_end", scAbortAfterEnd},
    {"owner_gone_preparing", scOwnerGonePreparing},
    {"owner_gone_receiving", scOwnerGoneReceiving},
    {"owner_gone_after_end", scOwnerGoneAfterEnd},
    {"foreign_conn", scForeignConn},
    {"notify_retry", scNotifyRetry},
    {"outbox_full", scOutboxFull},
    {"outbox_per_link", scOutboxPerLink},
    {"mtu23", scMtu23},
    {"mtu517", scMtu517},
    {"mtu_zero", scMtuZero},
    {"wrap", scWrap},
    {"window1", scWindow1},
    {"window32", scWindow32},
    {"partial_last_window", scPartialLastWindow},
    {"ignored_frames", scIgnored},
    {"second_session", scSecondSession},
    {"err11_not_authorised", scErr11NotAuthorised},
    {"err12_begin_not_newer", scErr12BeginNotNewer},
    {"err12_after_end", scErr12AfterEnd},
    {"tag_split_frames", scTagSplit},
    {"tag_decoy", scTagDecoy},
    {"version_compare", scVersionCompare},
};

// --scan <file>: what the session's scanner finds in a file fed in 250-byte
// frames (as over Bluetooth), and how many times the marker occurs at all.
static int scanFile(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    Bytes b;
    uint8_t buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + got);
    fclose(f);
    ota::VersionTagScanner sc;
    for (size_t at = 0; at < b.size(); at += 250) sc.feed(&b[at], b.size() - at < 250 ? b.size() - at : 250);
    const std::string marker = "SJPV1:";
    int occurrences = 0;
    for (size_t i = 0; i + marker.size() <= b.size(); i++)
        if (memcmp(&b[i], marker.data(), marker.size()) == 0) occurrences++;
    printf("%s: %zu bytes, marker occurs %d time(s), version tag %s%s\n", path, b.size(), occurrences,
           sc.found() ? "found: " : "NOT found", sc.found() ? sc.version() : "");
    return sc.found() ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--scan") == 0) return scanFile(argv[2]);
    g_doc = docImage();
    g_docTagged = makeImage(kDocSize);
    g_small = makeImage(4000);
    const size_t n = sizeof kScenarios / sizeof kScenarios[0];
    int run = 0, failed = 0;
    for (int i = 1; i < argc; i++) {              // every name must exist
        bool found = false;
        for (size_t k = 0; k < n; k++) found = found || strcmp(argv[i], kScenarios[k].name) == 0;
        if (!found) { fprintf(stderr, "unknown scenario '%s'\n", argv[i]); return 2; }
    }
    for (size_t k = 0; k < n; k++) {
        bool wanted = argc == 1;
        for (int i = 1; i < argc; i++) wanted = wanted || strcmp(argv[i], kScenarios[k].name) == 0;
        if (!wanted) continue;
        if (g_verbose) printf("== %s\n", kScenarios[k].name);
        Check c;
        c.name = kScenarios[k].name;
        kScenarios[k].fn(c);
        printf("%-24s %s  %s\n", c.name, c.fails ? "FAIL" : "OK  ", c.summary.c_str());
        run++;
        if (c.fails) failed++;
    }
    if (failed) printf("OTA SIM: FAILED (%d of %d scenarios)\n", failed, run);
    else printf("OTA SIM: all %d scenarios OK\n", run);
    return failed ? 1 : 0;
}
