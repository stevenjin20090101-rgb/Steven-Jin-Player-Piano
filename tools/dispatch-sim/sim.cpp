// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Host-side simulator for the solenoid dispatch state machine.
//
// Compiles the REAL src/power_boards.cpp against stub Arduino/Wire/FreeRTOS
// headers, replaces the PCA9685 driver with simulated chips, and drives the
// dispatch functions with a fake clock at the same ~100 Hz safety-tick cadence
// as loop(). Each scenario reports how long the coil stayed energized without
// a verified OFF landing (the fire-safety number), plus strike/lift counts.
// The firmware-update scenarios (inhibit, verified_off, verified_off_pending)
// check SAFETY.md rule 9's hooks instead: no energize gets through the inhibit,
// and coilsVerifiedOff() is true only when nothing is on or about to be.
//
//   make check          run every scenario, fail on any ceiling violation
//   SIM_VERBOSE=1 ./sim grace     print the event/coil timeline of one scenario
//
// See docs/SAFETY.md. Add a scenario whenever you touch the dispatch path.
#include "power_boards.h"
#include "config.h"
#include <vector>
#include <string>
#include <algorithm>

static uint32_t       g_now = 0;
static const uint32_t T0    = 1000;   // scenario clock offset (hardware never strikes at millis()==0)
static bool           g_verbose = getenv("SIM_VERBOSE") != nullptr;

uint32_t millis() { return g_now; }
void delay(uint32_t ms) { g_now += ms; }
void delayMicroseconds(uint32_t) {}
long random(long lo, long) { return lo; }          // deterministic: no humanize scatter
SimSerial Serial;
int  SimSerial::printf(const char* fmt, ...) { if (!g_verbose) return 0; va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); va_end(ap); return r; }
void SimSerial::println(const char* s) { if (g_verbose) puts(s); }
void SimSerial::println() { if (g_verbose) puts(""); }
int  SimSerial::availableForWrite() { return 128; }

SemaphoreHandle_t g_wireMutex = nullptr;
uint32_t g_i2cFailCount = 0;
void i2cForceRecovery() {}
TwoWire Wire, Wire1;

// ---- simulated PCA9685 chips ----------------------------------------------
struct SimChip { uint8_t addr; uint16_t ch[16]; uint32_t onSince[16]; uint32_t maxOn[16]; int lifts[16]; int strikes[16]; uint32_t offAt[16]; };
static SimChip g_chips[8]; static int g_nChips = 0;
static SimChip* chipFor(uint8_t a) { for (int i = 0; i < g_nChips; i++) if (g_chips[i].addr == a) return &g_chips[i]; return nullptr; }
// Fault injection: every OFF write (value 0, allOff, ALLCALL) fails while now is in [from, to).
static uint32_t g_failOffFrom = 1, g_failOffTo = 0;
static bool offWriteFails() { return g_now >= g_failOffFrom && g_now < g_failOffTo; }
// Every setPWM() asked to drive a non-zero value, landed or not — an energize attempt.
static int g_nonZeroWrites = 0;
static void setChannel(SimChip& c, int ch, uint16_t v) {
    bool was = c.ch[ch] != 0, is = v != 0;
    if (!was && is) { c.onSince[ch] = g_now; c.strikes[ch]++; }
    if (was && !is) { uint32_t d = g_now - c.onSince[ch]; if (d > c.maxOn[ch]) c.maxOn[ch] = d; c.lifts[ch]++; c.offAt[ch] = g_now; }
    if (was != is && g_verbose) printf("  [sim] t=%6u  chip 0x%02X ch%-2d -> %s\n", g_now, c.addr, ch, is ? (v >= 4095 ? "ON (full)" : "ON") : "OFF");
    c.ch[ch] = v;
}
static uint8_t s_txAddr = 0; static std::vector<uint8_t> s_txBuf;
void    TwoWire::beginTransmission(uint8_t a) { s_txAddr = a; s_txBuf.clear(); }
size_t  TwoWire::write(uint8_t b) { s_txBuf.push_back(b); return 1; }
uint8_t TwoWire::endTransmission() {
    if (s_txAddr == 0x70 && !s_txBuf.empty() && s_txBuf[0] == 0xFA) {      // ALLCALL FULL_OFF burst
        if (offWriteFails()) return 4;
        for (int i = 0; i < g_nChips; i++) for (int ch = 0; ch < 16; ch++) setChannel(g_chips[i], ch, 0);
    }
    return 0;
}
bool PCA9685::begin() {
    SimChip* c = chipFor(_addr);
    if (!c) { c = &g_chips[g_nChips++]; memset(c, 0, sizeof(*c)); c->addr = _addr; }
    for (int ch = 0; ch < 16; ch++) setChannel(*c, ch, 0);
    return true;
}
bool PCA9685::isConnected() { return true; }
bool PCA9685::setPWM(uint8_t channel, uint16_t value) {
    if (value != 0) g_nonZeroWrites++;
    SimChip* c = chipFor(_addr); if (!c || channel >= 16) return false;
    if (value == 0 && offWriteFails()) { g_i2cFailCount++; return false; }   // failed OFF: coil stays as it was
    setChannel(*c, channel, value >= 4095 ? 4096 : value);
    return true;
}
bool PCA9685::allOff() { SimChip* c = chipFor(_addr); if (!c) return false; if (offWriteFails()) return false; for (int ch = 0; ch < 16; ch++) setChannel(*c, ch, 0); return true; }
bool PCA9685::setPWMFrequency(uint16_t) { return true; }
bool PCA9685::writeReg(uint8_t, uint8_t) { return true; }
bool PCA9685::writeBurst(uint8_t, const uint8_t*, uint8_t) { return true; }

