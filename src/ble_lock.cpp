// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// The Bluetooth lock (docs/BLE_LOCK.md) — see include/ble_lock.h for the model.
// The cryptography is src/lock_crypto.cpp (proven on the Mac by
// tools/lock-check); this file is the state: keys in NVS, the pairing window,
// guest play, each connection's nonce, trust and failed attempts, and every
// reply line of the contract.
#include "ble_lock.h"
#include "ble_console.h"   // BLE_REFUSAL
#include "console_out.h"
#include "lock_crypto.h"

#include <NimBLEDevice.h>
#include <nvs.h>
#include <sodium.h>
#include <lvgl.h>          // LV_SYMBOL_BULLET only — this file makes no LVGL call

namespace {

const char    *kNvsNamespace = "piano";      // with the settings: an app-slot update keeps it
const char    *kNvsKeys      = "lockKeys";   // count byte, then count x 32 bytes
const uint8_t  kMaxKeys      = 4;
const uint32_t kWindowMs     = 5UL * 60UL * 1000UL;
const uint32_t kAuthGapMs    = 1000;         // at most one auth attempt a second per connection
const uint32_t kClaimGapMs   = 1000;         // at most one claim's X25519 work a second, from anyone
const uint8_t  kMaxFailures  = 5;            // then the piano drops the connection
const uint32_t kDropDelayMs  = 300;          // ... once the last "  auth refused" has gone out
const uint32_t kDropRetryMs  = 1000;
const unsigned kGuestMaxMin  = 240;
const int      kSlots        = 4;            // NimBLE-Arduino 1.4.3 allows 3 connections
const uint16_t kNoConn       = 0xFFFF;       // BLE_HS_CONN_HANDLE_NONE

const uint32_t kRgbPairing = 0xFFB060;       // amber, like "advertising": a device may be added now
const uint32_t kRgbGuest   = 0x60C0FF;

// The vector `lock selftest` checks on the device: RFC 7748 §6.1 (the piano is
// Bob, the app Alice), K and the auth over nonce 00 01 … 0f as computed by the
// app's side in Python (tools/lock-check/check.py prints them).
const char *kTestPianoSecret = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
const char *kTestAppPublic   = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
const char *kTestPianoPublic = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
const char *kTestKey         = "44ce429a81459d2c24076dd06e1e652e55561440eca70d806ea03ca80ee6dab9";
const char *kTestAuth        = "e25c89e2d232a4e45066cd17c84cc28e35751430a984ee99ef5efe912afa2036";

// ---- Live connections: the GAP listener (host task) writes, anyone reads ------
// gen numbers each connection the host accepts (never 0, wraps past it), so a
// handle reused by a new central is a different (handle, gen).
struct Live { uint16_t conn; uint16_t gen; };    // gen 0 = free slot
Live         s_live[kSlots];
int          s_liveCount = 0;                    // every connection the host holds, slot or not
uint16_t     s_genNext   = 1;
bool         s_listening = false;
portMUX_TYPE s_liveMux   = portMUX_INITIALIZER_UNLOCKED;
struct ble_gap_event_listener s_listener;

// NimBLE host task: count and number connections. Nothing else.
int gapListener(struct ble_gap_event *ev, void *) {
    if (ev->type == BLE_GAP_EVENT_CONNECT && ev->connect.status == 0) {
        const uint16_t conn = ev->connect.conn_handle;
        portENTER_CRITICAL(&s_liveMux);
        const uint16_t gen = s_genNext;
        s_genNext = (uint16_t)(s_genNext + 1);
        if (s_genNext == 0) s_genNext = 1;
        int slot = -1;
        for (int i = 0; i < kSlots; i++) {
            if (s_live[i].gen != 0 && s_live[i].conn == conn) { slot = i; s_liveCount--; break; }   // stale
        }
        for (int i = 0; slot < 0 && i < kSlots; i++) {
            if (s_live[i].gen == 0) slot = i;
        }
        if (slot >= 0) { s_live[slot].conn = conn; s_live[slot].gen = gen; }
        s_liveCount++;
        portEXIT_CRITICAL(&s_liveMux);
    } else if (ev->type == BLE_GAP_EVENT_DISCONNECT) {
        const uint16_t conn = ev->disconnect.conn.conn_handle;
        portENTER_CRITICAL(&s_liveMux);
        for (int i = 0; i < kSlots; i++) {
            if (s_live[i].gen != 0 && s_live[i].conn == conn) { s_live[i].gen = 0; s_live[i].conn = kNoConn; }
        }
        if (s_liveCount > 0) s_liveCount--;
        portEXIT_CRITICAL(&s_liveMux);
    }
    return 0;
}

uint16_t liveGen(uint16_t conn) {
    if (conn == kNoConn) return 0;
    if (!s_listening) return ble_gap_conn_find(conn, nullptr) == 0 ? 1 : 0;   // degraded (logged at boot)
    uint16_t gen = 0;
    portENTER_CRITICAL(&s_liveMux);
    for (int i = 0; i < kSlots; i++) {
        if (s_live[i].gen != 0 && s_live[i].conn == conn) gen = s_live[i].gen;
    }
    portEXIT_CRITICAL(&s_liveMux);
    return gen;
}

bool isLive(uint16_t conn, uint16_t gen) { return gen != 0 && liveGen(conn) == gen; }

int liveCount() {
    portENTER_CRITICAL(&s_liveMux);
    const int n = s_liveCount;
    portEXIT_CRITICAL(&s_liveMux);
    return n;
}

// ---- Loop-task state ----------------------------------------------------------
uint8_t  s_keys[kMaxKeys][32];
uint8_t  s_keyCount   = 0;
bool     s_unreadable = false;   // the key store could not be read: locked, with no usable key

bool     s_windowOpen  = false;
uint32_t s_windowUntil = 0;
bool     s_guestOn     = false;
uint32_t s_guestUntil  = 0;
bool     s_claimTried  = false;
uint32_t s_lastClaimMs = 0;

// One per connection that asked for a nonce, tried auth or claimed.
struct Peer {
    uint16_t conn, gen;          // gen 0 = free
    bool     trusted;            // proved a key (or claimed) on this connection
    bool     hasNonce;
    uint8_t  nonce[16];
    uint8_t  failures;
    bool     attempted;
    uint32_t lastAttemptMs;
    bool     dropPending;        // five failures: disconnect at dropAtMs
    uint32_t dropAtMs;
};
Peer s_peers[kSlots];

bool     s_askerSet  = false;
uint16_t s_askerConn = kNoConn;
uint16_t s_askerGen  = 0;

bool timeLeft(uint32_t until, uint32_t now) { return (int32_t)(until - now) > 0; }
bool windowActive(uint32_t now) { return s_windowOpen && timeLeft(s_windowUntil, now); }
bool guestActive(uint32_t now)  { return s_guestOn && timeLeft(s_guestUntil, now); }
uint32_t secondsLeft(uint32_t until, uint32_t now) {
    return timeLeft(until, now) ? (uint32_t)((until - now + 999UL) / 1000UL) : 0;
}

// True if the first word of `line` is exactly `word`.
bool commandIs(const char *line, const char *word) {
    const size_t n = strcspn(line, " ");
    return n == strlen(word) && !strncmp(line, word, n);
}
// What follows the first word and its spaces ("" when nothing does).
const char *argOf(const char *line) {
    const char *p = line + strcspn(line, " ");
    while (*p == ' ') p++;
    return p;
}

void wipePeer(Peer &p) {
    sodium_memzero(&p, sizeof p);
    p.conn = kNoConn;
}

Peer *findPeer(uint16_t conn, uint16_t gen) {
    if (gen == 0) return nullptr;
    for (int i = 0; i < kSlots; i++) {
        if (s_peers[i].gen == gen && s_peers[i].conn == conn) return &s_peers[i];
    }
    return nullptr;
}

// The record of an open connection, made on first need; nullptr for a closed one.
Peer *peerFor(uint16_t conn, uint16_t gen) {
    Peer *p = findPeer(conn, gen);
    if (p != nullptr) return p;
    if (!isLive(conn, gen)) return nullptr;
    for (int i = 0; i < kSlots; i++) {
        Peer &q = s_peers[i];
        if (q.gen == 0 || !isLive(q.conn, q.gen)) {
            wipePeer(q);
            q.conn = conn;
            q.gen  = gen;
            return &q;
        }
    }
    return nullptr;
}

bool trustedPeer(uint16_t conn, uint16_t gen) {
    const Peer *p = findPeer(conn, gen);
    return p != nullptr && p->trusted && !p->dropPending && isLive(conn, gen);
}

void newNonce(Peer &p) {
    randombytes_buf(p.nonce, sizeof p.nonce);   // the hardware RNG (Bluetooth is on)
    p.hasNonce = true;
}

void untrustEveryone() {
    for (int i = 0; i < kSlots; i++) s_peers[i].trusted = false;
}

// ---- The key store (NVS "piano" / "lockKeys") ---------------------------------
bool storeBlob(const uint8_t *blob, size_t len) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_blob(h, kNvsKeys, blob, len) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool storeKeys(const uint8_t (*keys)[32], uint8_t count) {
    uint8_t blob[1 + kMaxKeys * 32];
    blob[0] = count;
    memcpy(blob + 1, keys, (size_t)count * 32);
    const bool ok = storeBlob(blob, 1 + (size_t)count * 32);
    sodium_memzero(blob, sizeof blob);
    return ok;
}

bool eraseKeys() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const esp_err_t e = nvs_erase_key(h, kNvsKeys);
    const bool ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// At boot. A missing namespace or key is an open piano; anything unreadable
// locks it (no usable key) until a claim in a pairing window writes a fresh
// store or `lock reset` clears it — never silently open.
void loadKeys() {
    nvs_handle_t h;
    esp_err_t e = nvs_open(kNvsNamespace, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return;
    if (e != ESP_OK) {
        s_unreadable = true;
        Serial.printf("[lock] *** NVS unreadable (%s) — locked with no usable key ***\n", esp_err_to_name(e));
        return;
    }
    size_t len = 0;
    e = nvs_get_blob(h, kNvsKeys, nullptr, &len);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return;
    }
    uint8_t blob[1 + kMaxKeys * 32];
    memset(blob, 0, sizeof blob);
    bool ok = e == ESP_OK && len >= 1 && len <= sizeof blob &&
              nvs_get_blob(h, kNvsKeys, blob, &len) == ESP_OK;
    const uint8_t count = blob[0];
    ok = ok && count >= 1 && count <= kMaxKeys && len == 1 + (size_t)count * 32;
    if (ok) {
        memcpy(s_keys, blob + 1, (size_t)count * 32);
        s_keyCount = count;
    }
    sodium_memzero(blob, sizeof blob);
    nvs_close(h);
    if (!ok) {
        s_unreadable = true;
        Serial.printf("[lock] *** device keys unreadable (%u bytes) — locked with no usable key; "
                      "open pairing on the screen to claim again, or `lock reset` over USB ***\n",
                      (unsigned)len);
    }
}

// ---- Actions ------------------------------------------------------------------
void openWindow(const char *by) {
    s_windowOpen  = true;
    s_windowUntil = millis() + kWindowMs;
    Serial.printf("[lock] pairing window open for 5 min (%s)\n", by);
}

// false: nothing changed (the store could not be written). USB passes force:
// the piano opens in RAM all the same (it locks again after a restart).
bool forget(bool force, bool &saved) {
    saved = eraseKeys();
    if (!saved && !force) return false;
    sodium_memzero(s_keys, sizeof s_keys);
    s_keyCount    = 0;
    s_unreadable  = false;
    s_windowOpen  = false;
    s_guestOn     = false;
    untrustEveryone();
    Serial.printf("[lock] device keys forgotten — the piano is open%s\n",
                  saved ? "" : " (NOT saved: it locks again after a restart)");
    return true;
}

// `lock guest <minutes>`: digits only, 0..240.
void guest(const char *arg) {
    unsigned minutes = 0;
    size_t   digits  = 0;
    for (const char *p = arg; *p; p++) {
        if (*p < '0' || *p > '9' || ++digits > 3) { digits = 99; break; }
        minutes = minutes * 10 + (unsigned)(*p - '0');
    }
    if (digits == 0 || digits > 3 || minutes > kGuestMaxMin) {
        Console.println("  lock guest out of range (0..240)");
        return;
    }
    if (minutes == 0) {
        s_guestOn = false;
        Console.println("  guest play off");
        Serial.println("[lock] guest play off");
        return;
    }
    s_guestOn    = true;
    s_guestUntil = millis() + minutes * 60000UL;
    Console.printf("  guest play on for %u min\n", minutes);
    Serial.printf("[lock] guest play on for %u min: anyone in range may play notes\n", minutes);
}

void claimLine(uint16_t conn, uint16_t gen, const char *arg) {
    if (!isLive(conn, gen)) {                        // its connection closed while the line waited
        Console.println("  refused — connection closed");
        return;
    }
    const uint32_t now = millis();
    const bool wasOpen = !ble_lock_claimed();
    if (!wasOpen && !windowActive(now)) {
        Console.println("  claim refused — not open (open pairing on the piano's screen)");
        return;
    }
    uint8_t appPublic[32];
    if (!lockcrypto::fromHex(arg, appPublic, sizeof appPublic) || lockcrypto::isSmallOrder(appPublic)) {
        Console.println("  claim refused — bad key");    // a small-order key costs no X25519 time
        return;
    }
    const uint8_t have = s_unreadable ? 0 : s_keyCount;   // an unreadable store is replaced
    if (have >= kMaxKeys) {                               // never replace a key silently
        Console.println("  claim refused — already 4 devices (lock forget first)");
        return;
    }
    // Two X25519 run on the loop task: at most one claim's worth a second,
    // whoever asks, so no stream of claims can hold back the safety ticks
    // (SAFETY.md rule 6). A real claim succeeds the first time.
    if (s_claimTried && (uint32_t)(now - s_lastClaimMs) < kClaimGapMs) {
        Console.println("  claim refused — wait 1 s");
        return;
    }
    s_claimTried  = true;
    s_lastClaimMs = now;
    const uint32_t t0 = millis();
    uint8_t secret[32], pianoPublic[32], key[32];
    randombytes_buf(secret, sizeof secret);               // a fresh piano key pair per claim
    const bool derived = lockcrypto::claimDerive(secret, appPublic, pianoPublic, key);
    sodium_memzero(secret, sizeof secret);
    if (!derived) {
        Console.println("  claim refused — bad key");    // all-zero shared secret
        return;
    }
    uint8_t keys[kMaxKeys][32];
    memset(keys, 0, sizeof keys);
    memcpy(keys, s_keys, (size_t)have * 32);
    memcpy(keys[have], key, 32);
    sodium_memzero(key, sizeof key);
    if (!storeKeys(keys, (uint8_t)(have + 1))) {
        sodium_memzero(keys, sizeof keys);
        Console.println("  claim refused — could not save (try again)");
        Serial.println("[lock] *** claim not saved: NVS write failed ***");
        return;
    }
    memcpy(s_keys, keys, sizeof keys);
    sodium_memzero(keys, sizeof keys);
    s_keyCount   = (uint8_t)(have + 1);
    s_unreadable = false;
    s_windowOpen = false;            // one claim per window
    if (wasOpen) s_guestOn = false;  // guest play started while open ends with the first claim
    Peer *p = peerFor(conn, gen);
    if (p != nullptr) p->trusted = true;

    char hex[65];
    lockcrypto::toHex(pianoPublic, sizeof pianoPublic, hex);
    Console.printf("!claim=%s\n", hex);
    Console.println("  claimed");
    Serial.printf("[lock] claimed by connection %u: device key %u of %u (%lu ms)\n",
                  conn, (unsigned)s_keyCount, (unsigned)kMaxKeys, (unsigned long)(millis() - t0));
}

void authLine(uint16_t conn, uint16_t gen, const char *arg) {
    if (!ble_lock_claimed()) {
        Console.println("  auth ok — piano is open");   // open trusts everyone; nothing checked
        return;
    }
    Peer *p = peerFor(conn, gen);
    if (p == nullptr) {                                  // its connection closed while the line waited
        Console.println("  refused — connection closed");
        return;
    }
    if (p->trusted && !p->dropPending) {
        Console.println("  auth ok — already trusted");
        return;
    }
    if (p->dropPending) {                                // five failures: on its way out
        Console.println("  auth refused");
        return;
    }
    const uint32_t now = millis();
    if (p->attempted && (uint32_t)(now - p->lastAttemptMs) < kAuthGapMs) {
        Console.println("  auth refused — wait 1 s");   // not checked, not counted, nonce kept
        return;
    }
    p->attempted     = true;
    p->lastAttemptMs = now;
    uint8_t mac[32];
    const bool parsed = lockcrypto::fromHex(arg, mac, sizeof mac);
    const bool ok = parsed && p->hasNonce &&
                    lockcrypto::authMatches(s_keys, s_unreadable ? 0 : s_keyCount, p->nonce, mac);
    sodium_memzero(mac, sizeof mac);
    newNonce(*p);                                        // a nonce answers one attempt
    if (ok) {
        p->trusted  = true;
        p->failures = 0;
        Console.println("  auth ok");
        Serial.printf("[lock] connection %u authenticated\n", conn);
        return;
    }
    p->failures++;
    Console.println("  auth refused");
    if (p->failures >= kMaxFailures) {
        p->dropPending = true;
        p->dropAtMs    = now + kDropDelayMs;
        Serial.printf("[lock] connection %u: %u failed auths — disconnecting it\n", conn, (unsigned)p->failures);
    }
}

void selftest(Print &out) {
    uint8_t secret[32], appPublic[32], wantPublic[32], wantKey[32], wantMac[32];
    uint8_t pub[32], key[32], mac[32], nonce[16], zero[32];
    for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)i;
    memset(zero, 0, sizeof zero);
    const bool vectors = lockcrypto::fromHex(kTestPianoSecret, secret, 32) &&
                         lockcrypto::fromHex(kTestAppPublic, appPublic, 32) &&
                         lockcrypto::fromHex(kTestPianoPublic, wantPublic, 32) &&
                         lockcrypto::fromHex(kTestKey, wantKey, 32) &&
                         lockcrypto::fromHex(kTestAuth, wantMac, 32);
    const uint32_t t0 = millis();
    const bool claim = vectors && lockcrypto::claimDerive(secret, appPublic, pub, key) &&
                       memcmp(pub, wantPublic, 32) == 0 && memcmp(key, wantKey, 32) == 0;
    const uint32_t claimMs = millis() - t0;
    lockcrypto::authMac(wantKey, nonce, mac);
    const bool auth = vectors && memcmp(mac, wantMac, 32) == 0;
    uint8_t wrong[32];
    memcpy(wrong, wantMac, 32);
    wrong[31] ^= 1;
    const bool match = vectors && lockcrypto::authMatches(&wantKey, 1, nonce, wantMac) &&
                       !lockcrypto::authMatches(&wantKey, 1, nonce, wrong);
    const bool small = !lockcrypto::claimDerive(secret, zero, pub, key);
    sodium_memzero(key, sizeof key);
    sodium_memzero(secret, sizeof secret);
    const bool all = claim && auth && match && small;
    out.printf("  lock selftest: %s — claim (X25519 + SHA-256) %s in %lu ms, auth HMAC %s, "
               "key match %s, small-order key refused %s\n",
               all ? "ok" : "FAILED", claim ? "ok" : "WRONG", (unsigned long)claimMs,
               auth ? "ok" : "WRONG", match ? "ok" : "WRONG", small ? "yes" : "NO");
}

