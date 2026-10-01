// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#include "ble_console.h"
#include "console_out.h"

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

// ---- Nordic UART Service (docs/BLE_SETTINGS.md section 1) ------------------
static const char *kNusService = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *kNusRx      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *kNusTx      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// Longest single RX write accepted. NimBLE copies every write into a stack
// array of this size on its 4 KB host task, so keep it modest: 256 covers one
// full write at the 255-byte MTU (252 bytes). The app writes one line (≤ 80).
static const uint16_t kRxMaxLen = 256;

static const size_t kRxStreamBytes      = 1024;  // host task -> loop, framed writes
static const int    kMaxLinesPerPass    = 4;     // console lines run per loop pass
static const int    kMaxNotifiesPerPass = 8;     // TX notifications per loop pass
static const int    kMinFreeMbufs       = 5;     // leave the stack its own ATT buffers
static const int    kMaxSubscribers     = 3;     // NimBLE's connection limit

static NimBLECharacteristic *s_rxChr = nullptr;
static NimBLECharacteristic *s_txChr = nullptr;
static uint16_t s_txHandle = 0;                  // set by ble_console_check()

// ---- RX: host task -> loop --------------------------------------------------
// Each write goes into the stream buffer as one frame: a 4-byte header
// (connection handle, payload length; little-endian) and the payload. Only
// the host task writes and only the loop reads, and a write goes in whole or
// not at all, so frames never tear.
static StreamBufferHandle_t s_rxStream = nullptr;
static volatile uint32_t    s_rxDropped = 0;     // bytes lost to a full buffer (host task)
static uint32_t             s_rxCut     = 0;     // bytes of half lines dropped (loop)

class NusRxCallbacks : public NimBLECharacteristicCallbacks {
public:
    using NimBLECharacteristicCallbacks::onWrite;

    // NimBLE host task. Copy and tag only: nothing here parses, prints,
    // touches I²C, LEDs, LVGL or NVS, or keeps anything big on this stack.
    void onWrite(NimBLECharacteristic *chr, ble_gap_conn_desc *desc) override {
        if (s_rxStream == nullptr) return;
        NimBLEAttValue value = chr->getValue();     // heap copy of the write
        size_t n = value.size();
        if (n == 0) return;
        uint16_t conn = desc->conn_handle;
        uint8_t hdr[4] = { (uint8_t)(conn & 0xFF), (uint8_t)(conn >> 8),
                           (uint8_t)(n & 0xFF),    (uint8_t)(n >> 8) };
        if (xStreamBufferSpacesAvailable(s_rxStream) < sizeof(hdr) + n) {
            s_rxDropped += n;                       // zero wait: drop, never block
            return;
        }
        xStreamBufferSend(s_rxStream, hdr, sizeof(hdr), 0);
        xStreamBufferSend(s_rxStream, value.data(), n, 0);
    }
};

// ---- TX subscriptions -------------------------------------------------------
// Which connections have notifications on, written by the host task
// (onSubscribe) and read by the loop. `since` numbers each subscription, so a
// reply queued for one can never go to a later one — not even after a
// reconnection that reuses the same connection handle. 0 = free slot.
struct NusSubscriber {
    uint16_t conn;
    uint32_t since;
};
static NusSubscriber s_subs[kMaxSubscribers] = {};
static uint32_t      s_subEvents = 0;
static portMUX_TYPE  s_subMux = portMUX_INITIALIZER_UNLOCKED;

class NusTxCallbacks : public NimBLECharacteristicCallbacks {
public:
    // NimBLE host task; also called with subValue 0 when a link drops.
    void onSubscribe(NimBLECharacteristic *chr, ble_gap_conn_desc *desc,
                     uint16_t subValue) override {
        uint16_t conn = desc->conn_handle;
        bool on = (subValue & 0x0001) != 0;        // notifications
        portENTER_CRITICAL(&s_subMux);
        int slot = -1, freeSlot = -1;
        for (int i = 0; i < kMaxSubscribers; i++) {
            if (s_subs[i].since != 0 && s_subs[i].conn == conn) slot = i;
            else if (s_subs[i].since == 0 && freeSlot < 0)      freeSlot = i;
        }
        if (on && slot < 0 && freeSlot >= 0) {
            if (++s_subEvents == 0) s_subEvents = 1;
            s_subs[freeSlot].conn  = conn;
            s_subs[freeSlot].since = s_subEvents;
        } else if (!on && slot >= 0) {
            s_subs[slot].since = 0;
        }
        portEXIT_CRITICAL(&s_subMux);
    }
};

