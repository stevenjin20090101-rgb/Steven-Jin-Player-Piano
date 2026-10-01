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
struct SimChip { uint8_t addr; uint16_t ch[16]; uint32_t onSince[16]; uint32_t maxOn[16]; int lifts[16]; int strikes[16]; };
static SimChip g_chips[8]; static int g_nChips = 0;
static SimChip* chipFor(uint8_t a) { for (int i = 0; i < g_nChips; i++) if (g_chips[i].addr == a) return &g_chips[i]; return nullptr; }
// Fault injection: every OFF write (value 0, allOff, ALLCALL) fails while now is in [from, to).
static uint32_t g_failOffFrom = 1, g_failOffTo = 0;
static bool offWriteFails() { return g_now >= g_failOffFrom && g_now < g_failOffTo; }
static void setChannel(SimChip& c, int ch, uint16_t v) {
    bool was = c.ch[ch] != 0, is = v != 0;
    if (!was && is) { c.onSince[ch] = g_now; c.strikes[ch]++; }
    if (was && !is) { uint32_t d = g_now - c.onSince[ch]; if (d > c.maxOn[ch]) c.maxOn[ch] = d; c.lifts[ch]++; }
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
static void ticks() { tickAutoRelease(); tickPendingFires(); tickRestrike(); }   // same order as loop()
static void advanceTo(uint32_t t) { while (g_now < t) { g_now++; if (g_now % 10 == 0) ticks(); } }
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
    return (ceilingOk && strikesOk) ? 0 : 1;
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
    }
    fprintf(stderr, "unknown scenario '%s'\n", sc.c_str());
    return 2;
}