// `lock …` from a trusted Bluetooth connection or from USB (already checked).
void lockCommand(const char *line, bool usb, const char *by) {
    const char *sub = argOf(line);
    if (!strcmp(sub, "pairwindow") || (usb && !strcmp(sub, "open"))) {
        openWindow(by);
        Console.println("  pairing open for 5 min");
    } else if (!strcmp(sub, "forget") || (usb && !strcmp(sub, "reset"))) {
        bool saved = false;
        if (!forget(usb, saved)) {
            Console.println("  lock forget refused — could not save (try again)");
        } else if (saved) {
            Console.println("  lock forgotten — piano is open");
        } else {
            Console.println("  lock forgotten — piano is open (not saved: it locks again after a restart)");
        }
    } else if (commandIs(sub, "guest")) {
        guest(argOf(sub));
    } else if (!strcmp(sub, "status")) {
        ble_lock_print_status(Console);
    } else if (usb && !strcmp(sub, "selftest")) {
        selftest(Console);
    } else if (!usb && (!strcmp(sub, "open") || !strcmp(sub, "reset") || !strcmp(sub, "selftest"))) {
        Console.println(BLE_REFUSAL);
    } else if (usb) {
        Console.println("  usage: lock status | open | reset | pairwindow | forget | guest <0..240> | selftest");
    } else {
        Console.println("  usage: lock pairwindow | forget | guest <0..240> | status");
    }
}

}  // namespace