// ---- scenario driver -------------------------------------------------------
// coilsVerifiedOff() may claim "all off" only while every simulated channel
// really is off. Asked every simulated ms in EVERY scenario (it is read-only,
// so asking changes nothing); a false claim fails the scenario.
static int g_falseOffClaims = 0;
static bool anyChannelOn() {
    for (int i = 0; i < g_nChips; i++) for (int ch = 0; ch < 16; ch++) if (g_chips[i].ch[ch]) return true;
    return false;
}
static void ticks() { tickAutoRelease(); tickPendingFires(); tickRestrike(); }   // same order as loop()
static void advanceTo(uint32_t t) {
    while (g_now < t) {
        g_now++;
        if (g_now % 10 == 0) ticks();
        if (coilsVerifiedOff() && anyChannelOn()) g_falseOffClaims++;
    }
}
struct Ev { uint32_t t; int kind; uint8_t note; uint8_t vel; };   // kind: 0 NoteOff, 1 NoteOn, 2 serial "fire" (board 3 ch 0)
static void runEvents(std::vector<Ev> evs, uint32_t settleUntil) {
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    for (auto& e : evs) e.t += T0;
    settleUntil += T0;
    for (auto& e : evs) {
        advanceTo(e.t);
        if (g_verbose) printf("  [src] t=%6u  %s note %u\n", g_now, e.kind == 1 ? "NoteOn " : e.kind == 0 ? "NoteOff" : "fire   ", e.note);
        if (e.kind == 1) dispatchNoteOn(e.note, e.vel);
        else if (e.kind == 0) dispatchNoteOff(e.note);
        else manualFire(3, 0, 4096);
    }
    advanceTo(settleUntil);
}
// Note 60 (C4) lives on board 3 (0x43), PCA channel 0.
static int report(const char* name, int minStrikes, int maxStrikes) {
    SimChip* c = chipFor(0x43);
    uint32_t maxOn = c->maxOn[0];
    bool stillOn = c->ch[0] != 0;
    if (stillOn) maxOn = std::max(maxOn, g_now - c->onSince[0]);
    bool ceilingOk = !stillOn && maxOn <= g_maxHoldMs + 20;   // 20 ms slack: one 10 ms tick + write
    bool strikesOk = c->strikes[0] >= minStrikes && c->strikes[0] <= maxStrikes;
    printf("%-16s strikes=%-4d lifts=%-4d longest_energized=%6u ms  final=%s  ceiling(%u ms)=%s  strikes(%d..%d)=%s\n",
           name, c->strikes[0], c->lifts[0], maxOn, stillOn ? "ON " : "off", g_maxHoldMs,
           ceilingOk ? "OK" : "VIOLATED", minStrikes, maxStrikes, strikesOk ? "OK" : "FAIL");
    if (g_falseOffClaims) printf("  FAIL: coilsVerifiedOff() claimed all off while a coil was ON (%d ms)\n", g_falseOffClaims);
    return (ceilingOk && strikesOk && !g_falseOffClaims) ? 0 : 1;
}