static NusRxCallbacks s_rxCallbacks;
static NusTxCallbacks s_txCallbacks;

// The subscription `conn` holds now, or 0 when it is not subscribed.
static uint32_t subscribedSince(uint16_t conn) {
    uint32_t since = 0;
    portENTER_CRITICAL(&s_subMux);
    for (int i = 0; i < kMaxSubscribers; i++) {
        if (s_subs[i].since != 0 && s_subs[i].conn == conn) since = s_subs[i].since;
    }
    portEXIT_CRITICAL(&s_subMux);
    return since;
}

// ---- Setup ------------------------------------------------------------------
void ble_console_setup(NimBLEServer *srv) {
    s_rxStream = xStreamBufferCreate(kRxStreamBytes, 1);
    if (s_rxStream == nullptr) {
        Serial.println("[ble] *** console: no memory for the RX buffer — Bluetooth settings off ***");
        return;
    }
    NimBLEService *svc = srv->createService(kNusService);
    s_rxChr = svc->createCharacteristic(kNusRx, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
                                        kRxMaxLen);
    s_txChr = svc->createCharacteristic(kNusTx, NIMBLE_PROPERTY::NOTIFY);  // CCCD added by NimBLE
    s_rxChr->setCallbacks(&s_rxCallbacks);
    s_txChr->setCallbacks(&s_txCallbacks);
    svc->start();
}

void ble_console_check() {
    // A characteristic's handle reads 0xFFFF (NimBLE's "none") until the GATT
    // server registers it. Compare with the host's own table too — that is
    // what a client discovers — so a service registered twice or empty shows.
    uint16_t rx = s_rxChr ? s_rxChr->getHandle() : 0;
    uint16_t tx = s_txChr ? s_txChr->getHandle() : 0;
    uint16_t rxHost = 0, txHost = 0;
    NimBLEUUID svc(kNusService), rxUuid(kNusRx), txUuid(kNusTx);
    ble_gatts_find_chr(&svc.getNative()->u, &rxUuid.getNative()->u, nullptr, &rxHost);
    ble_gatts_find_chr(&svc.getNative()->u, &txUuid.getNative()->u, nullptr, &txHost);
    bool ok = rx != 0 && rx != 0xFFFF && tx != 0 && tx != 0xFFFF &&
              rx == rxHost && tx == txHost;
    if (ok) {
        s_txHandle = tx;
        Serial.printf("[ble] console (Nordic UART) ready: RX handle 0x%04X, TX handle 0x%04X\n",
                      rx, tx);
    } else {
        s_txHandle = 0;
        Serial.printf("[ble] *** console (Nordic UART) NOT registered: RX 0x%04X (host 0x%04X), "
                      "TX 0x%04X (host 0x%04X) — Bluetooth settings unavailable ***\n",
                      rx, rxHost, tx, txHost);
    }
}

// ---- Allow-list (docs/BLE_SETTINGS.md section 5; plan correction 9) --------
// Default deny. A line runs over Bluetooth only if its first word is here.
static const char *const kAllowed[] = {
    // every tunable `dump` lists, except keyforce_white / keyforce_black
    "leds", "ledmode", "ledbright", "ledcount", "ledoffset", "ledscale", "ledtail",
    "ledreverse", "reactcolor", "ledglow", "velbright", "decay", "rainspeed",
    "dimsecs", "dimfloor", "keyviz", "fullpower", "volume", "velcurve", "velmult",
    "min", "minblack", "max", "humanvel", "humantime", "burstgap", "burstboost",
    "minstrike", "isostrike", "isogap", "gap", "hold", "restrike", "softrelease",
    "releasepwm", "releasems", "freq", "pedalon", "pedalhalf", "pedalup", "pedaldown",
    "minwhite",                                   // the same variable as min
    // commands
    "status", "dump", "get", "help", "?", "off", "broadcast", "panic", "save",
    "ledtest", "testmin", "testmax", "note",
    "soft", "cinematic", "expressive", "snappy",
};