// ---- Public -------------------------------------------------------------------

void ble_lock_init() {
    for (int i = 0; i < kSlots; i++) {
        s_live[i].conn = kNoConn;
        s_live[i].gen  = 0;
        wipePeer(s_peers[i]);
    }
    loadKeys();
    const int rc = ble_gap_event_listener_register(&s_listener, gapListener, nullptr);
    s_listening = (rc == 0);
    if (!s_listening) {
        Serial.printf("[lock] *** connection tracking unavailable (rc=%d): on a claimed piano Bluetooth "
                      "notes play only in guest play ***\n", rc);
    }
    if (s_unreadable) {
        // logged by loadKeys()
    } else if (s_keyCount == 0) {
        Serial.println("[lock] open — no device key: the first device to claim adds one");
    } else {
        Serial.printf("[lock] claimed — %u device key(s); a connection must send auth before it plays, "
                      "changes settings or updates\n", (unsigned)s_keyCount);
    }
}

void ble_lock_tick() {
    const uint32_t now = millis();
    if (s_windowOpen && !timeLeft(s_windowUntil, now)) {
        s_windowOpen = false;
        Serial.println("[lock] pairing window closed");
    }
    if (s_guestOn && !timeLeft(s_guestUntil, now)) {
        s_guestOn = false;
        Serial.println("[lock] guest play ended");
    }
    for (int i = 0; i < kSlots; i++) {
        Peer &p = s_peers[i];
        if (p.gen == 0) continue;
        if (!isLive(p.conn, p.gen)) {                    // trust, nonce and count end with the link
            wipePeer(p);
            continue;
        }
        if (p.dropPending && !timeLeft(p.dropAtMs, now)) {
            NimBLEServer *srv = NimBLEDevice::getServer();
            const int rc = srv ? srv->disconnect(p.conn) : -1;
            if (rc != 0) p.dropAtMs = now + kDropRetryMs;   // try again; still refused meanwhile
            else         p.dropAtMs = now + 60000UL;        // asked; the link is about to go
        }
    }
}