// ---- firmware-update hooks (SAFETY.md rule 9, BLE_OTA.md §7) ---------------
// Board 3 (0x43) holds C4..B4; note 60 + semitone s drives PCA channel kKeyToChannel[s]:
// 60 → ch 0, 62 → ch 1, 64 → ch 2, 65 → ch 3, 67 → ch 4.
static int g_hookFails = 0;
static void expect(bool ok, const char* what) {
    if (!ok) { g_hookFails++; printf("  FAIL at t=+%u ms: %s\n", g_now - T0, what); }
}
static void at(uint32_t rel) { advanceTo(T0 + rel); }
static bool chOn(int ch) { return chipFor(0x43)->ch[ch] != 0; }
static uint32_t offAtRel(int ch) { uint32_t t = chipFor(0x43)->offAt[ch]; return t ? t - T0 : 0; }
static int allStrikes() { int n = 0; for (int i = 0; i < g_nChips; i++) for (int ch = 0; ch < 16; ch++) n += g_chips[i].strikes[ch]; return n; }
static int hookResult(const char* name, const char* summary) {
    if (g_falseOffClaims) expect(false, "coilsVerifiedOff() claimed all off while a coil was ON");
    printf("%-16s %s  %s\n", name, summary, g_hookFails ? "FAIL" : "OK");
    return g_hookFails ? 1 : 0;
}

// One energize attempt per call, cycling through every entry point a session
// must refuse: NoteOn on a held key and on a released one, serial "fire" (also
// on unwired channels) and "rawnote", on every board.
static bool energizeAttempt(uint32_t k) {
    uint8_t key = (uint8_t)(24 + (k * 5) % 84);
    switch (k % 7) {
        case 0:  return dispatchNoteOn(60, 127);                                      // A's key
        case 1:  return dispatchNoteOn(62, 100);                                      // B's key, still held
        case 2:  return manualFire(3, 0, 4096);                                       // "fire" on A
        case 3:  return manualFireNote(64, 3000);                                     // "rawnote" on C
        case 4:  return dispatchNoteOn(key, 90);
        case 5:  return manualFire((uint8_t)((k / 7) % 7), (uint8_t)(k % 16), 2000);  // unwired chs too
        default: return manualFireNote(key, 4096);
    }
}