// True if the first word of `line` (up to the first space) is exactly `word`.
static bool commandIs(const char *line, const char *word) {
    size_t n = strcspn(line, " ");
    return n == strlen(word) && !strncmp(line, word, n);
}

bool ble_console_allowed(const char *line) {
    for (const char *word : kAllowed) {
        if (commandIs(line, word)) return true;
    }
    return false;
}

// ---- Line assembly (loop task) ----------------------------------------------
// Its own 80-byte line buffer, never the serial reader's.
static uint16_t s_frameConn = BLE_HS_CONN_HANDLE_NONE;  // writer of the staged bytes
static uint16_t s_frameLeft = 0;                        // payload still in the stream
static uint8_t  s_stage[64];
static uint8_t  s_stageLen = 0, s_stagePos = 0;

static char     s_rxLine[80];
static uint8_t  s_rxLineLen  = 0;
static uint16_t s_rxLineConn = BLE_HS_CONN_HANDLE_NONE;   // who the half line is from

// The next received byte and the connection that wrote it; false when nothing
// is waiting.
static bool nextByte(uint8_t &c, uint16_t &conn) {
    if (s_stagePos >= s_stageLen) {
        if (s_frameLeft == 0) {
            uint8_t hdr[4];
            if (xStreamBufferBytesAvailable(s_rxStream) < sizeof(hdr)) return false;
            xStreamBufferReceive(s_rxStream, hdr, sizeof(hdr), 0);
            s_frameConn = (uint16_t)(hdr[0] | (hdr[1] << 8));
            s_frameLeft = (uint16_t)(hdr[2] | (hdr[3] << 8));
        }
        size_t want = (s_frameLeft < sizeof(s_stage)) ? s_frameLeft : sizeof(s_stage);
        size_t got = want ? xStreamBufferReceive(s_rxStream, s_stage, want, 0) : 0;
        if (got == 0) return false;                     // the rest comes next pass
        s_frameLeft -= (uint16_t)got;
        s_stageLen = (uint8_t)got;
        s_stagePos = 0;
    }
    c = s_stage[s_stagePos++];
    conn = s_frameConn;
    return true;
}

// Assembles the next complete line into s_rxLine; false when none is waiting.
// Ends on "\n" or "\r"; empty lines are skipped; a line longer than the buffer
// keeps its first 79 characters (as on USB). A half line is dropped when bytes
// from another connection arrive: a line comes from one writer.
static bool nextLine(uint16_t &conn) {
    uint8_t  c;
    uint16_t from;
    while (nextByte(c, from)) {
        if (from != s_rxLineConn) {
            s_rxCut += s_rxLineLen;
            s_rxLineLen = 0;
            s_rxLineConn = from;
        }
        if (c == '\n' || c == '\r') {
            if (s_rxLineLen > 0) {
                s_rxLine[s_rxLineLen] = 0;
                s_rxLineLen = 0;
                conn = s_rxLineConn;
                return true;
            }
        } else if (s_rxLineLen < sizeof(s_rxLine) - 1) {
            s_rxLine[s_rxLineLen++] = (char)c;
        }
    }
    return false;
}

// ---- Per-pass work (loop task) ----------------------------------------------
// The asker the TX ring's bytes are for, and its subscription.
static uint16_t s_txConn  = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_txSince = 0;

// A complete line waiting in s_rxLine (see ble_console_poll), and its writer.
// While one waits, nextLine() is not called, so s_rxLine stays intact.
static bool     s_rxLineHeld = false;
static uint16_t s_rxHeldConn = BLE_HS_CONN_HANDLE_NONE;