bool ble_lock_claimed() { return s_keyCount > 0 || s_unreadable; }

uint16_t ble_lock_conn_gen(uint16_t conn) { return liveGen(conn); }

bool ble_lock_trusted(uint16_t conn, uint16_t gen) {
    if (!ble_lock_claimed()) return true;
    return trustedPeer(conn, gen);
}

bool ble_lock_trusted_conn(uint16_t conn) {
    if (!ble_lock_claimed()) return true;
    return trustedPeer(conn, liveGen(conn));
}

bool ble_lock_notes_allowed() {
    if (!ble_lock_claimed()) return true;
    if (guestActive(millis())) return true;
    if (!s_listening) return false;
    uint16_t conn = kNoConn, gen = 0;
    int n;
    portENTER_CRITICAL(&s_liveMux);
    n = s_liveCount;
    if (n == 1) {
        for (int i = 0; i < kSlots; i++) {
            if (s_live[i].gen != 0) { conn = s_live[i].conn; gen = s_live[i].gen; }
        }
    }
    portEXIT_CRITICAL(&s_liveMux);
    if (n != 1 || gen == 0) return false;                // BLE-MIDI bytes carry no connection
    const Peer *p = findPeer(conn, gen);
    return p != nullptr && p->trusted && !p->dropPending;
}