// Five coils on board 3 are energized, then the inhibit goes on and a storm of
// energize attempts runs for 2.5 s. Not one non-zero write may follow, and each
// coil must still be released by the path that owns it.
static int scenarioInhibit() {
    g_restrikeMs = 300;                                   // held-note re-strike ticks run
    g_failOffFrom = T0 + 1995; g_failOffTo = T0 + 2045;   // the watchdog's first OFFs glitch
    at(0);
    manualFire(3, 0, 4096);          // A: serial "fire", not held — the hold watchdog's to release
    dispatchNoteOn(62, 100);         // B: held, re-struck every 300 ms — the lift releases it
    dispatchNoteOn(65, 100);         // D: NoteOff while inhibited — must skip the cushion
    at(90);  dispatchNoteOn(67, 100);         // E: NoteOff deferred by min-strike, runs inhibited
    at(95);  manualFireNote(64, 3000);        // C: allKeysOff()'s to release
    at(100);
    expect(!energize_inhibited(), "inhibit is off until set");
    set_energize_inhibit(true);
    expect(energize_inhibited(), "inhibit reads back");
    expect(chOn(0) && chOn(1) && chOn(2) && chOn(3) && chOn(4), "setting the inhibit releases nothing by itself");
    const int writes0 = g_nonZeroWrites, strikes0 = allStrikes();
    const uint32_t last0 = lastEnergizeMs();
    expect(last0 == T0 + 95, "lastEnergizeMs() = the last energize before the inhibit");

    int attempts = 0, refused = 0, sweeps = 0, lateFalse = 0;
    uint32_t k = 0;
    for (uint32_t t = 101; t <= 2600; t++) {
        at(t);
        if (t == 110)  dispatchNoteOff(67);   // E: held 20 ms < min-strike → release deferred to +150
        if (t == 120) {                       // "rawnote" on E: refused BEFORE it can cancel that release
            attempts++;
            if (!manualFireNote(67, 4096)) refused++;
        }
        if (t == 150)  dispatchNoteOff(65);   // D: released while inhibited
        if (t == 2081) expect(!coilsVerifiedOff(), "false while C is still energized");
        if (t == 2082) allKeysOff();          // C: the stop
        if ((t - 101) % 7 == 0) {
            attempts++;
            if (!energizeAttempt(k)) refused++;
            uint8_t off = (uint8_t)(24 + (k * 3) % 84);
            if (off >= 60 && off < 72) off += 12;     // releases elsewhere; board 3's coils keep their owners
            dispatchNoteOff(off);
            if (k % 100 == 50) {
                uint32_t before = g_now;
                testSweepAllBoards();
                sweeps++;
                expect(g_now == before, "the sweep returns at once");
            }
            k++;
        }
        if (t >= 2082 && !coilsVerifiedOff()) lateFalse++;
    }
    at(4000);
    expect(refused == attempts, "every energize attempt refused");
    expect(g_nonZeroWrites == writes0, "zero non-zero PWM writes after the inhibit");
    expect(allStrikes() == strikes0, "no coil went from off to on after the inhibit");
    expect(offAtRel(0) == 2050, "A: released by the hold watchdog once its glitched OFF retried through");
    expect(offAtRel(1) == 300 && chipFor(0x43)->strikes[1] == 1, "B: lifted by the re-strike tick, the re-strike refused");
    expect(offAtRel(2) == 2082, "C: released by allKeysOff()");
    expect(offAtRel(3) == 150 && offAtRel(4) == 150, "D, E: straight to the verified OFF, no cushion");
    expect(lateFalse == 0, "true from the stop on, refused NoteOns queue nothing");
    expect(lastEnergizeMs() == last0, "refused energizes leave lastEnergizeMs() alone");
    expect(!anyChannelOn() && coilsVerifiedOff() && energize_inhibited(), "all off, verified, still inhibited");
    char s[200];
    snprintf(s, sizeof s, "refused=%d/%d sweeps_refused=%d energize_writes_after=%d  A_off=+%u(watchdog) B_off=+%u(lift) C_off=+%u(allKeysOff) D,E_off=+%u(no cushion)",
             refused, attempts, sweeps, g_nonZeroWrites - writes0, offAtRel(0), offAtRel(1), offAtRel(2), offAtRel(3));
    return hookResult("inhibit", s);
}