int ble_console_poll(ConsoleLineHandler handle) {
    if (s_rxStream == nullptr) return 0;
    int handled = 0;
    while (handled < kMaxLinesPerPass) {
        if (!s_rxLineHeld) {
            uint16_t conn;
            if (!nextLine(conn)) break;
            size_t len = strlen(s_rxLine);            // trim like handleLine does
            while (len > 0 && s_rxLine[len - 1] == ' ') s_rxLine[--len] = 0;
            if (len == 0) continue;
            s_rxLineHeld = true;
            s_rxHeldConn = conn;
        }
        uint16_t conn  = s_rxHeldConn;
        uint32_t since = subscribedSince(conn);

        // Replies go to the asker, and only while it listens on TX. The ring
        // holds one asker's reply at a time: a line from someone else waits
        // until the flush has sent the previous asker's bytes — unless that
        // asker has gone (unsubscribed, link lost, reconnected), whose bytes
        // are then no one's.
        if (since != 0 && Console.pending() > 0 && (conn != s_txConn || since != s_txSince)) {
            if (s_txSince != 0 && subscribedSince(s_txConn) == s_txSince) break;
            Console.discard();
        }
        s_rxLineHeld = false;
        handled++;
        if (since != 0) {
            s_txConn  = conn;
            s_txSince = since;
            Console.setMirror(true);
        }

        // testmin / testmax / panic hold the loop (delay()); after one,
        // nothing else runs this pass.
        bool endsPass = commandIs(s_rxLine, "testmin") || commandIs(s_rxLine, "testmax") ||
                        commandIs(s_rxLine, "panic");
        if (ble_console_allowed(s_rxLine)) {
            handle(s_rxLine, SRC_BLE);
        } else {
            Console.printf("> %s\n", s_rxLine);
            Console.println(BLE_REFUSAL);
        }
        Console.setMirror(false);
        if (endsPass) break;
    }
    return handled;
}

void ble_console_flush() {
    if (Console.pending() == 0) return;
    // The asker unsubscribed, dropped the link, or came back as a new
    // subscription: its queued reply is no longer anyone's.
    if (s_txSince == 0 || s_txHandle == 0 || subscribedSince(s_txConn) != s_txSince) {
        Console.discard();
        return;
    }
    uint16_t mtu = ble_att_mtu(s_txConn);           // 0 = no such connection
    if (mtu <= 3) {
        Console.discard();
        return;
    }
    static uint8_t chunk[252];                       // MTU 255 - 3; loop task only
    size_t chunkMax = mtu - 3;
    if (chunkMax > sizeof(chunk)) chunkMax = sizeof(chunk);

    // The checked low-level send (never NimBLECharacteristic::notify(), which
    // ignores a failed mbuf allocation and the stack's return code).
    for (int i = 0; i < kMaxNotifiesPerPass && Console.pending() > 0; i++) {
        if (os_msys_num_free() < kMinFreeMbufs) break;    // the stack needs its share
        size_t n = Console.peek(chunk, chunkMax);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(chunk, (uint16_t)n);
        if (om == nullptr) break;                         // retry next pass
        int rc = ble_gattc_notify_custom(s_txConn, s_txHandle, om);  // consumes om
        if (rc == BLE_HS_ENOTCONN) {
            Console.discard();
            break;
        }
        if (rc != 0) break;                               // keep the bytes; retry next pass
        Console.consume(n);
    }
}

void ble_console_print_status(Print &out) {
    int subscribers = 0;
    portENTER_CRITICAL(&s_subMux);
    for (int i = 0; i < kMaxSubscribers; i++) {
        if (s_subs[i].since != 0) subscribers++;
    }
    portEXIT_CRITICAL(&s_subMux);
    out.printf("  bluetooth console: %d subscribed  rx dropped=%lu B  tx dropped=%lu B\n",
               subscribers, (unsigned long)(s_rxDropped + s_rxCut),
               (unsigned long)Console.dropped());
}