bool ble_lock_is_lock_command(const char *line) {
    return commandIs(line, "claim") || commandIs(line, "auth") || commandIs(line, "lock");
}

void ble_lock_handle_ble(uint16_t conn, uint16_t gen, const char *line) {
    if (commandIs(line, "claim")) {
        claimLine(conn, gen, argOf(line));
    } else if (commandIs(line, "auth")) {
        authLine(conn, gen, argOf(line));
    } else if (!ble_lock_trusted(conn, gen)) {           // any `lock …` from an unproven connection
        Console.println(BLE_LOCK_REFUSAL);
    } else {
        char by[24];
        snprintf(by, sizeof by, "connection %u", conn);
        lockCommand(line, false, by);
    }
}

bool ble_lock_untrusted_allowed(const char *line) {
    static const char *const kOpen[] = { "dump", "get", "status", "help", "off", "broadcast" };
    for (const char *word : kOpen) {
        if (commandIs(line, word)) return true;
    }
    return false;
}

void ble_lock_handle_usb(const char *line) {
    if (commandIs(line, "claim") || commandIs(line, "auth")) {
        Console.println("  claim and auth are Bluetooth commands — the USB console is never locked");
        return;
    }
    lockCommand(line, true, "USB console");
}

void ble_lock_set_asker(uint16_t conn, uint16_t gen) {
    s_askerSet  = true;
    s_askerConn = conn;
    s_askerGen  = gen;
}