// Strike, then allKeysOff() while every OFF write fails: the coil is still ON,
// so coilsVerifiedOff() must stay false until the watchdog's retry lands.
static int scenarioVerifiedOff() {
    at(0);
    expect(coilsVerifiedOff(), "true at boot: every chip began with a verified all-off");
    expect(lastEnergizeMs() == 0, "lastEnergizeMs() is 0 before any energize");
    dispatchNoteOn(60, 100);
    expect(lastEnergizeMs() == T0, "lastEnergizeMs() = the strike");
    g_failOffFrom = T0 + 100; g_failOffTo = T0 + 400;
    at(150);
    allKeysOff();
    expect(!coilsVerifiedOff(), "false right after a stop whose OFF writes failed");
    uint32_t firstTrue = 0;
    int relapses = 0;
    for (uint32_t t = 151; t <= 1000; t++) {
        at(t);
        bool v = coilsVerifiedOff();
        if (v && !firstTrue) firstTrue = t;
        if (!v && firstTrue) relapses++;
        if (t == 399) expect(chOn(0) && !v, "the coil is still ON and it says so");
    }
    expect(firstTrue == 400 && offAtRel(0) == 400 && relapses == 0, "true exactly when the verified OFF lands");
    expect(lastEnergizeMs() == T0, "the stop and the retries are not energizes");
    const uint32_t lastPhase1 = lastEnergizeMs();

    // An OFFLINE board's believed-on coil (rule 5): the stop's ALLCALL ACK proves
    // nothing for a chip off the bus, so only the health tick's verified per-chip
    // all-off may clear it.
    at(1100); dispatchNoteOn(72, 100);        // C5: board 4 (0x44), ch 0
    at(1150);
    powerBoards[4].connected = false;         // as tickChipHealth leaves a board whose re-init failed
    allKeysOff();
    expect(!coilsVerifiedOff(), "an offline board's believed-on coil keeps it false after the stop");
    at(3990); expect(!coilsVerifiedOff(), "still false: nothing verified that chip");
    at(4000); tickChipHealth();               // its 5 s slot: verified per-chip all-off, then rejoin
    expect(powerBoards[4].connected && coilsVerifiedOff(), "true once that chip's all-off is verified");

    char s[200];
    snprintf(s, sizeof s, "strike=+0 lastEnergizeMs=+%u  allKeysOff=+150, OFF writes fail until +400  coil_off=+%u verified_off_from=+%u  offline_board: false until its verified all-off",
             lastPhase1 - T0, offAtRel(0), firstTrue);
    return hookResult("verified_off", s);
}

// Every coil verified off, yet something is still queued: a deferred strike, a
// cushion the watchdog overtook, a held note between re-strikes. Each keeps
// coilsVerifiedOff() false until it is cleared.
static int scenarioVerifiedOffPending() {
    at(0);    dispatchNoteOn(60, 100);
    at(200);  dispatchNoteOff(60);            // cushion until +222
    at(210);  dispatchNoteOn(60, 100);        // repeat during the cushion: cut now, strike deferred to +240
    at(215);  dispatchNoteOff(60);            // its release queued behind the deferred strike
    at(216);  expect(!chOn(0) && !coilsVerifiedOff(), "deferred strike pending, every coil off: false");
    at(220);  allKeysOff();
    expect(coilsVerifiedOff(), "the stop cleared the deferred strike: true");
    at(400);  expect(chipFor(0x43)->strikes[0] == 1, "the cleared strike never fired");

    at(500);  dispatchNoteOn(60, 100);        // hold deadline +2500
    at(2490); dispatchNoteOff(60);            // cushion until +2512 ...
    expect(lastEnergizeMs() == T0 + 2490, "the cushion write stamps lastEnergizeMs() though the anchor stays +500");
    at(2505); expect(!chOn(0) && !coilsVerifiedOff(), "watchdog cut at +2500, cushion still pending: false");
    g_failOffFrom = T0 + 2515; g_failOffTo = T0 + 2525;   // ... and the cushion's own OFF at +2520 glitches
    at(2521); expect(!chOn(0) && !coilsVerifiedOff(), "an OFF write still owed (s_needOff), timers clear: false");
    at(2531); expect(coilsVerifiedOff(), "its retry landed at +2530: true");

    at(2600); g_restrikeMs = 300;
    at(3000); dispatchNoteOn(60, 100);        // held; lifted at +3300, re-struck at +3340
    at(3310); expect(!chOn(0) && !coilsVerifiedOff(), "held note in its re-strike lift: false");
    at(3400); dispatchNoteOff(60);
    at(3480); expect(coilsVerifiedOff(), "released: true");
    at(4000); expect(!anyChannelOn(), "all off");
    return hookResult("verified_off_pending", "deferred strike: false until the stop  cushion after a watchdog cut: false until it completes  owed OFF retry: false until it lands  held in lift gap: false");
}