void ble_lock_clear_asker() {
    s_askerSet  = false;
    s_askerConn = kNoConn;
    s_askerGen  = 0;
}

bool ble_lock_asker_trusted() {
    return !s_askerSet || ble_lock_trusted(s_askerConn, s_askerGen);
}

const char *ble_lock_state_word() {
    if (!ble_lock_claimed()) return "open";
    return windowActive(millis()) ? "pairing" : "claimed";
}

void ble_lock_nonce_hex(char *buf, size_t len) {
    if (buf == nullptr || len == 0) return;
    static const char kZeros[] = "00000000000000000000000000000000";
    Peer *p = s_askerSet ? peerFor(s_askerConn, s_askerGen) : nullptr;
    if (p == nullptr) {                                   // USB, or a closed connection
        snprintf(buf, len, "%s", kZeros);
        return;
    }
    if (!p->hasNonce) newNonce(*p);                       // new for every connection
    char hex[33];
    lockcrypto::toHex(p->nonce, sizeof p->nonce, hex);
    snprintf(buf, len, "%s", hex);
}

uint32_t ble_lock_guest_seconds() {
    return s_guestOn ? secondsLeft(s_guestUntil, millis()) : 0;
}

void ble_lock_open_window() { openWindow("the piano's screen"); }

bool ble_lock_status_text(char *buf, size_t len, uint32_t *rgb, bool *keepAwake) {
    if (keepAwake) *keepAwake = false;
    if (buf == nullptr || len == 0) return false;
    buf[0] = '\0';
    const uint32_t now = millis();
    if (windowActive(now)) {
        const uint32_t s = secondsLeft(s_windowUntil, now);
        snprintf(buf, len, "Pairing open " LV_SYMBOL_BULLET " %lu:%02lu",
                 (unsigned long)(s / 60), (unsigned long)(s % 60));
        if (rgb) *rgb = kRgbPairing;
        if (keepAwake) *keepAwake = true;                 // someone is at the piano
        return true;
    }
    if (guestActive(now)) {
        const uint32_t s = secondsLeft(s_guestUntil, now);
        snprintf(buf, len, "Guest play " LV_SYMBOL_BULLET " %lu:%02lu",
                 (unsigned long)(s / 60), (unsigned long)(s % 60));
        if (rgb) *rgb = kRgbGuest;
        return true;
    }
    return false;
}

void ble_lock_print_status(Print &out) {
    const uint32_t now = millis();
    int trusted = 0;
    for (int i = 0; i < kSlots; i++) {
        if (s_peers[i].gen != 0 && trustedPeer(s_peers[i].conn, s_peers[i].gen)) trusted++;
    }
    char state[64];
    if (s_unreadable)         snprintf(state, sizeof state, "claimed (device keys unreadable)");
    else if (s_keyCount == 0) snprintf(state, sizeof state, "open (no device key)");
    else                      snprintf(state, sizeof state, "claimed, %u of %u device keys",
                                       (unsigned)s_keyCount, (unsigned)kMaxKeys);
    char window[24], guestText[24];
    if (windowActive(now)) {
        const uint32_t s = secondsLeft(s_windowUntil, now);
        snprintf(window, sizeof window, "open %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    } else {
        snprintf(window, sizeof window, "closed");
    }
    if (guestActive(now)) {
        const uint32_t s = secondsLeft(s_guestUntil, now);
        snprintf(guestText, sizeof guestText, "on %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    } else {
        snprintf(guestText, sizeof guestText, "off");
    }
    out.printf("  bluetooth lock: %s; pairing %s; guest play %s; %d connected, %d trusted%s\n",
               state, window, guestText, liveCount(), ble_lock_claimed() ? trusted : liveCount(),
               s_listening ? "" : " (connection tracking unavailable)");
}

void ble_lock_after_settings_reset(Print &out) {
    if (s_unreadable) {
        const uint8_t marker = 0xFF;                      // stays unreadable: still locked after a restart
        storeBlob(&marker, 1);
        out.println("  the Bluetooth lock stays locked (`lock reset` clears it)");
    } else if (s_keyCount > 0) {
        if (storeKeys(s_keys, s_keyCount)) out.println("  the Bluetooth lock is kept (`lock reset` clears it)");
        else out.println("  [lock] device keys NOT written back — the piano opens after a restart");
    }
}