int main(int argc, char** argv) {
    std::string sc = argc > 1 ? argv[1] : "rapid";
    initPowerBoards();
    std::vector<Ev> ev;
    if (sc == "rapid") {                 // same key: onsets every 40 ms, notes 20 ms long, for 10 s
        for (uint32_t t = 0; t < 10000; t += 40) { ev.push_back({t, 1, 60, 100}); ev.push_back({t + 20, 0, 60, 0}); }
        runEvents(ev, 13000); return report(sc.c_str(), 100, 250);
    } else if (sc == "rapid_snappy") {   // "snappy" preset (gap 0, minstrike 20, no cushion): onsets 15 ms, notes 5 ms
        g_minRetriggerGapMs = 0; g_minStrikeMs = 20; g_softRelease = false;
        for (uint32_t t = 0; t < 5000; t += 15) { ev.push_back({t, 1, 60, 100}); ev.push_back({t + 5, 0, 60, 0}); }
        runEvents(ev, 8000); return report(sc.c_str(), 100, 340);
    } else if (sc == "rapid_failoff") {  // rapid pattern, and the first lift's OFF write glitches for 20 ms
        g_failOffFrom = T0 + 95; g_failOffTo = T0 + 115;
        for (uint32_t t = 0; t < 2000; t += 40) { ev.push_back({t, 1, 60, 100}); ev.push_back({t + 20, 0, 60, 0}); }
        runEvents(ev, 5000); return report(sc.c_str(), 20, 50);
    } else if (sc == "grace") {          // grace note then a long note on the same key: both must sound
        ev = {{0, 1, 60, 100}, {20, 0, 60, 0}, {40, 1, 60, 100}, {1500, 0, 60, 0}};
        runEvents(ev, 3000); return report(sc.c_str(), 2, 2);
    } else if (sc == "short_repeat") {   // two short notes back to back: both must sound
        ev = {{0, 1, 60, 100}, {20, 0, 60, 0}, {40, 1, 60, 100}, {60, 0, 60, 0}};
        runEvents(ev, 1000); return report(sc.c_str(), 2, 2);
    } else if (sc == "case1_gap") {      // existing deferral: retrigger during the cushion + NoteOff before the deferred fire
        ev = {{0, 1, 60, 100}, {100, 0, 60, 0}, {110, 1, 60, 100}, {115, 0, 60, 0}};
        runEvents(ev, 1000); return report(sc.c_str(), 2, 2);
    } else if (sc == "manual_refire") {  // serial "fire" re-issued every 500 ms with no OFF: ceiling must still trip
        for (uint32_t t = 0; t <= 5000; t += 500) ev.push_back({t, 2, 60, 0});
        runEvents(ev, 8000); return report(sc.c_str(), 3, 3);
    } else if (sc == "rearticulate") {   // NoteOn repeated without NoteOff (buggy source), held
        ev = {{0, 1, 60, 100}, {1000, 1, 60, 100}, {1900, 1, 60, 100}, {2800, 1, 60, 100}};
        runEvents(ev, 6000); return report(sc.c_str(), 2, 2);
    } else if (sc == "long_note") {      // ordinary 1.5 s note
        ev = {{0, 1, 60, 100}, {1500, 0, 60, 0}};
        runEvents(ev, 3000); return report(sc.c_str(), 1, 1);
    } else if (sc == "orphan") {         // NoteOn with no NoteOff ever: watchdog releases at g_maxHoldMs
        ev = {{0, 1, 60, 100}};
        runEvents(ev, 5000); return report(sc.c_str(), 1, 1);
    } else if (sc == "inhibit") {        // update session: no energize gets through, releases still do
        return scenarioInhibit();
    } else if (sc == "verified_off") {   // coilsVerifiedOff() waits for a VERIFIED OFF; lastEnergizeMs()
        return scenarioVerifiedOff();
    } else if (sc == "verified_off_pending") {   // queued strike / cushion / held note keep it false
        return scenarioVerifiedOffPending();
    }
    fprintf(stderr, "unknown scenario '%s'\n", sc.c_str());
    return 2;
}
