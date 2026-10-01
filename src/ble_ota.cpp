// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Firmware updates over Bluetooth: the glue (see include/ble_ota.h). The
// protocol is ota::Session (src/ota_session.cpp, proven by tools/ota-sim);
// everything here is the device side of its ota::Port, the two queues that
// carry frames from the NimBLE task to loop(), rollback and the self-test.
#include "ble_ota.h"
#include "ota_session.h"
#include "ota_pubkey.h"
#include "config.h"
#include "fw_info.h"
#include "power_boards.h"
#include "settings.h"

#include <NimBLEDevice.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/sha256.h>
#include <sodium.h>
#include <lvgl.h>          // LV_SYMBOL_BULLET only — this file makes no LVGL call

// ---- The Update service (docs/BLE_OTA.md section 3) -------------------------
static const char *kUpdateService = "7D0A0001-3F5E-4B8C-9A2D-6E1F0C4B8A17";
static const char *kControlChr    = "7D0A0002-3F5E-4B8C-9A2D-6E1F0C4B8A17";
static const char *kDataChr       = "7D0A0003-3F5E-4B8C-9A2D-6E1F0C4B8A17";

static const uint16_t    kControlMax       = 118;    // BEGIN, the longest Control write
static const uint16_t    kDataMax          = 252;    // seq u16 + at most 250 payload bytes
static const UBaseType_t kControlDepth     = 4;
static const UBaseType_t kDataDepth        = 34;     // a 32-frame window and two to spare
static const size_t      kDataBytesPerPass = 4096;   // at most one sector program (+ one 64 KB
                                                     // block erase at a boundary) per loop pass

// ---- NVS (namespace "piano", like the settings: an app-slot update keeps it) -
static const char *kNvsNamespace = "piano";
static const char *kKeyOtaPart   = "otaPart";      // the slot an activated update went to
static const char *kKeyOtaVer    = "otaVer";       // ... and its version
static const char *kKeyBoards    = "boardsSeen";   // a power board was online once on this piano

// ---- Rollback and the self-test (docs/BLE_OTA.md section 9) -----------------
static const uint32_t kSelfTestFromMs = 30000;   // the uptime that also clears the crash counter
static const uint32_t kNoticeMs       = 60000;   // "Updated to …" / "… rolled back" on screen

// ---- Status-line colours (the BLE line is green 0x60FF60 / amber 0xFFB060) ----
static const uint32_t kRgbUpdating   = 0x60C0FF;
static const uint32_t kRgbUpdated    = 0x60FF60;
static const uint32_t kRgbRolledBack = 0xFFB060;

// The Arduino core marks every image valid at startup unless the sketch
// overrides this weak C hook (cores/esp32/esp32-hal-misc.c). Returning true
// keeps a new image PENDING_VERIFY until the self-test below confirms it; a
// reset of any kind before that makes the bootloader roll back. C linkage: a
// C++ definition would be name-mangled and silently ignored (the build check
// is `nm firmware.elf | grep -w verifyRollbackLater` showing T, not W).
extern "C" bool verifyRollbackLater() { return true; }

// ---- Frames: NimBLE host task -> loop -----------------------------------------
struct ControlItem { uint16_t conn; uint8_t  len; uint8_t b[kControlMax]; };
struct DataItem    { uint16_t conn; uint16_t len; uint8_t b[kDataMax]; };

// Static queues: made once in setup(), never in a callback; their RAM shows in
// the build's figure and cannot fail at run time.
static StaticQueue_t s_ctrlQueueBuf, s_dataQueueBuf;
static uint8_t       s_ctrlQueueStore[kControlDepth * sizeof(ControlItem)];
static uint8_t       s_dataQueueStore[kDataDepth * sizeof(DataItem)];
static QueueHandle_t s_ctrlQ = nullptr, s_dataQ = nullptr;

// Written by loop() after every call into the session, read by the Data
// callback: frames are queued only while the session receives, and only from
// its owner. Opening writes the owner before the flag, closing clears the flag
// first; a frame that slips past still meets the session's own owner check.
static volatile bool     s_ready         = false;          // ble_ota_check() passed
static volatile bool     s_acceptData    = false;
static volatile uint16_t s_owner         = ota::NO_CONN;
static volatile bool     s_sessionActive = false;          // ble_ota_session_active()
static volatile bool     s_dataDropped   = false;          // host task: the Data queue was full
static volatile uint32_t s_ctrlDropped   = 0;              // host task: Control frames lost

// Host-task scratch, one item each: nothing frame-sized on the 4 KB host stack.
static ControlItem s_ctrlIn;
static DataItem    s_dataIn;

class ControlCallbacks : public NimBLECharacteristicCallbacks {
public:
    using NimBLECharacteristicCallbacks::onWrite;
    // NimBLE host task: copy and tag only.
    void onWrite(NimBLECharacteristic *chr, ble_gap_conn_desc *desc) override {
        if (!s_ready) return;
        NimBLEAttValue v = chr->getValue();
        const size_t n = v.size();
        if (n > sizeof(s_ctrlIn.b)) return;          // NimBLE refuses longer writes already
        s_ctrlIn.conn = desc->conn_handle;
        s_ctrlIn.len  = (uint8_t)n;
        if (n) memcpy(s_ctrlIn.b, v.data(), n);
        if (xQueueSend(s_ctrlQ, &s_ctrlIn, 0) != pdTRUE) s_ctrlDropped = s_ctrlDropped + 1;
    }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
public:
    using NimBLECharacteristicCallbacks::onWrite;
    // NimBLE host task: the owner's frames while the session receives, copied
    // and tagged; stale and foreign frames go nowhere. Never blocks: a full
    // queue drops the frame and says so (the session answers ERR 4).
    void onWrite(NimBLECharacteristic *chr, ble_gap_conn_desc *desc) override {
        if (!s_ready || !s_acceptData || desc->conn_handle != s_owner) return;
        NimBLEAttValue v = chr->getValue();
        const size_t n = v.size();
        if (n > sizeof(s_dataIn.b)) return;          // NimBLE refuses longer writes already
        s_dataIn.conn = desc->conn_handle;
        s_dataIn.len  = (uint16_t)n;
        if (n) memcpy(s_dataIn.b, v.data(), n);
        if (xQueueSend(s_dataQ, &s_dataIn, 0) != pdTRUE) s_dataDropped = true;
    }
};

static ControlCallbacks      s_ctrlCallbacks;
static DataCallbacks         s_dataCallbacks;
static NimBLECharacteristic *s_ctrlChr = nullptr;
static NimBLECharacteristic *s_dataChr = nullptr;
static NimBLECharacteristic *s_fwRevChr = nullptr;   // 0x2A26
static uint16_t              s_ctrlHandle = 0;       // set by ble_ota_check()

// ---- Loop-task state ----------------------------------------------------------
static ControlItem s_ctrlOut;                 // loop-task scratch (the queues copy)
static DataItem    s_dataOut;

static mbedtls_sha256_context s_sha;
static bool     s_shaOk      = false;         // shaStart() and every step since succeeded
static bool     s_sodiumOk   = false;         // sodium_init() succeeded: else nothing verifies
static char     s_targetSlot[17] = "";        // the slot flashBegin() opened ("app1")

static bool     s_runningPending  = false;    // the running image is PENDING_VERIFY
static bool     s_boardsSeenAtBoot = false;   // NVS boardsSeen as an earlier boot left it
static bool     s_boardsSeenStored = false;   // set in NVS, by an earlier boot or this one
static uint8_t  s_selfTestWaiting  = 0xFF;    // what the self-test last waited for (logged on change)
static uint32_t s_selfTestRetryMs  = 0;       // after a failed mark-valid: not before this

static char     s_sessionText[40] = "";       // "Updating • 38 %" while a session runs
static char     s_noticeText[48]  = "";       // an outcome, shown for kNoticeMs
static uint32_t s_noticeRgb       = 0;
static uint32_t s_noticeSinceMs   = 0;        // when first shown; 0 = not yet

static uint32_t s_passPrevMs    = 0;          // longest loop pass while a session runs
static uint32_t s_passLongestMs = 0;
static uint32_t s_ctrlDroppedSeen = 0;

static bool anyBoardOnline() {
    for (int i = 0; i < kNumPowerBoards; i++) {
        if (powerBoards[i].connected) return true;
    }
    return false;
}

static void logLongestPass() {
    Serial.printf("[ota] longest pass %lu ms\n", (unsigned long)s_passLongestMs);
}

static void setNotice(const char *text, uint32_t rgb) {
    snprintf(s_noticeText, sizeof s_noticeText, "%s", text);
    s_noticeRgb     = rgb;
    s_noticeSinceMs = 0;                       // the 60 s start when it is first shown
}

// Erases otaPart and otaVer from an open RW namespace (only the keys that exist:
// removing a missing key logs an error).
static void eraseUpdateRecord(Preferences &p) {
    if (p.isKey(kKeyOtaPart)) p.remove(kKeyOtaPart);
    if (p.isKey(kKeyOtaVer))  p.remove(kKeyOtaVer);
}

// ---- The device side of the session -------------------------------------------
class DevicePort : public ota::Port {
public:
    uint32_t nowMs() override { return millis(); }

    bool linkAlive(uint16_t conn) override {
        return conn != ota::NO_CONN && ble_gap_conn_find(conn, nullptr) == 0;
    }

    uint16_t mtu(uint16_t conn) override { return ble_att_mtu(conn); }   // 0 when the link is gone

    // To that connection only, with the checked low-level send. Never a NULL
    // mbuf: NimBLE would notify the characteristic's stored value instead.
    bool notify(uint16_t conn, const uint8_t *bytes, size_t n) override {
        if (s_ctrlHandle == 0 || bytes == nullptr || n == 0) return false;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(bytes, (uint16_t)n);
        if (om == nullptr) return false;
        return ble_gattc_notify_custom(conn, s_ctrlHandle, om) == 0;   // consumes om either way
    }

    bool crashSafeMode() override { return g_crashSafeMode; }
    bool runningImagePending() override { return s_runningPending; }

    uint32_t slotBytes() override {
        const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
        return p ? p->size : 0;
    }

    bool signatureOk(const uint8_t sig[64], const uint8_t digest[32]) override {
        return s_sodiumOk && crypto_sign_verify_detached(sig, digest, 32, kOtaPublicKey) == 0;
    }

    void enterUpdateMode() override {
        set_energize_inhibit(true);   // FIRST: the dispatch layer refuses every energize from here
        s_sessionActive = true;       // the note funnel drops notes before the queue is flushed
        piano_stop_for_update();      // flush, allKeysOff(), reactive LEDs, pedal up, strip dark
    }

    void leaveUpdateMode() override {
        set_energize_inhibit(false);
        s_sessionActive  = false;
        s_sessionText[0] = '\0';
        piano_resume_after_update();  // a full LED frame in the current mode, the BLE status line
    }

    bool     coilsVerifiedOff() override { return ::coilsVerifiedOff(); }
    uint32_t lastEnergizeMs() override   { return ::lastEnergizeMs(); }

    uint8_t flashBegin(uint32_t size) override {
        const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);   // what Update opens
        snprintf(s_targetSlot, sizeof s_targetSlot, "%s", p ? p->label : "");
        if (Update.begin(size, U_FLASH)) return 0;
        return Update.getError() == UPDATE_ERROR_SIZE ? ota::ERR_TOO_LARGE : ota::ERR_FLASH;
    }

    uint8_t flashWrite(const uint8_t *bytes, size_t n) override {
        // Update.write() takes a non-const pointer; it only copies from it.
        if (Update.write(const_cast<uint8_t *>(bytes), n) == n) return 0;
        return Update.getError() == UPDATE_ERROR_MAGIC_BYTE ? ota::ERR_IMAGE : ota::ERR_FLASH;
    }

    // The one blocking step of a session: writes the held-back first 16 bytes,
    // re-reads the header, then esp_ota_set_boot_partition() validates the whole
    // image and switches otadata. The coils are verified off and inhibited.
    uint8_t flashEnd() override {
        if (!s_shaOk) {                 // never activate after a SHA-256 engine error
            Serial.println("[ota] the SHA-256 engine reported an error — slot not activated");
            return ota::ERR_FLASH;
        }
        esp_task_wdt_reset();
        if (Update.end(false)) return 0;
        const uint8_t e = Update.getError();
        return (e == UPDATE_ERROR_ACTIVATE || e == UPDATE_ERROR_MAGIC_BYTE) ? ota::ERR_IMAGE
                                                                            : ota::ERR_FLASH;
    }

    // Every return to idle, also when nothing was opened. Update.abort() only
    // while an update is open: until the first begin() its LED pin field is 0
    // (never initialised) and abort() would write GPIO 0. It frees the 4 KB
    // buffer and erases nothing; the partial slot cannot boot (Update holds the
    // image's first 16 bytes back until end()) and otadata is untouched.
    void flashAbort() override {
        if (Update.isRunning()) Update.abort();
        mbedtls_sha256_free(&s_sha);
        mbedtls_sha256_init(&s_sha);
        s_shaOk = false;
    }

    void shaStart() override {
        mbedtls_sha256_free(&s_sha);
        mbedtls_sha256_init(&s_sha);
        s_shaOk = mbedtls_sha256_starts_ret(&s_sha, 0) == 0;
    }

    void shaUpdate(const uint8_t *bytes, size_t n) override {
        if (mbedtls_sha256_update_ret(&s_sha, bytes, n) != 0) s_shaOk = false;
    }

    void shaFinish(uint8_t out[32]) override {
        if (mbedtls_sha256_finish_ret(&s_sha, out) != 0) s_shaOk = false;
        if (!s_shaOk) memset(out, 0, 32);
    }

    void onProgress(ota::State state, uint32_t received, uint32_t size) override {
        switch (state) {
            case ota::PREPARING:
            case ota::RECEIVING:
                snprintf(s_sessionText, sizeof s_sessionText, "Updating " LV_SYMBOL_BULLET " %u %%",
                         size ? (unsigned)(((uint64_t)received * 100u) / size) : 0u);
                break;
            case ota::VERIFYING:
                snprintf(s_sessionText, sizeof s_sessionText, "Updating " LV_SYMBOL_BULLET " checking");
                break;
            case ota::REBOOTING:
                snprintf(s_sessionText, sizeof s_sessionText, "Updating " LV_SYMBOL_BULLET " restarting");
                break;
            case ota::IDLE:
            default:
                s_sessionText[0] = '\0';
                break;
        }
    }

    // Verified and activated; OK goes out next. The settings as they are now
    // (the new image reads them), then the record the next boot checks.
    void onActivated(const char *version) override {
        settings_save();
        Preferences p;
        if (p.begin(kNvsNamespace, false)) {
            p.putString(kKeyOtaPart, s_targetSlot);
            p.putString(kKeyOtaVer, version);
            p.end();
        } else {
            Serial.println("[ota] NVS unavailable — this update will not be reported after the restart");
        }
    }

    // OK + 1.5 s: drop the app's link, let it and the log go out, one more
    // ALLCALL all-off (every coil has been verified off and inhibited since
    // READY), reboot into the new slot.
    void restart(uint16_t conn) override {
        logLongestPass();
        NimBLEServer *srv = NimBLEDevice::getServer();
        if (srv != nullptr && linkAlive(conn)) srv->disconnect(conn);
        delay(100);
        broadcastAllOff();
        ESP.restart();
    }

    void log(const char *line) override { Serial.println(line); }
};

static DevicePort   s_port;
static ota::Session s_session(s_port);

// After every call into the session: the gate the Data callback reads, and the
// flag the rest of the firmware reads.
static void syncGate() {
    const uint16_t owner = s_session.owner();
    if (s_session.acceptsData()) {
        s_owner      = owner;
        s_acceptData = true;
    } else {
        s_acceptData = false;
        s_owner      = owner;
    }
    s_sessionActive = s_session.active();
}

// ---- Setup ------------------------------------------------------------------
void ble_ota_setup(NimBLEServer *srv) {
    s_ctrlQ = xQueueCreateStatic(kControlDepth, sizeof(ControlItem), s_ctrlQueueStore, &s_ctrlQueueBuf);
    s_dataQ = xQueueCreateStatic(kDataDepth, sizeof(DataItem), s_dataQueueStore, &s_dataQueueBuf);
    mbedtls_sha256_init(&s_sha);
    s_sodiumOk = sodium_init() >= 0;              // 0 started, 1 already running
    if (!s_sodiumOk) {
        Serial.println("[ota] *** libsodium did not start — every update will be refused (ERR 6) ***");
    }

    // Device Information: read by the app after every connection, so it knows
    // the version that is running. Values as std::string, never const char*
    // (NimBLE 1.4.3's template would store sizeof(pointer) bytes).
    NimBLEService *dis = srv->createService(NimBLEUUID((uint16_t)0x180A));
    dis->createCharacteristic(NimBLEUUID((uint16_t)0x2A29), NIMBLE_PROPERTY::READ)
       ->setValue(std::string("Steven Jin"));
    dis->createCharacteristic(NimBLEUUID((uint16_t)0x2A24), NIMBLE_PROPERTY::READ)
       ->setValue(std::string(BLE_MIDI_DEVICE_NAME));
    s_fwRevChr = dis->createCharacteristic(NimBLEUUID((uint16_t)0x2A26), NIMBLE_PROPERTY::READ);
    s_fwRevChr->setValue(std::string(fw_version_full()));
    dis->start();

    NimBLEService *svc = srv->createService(kUpdateService);
    s_ctrlChr = svc->createCharacteristic(kControlChr, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY,
                                          kControlMax);   // CCCD added by NimBLE
    s_dataChr = svc->createCharacteristic(kDataChr, NIMBLE_PROPERTY::WRITE_NR, kDataMax);
    s_ctrlChr->setCallbacks(&s_ctrlCallbacks);
    s_dataChr->setCallbacks(&s_dataCallbacks);
    svc->start();
}

void ble_ota_check() {
    // Handles read 0xFFFF until the GATT server registers them; compare with
    // the host's own table too, which is what a client discovers.
    const uint16_t ctrl = s_ctrlChr ? s_ctrlChr->getHandle() : 0;
    const uint16_t data = s_dataChr ? s_dataChr->getHandle() : 0;
    const uint16_t rev  = s_fwRevChr ? s_fwRevChr->getHandle() : 0;
    uint16_t ctrlHost = 0, dataHost = 0, revHost = 0;
    NimBLEUUID svc(kUpdateService), ctrlUuid(kControlChr), dataUuid(kDataChr);
    NimBLEUUID dis((uint16_t)0x180A), revUuid((uint16_t)0x2A26);
    ble_gatts_find_chr(&svc.getNative()->u, &ctrlUuid.getNative()->u, nullptr, &ctrlHost);
    ble_gatts_find_chr(&svc.getNative()->u, &dataUuid.getNative()->u, nullptr, &dataHost);
    ble_gatts_find_chr(&dis.getNative()->u, &revUuid.getNative()->u, nullptr, &revHost);
    const bool ctrlOk = ctrl != 0 && ctrl != 0xFFFF && ctrl == ctrlHost;
    const bool dataOk = data != 0 && data != 0xFFFF && data == dataHost;
    const bool revOk  = rev != 0 && rev != 0xFFFF && rev == revHost;
    if (ctrlOk && dataOk && s_ctrlQ != nullptr && s_dataQ != nullptr) {
        s_ctrlHandle = ctrl;
        s_ready      = true;
        Serial.printf("[ble] firmware update ready: Control 0x%04X, Data 0x%04X; "
                      "Device Information 0x2A26 0x%04X%s = \"%s\"\n",
                      ctrl, data, rev, revOk ? "" : " (NOT registered)", fw_version_full());
    } else {
        s_ctrlHandle = 0;
        s_ready      = false;
        Serial.printf("[ble] *** firmware update NOT registered: Control 0x%04X (host 0x%04X), "
                      "Data 0x%04X (host 0x%04X), 0x2A26 0x%04X (host 0x%04X) — "
                      "updates over Bluetooth unavailable ***\n",
                      ctrl, ctrlHost, data, dataHost, rev, revHost);
    }
}

// ---- Boot report (right after the [fw] banner) ------------------------------
void ble_ota_boot_report() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    s_runningPending = running != nullptr &&
                       esp_ota_get_state_partition(running, &state) == ESP_OK &&
                       state == ESP_OTA_IMG_PENDING_VERIFY;

    Preferences p;
    if (!p.begin(kNvsNamespace, false)) {
        Serial.println("[ota] NVS unavailable — no update record to check");
        return;
    }
    s_boardsSeenAtBoot = p.getBool(kKeyBoards, false);
    s_boardsSeenStored = s_boardsSeenAtBoot;

    char part[17] = "", ver[17] = "";
    if (p.isKey(kKeyOtaPart)) p.getString(kKeyOtaPart, part, sizeof part);
    if (p.isKey(kKeyOtaVer))  p.getString(kKeyOtaVer, ver, sizeof ver);
    const char *slot = fw_running_slot();

    if (part[0] && strcmp(part, slot) != 0) {
        // The update was activated but this boot runs the other slot: it
        // crashed, lost power or failed its self-test, and the bootloader
        // rolled back (or USB flashed over it).
        Serial.printf("[ota] update to %s (%s) was not confirmed — running %s\n",
                      ver[0] ? ver : "?", part, slot);
        char text[48];
        if (ver[0]) snprintf(text, sizeof text, "Update to %s rolled back", ver);
        else        snprintf(text, sizeof text, "Update rolled back");
        setNotice(text, kRgbRolledBack);
        eraseUpdateRecord(p);
    } else if (part[0] && s_runningPending) {
        Serial.printf("[ota] running the update to %s in %s — it confirms itself after 30 s up "
                      "with Bluetooth on%s; a reset before that rolls back\n",
                      ver[0] ? ver : "?", slot,
                      s_boardsSeenAtBoot ? " and a power board online" : "");
    } else if (part[0]) {
        // This slot, already confirmed (or USB flashed over it): the record
        // outlived it, e.g. power lost between the confirm and its erase.
        Serial.printf("[ota] stale update record (%s in %s) cleared\n", ver[0] ? ver : "?", part);
        eraseUpdateRecord(p);
    } else if (s_runningPending) {
        Serial.printf("[ota] this firmware is on trial (no update record) — it confirms itself "
                      "after 30 s up with Bluetooth on%s\n",
                      s_boardsSeenAtBoot ? " and a power board online" : "");
    }
    p.end();
}

// ---- Every loop pass ----------------------------------------------------------

// NVS boardsSeen, once, the first time any power board is online on this piano:
// from the next boot on, a new image needs a board online to confirm itself
// (BLE_OTA.md section 15.3), while a bench board that never saw the trunk
// still can.
static void noteBoardsSeen() {
    if (s_boardsSeenStored || !anyBoardOnline()) return;
    s_boardsSeenStored = true;                    // once, whether or not the write lands
    Preferences p;
    if (p.begin(kNvsNamespace, false)) {
        p.putBool(kKeyBoards, true);
        p.end();
    }
    Serial.println("[ota] a power board is online: new firmware will need one online to confirm itself");
}

// While the running image is PENDING_VERIFY, from 30 s of uptime, every pass:
// (a) 30 s up, (b) a power board online — only if one was ever seen on this
// piano before this boot, (c) Bluetooth up. All true: confirm. No deadline —
// an image that never passes stays pending until the next reset rolls it back.
static void selfTest(uint32_t now) {
    if (!s_runningPending || now < kSelfTestFromMs) return;
    if (s_selfTestRetryMs != 0 && (int32_t)(now - s_selfTestRetryMs) < 0) return;

    const bool boardOk = !s_boardsSeenAtBoot || anyBoardOnline();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    const bool bleOk = (adv != nullptr && adv->isAdvertising()) || appState.bleConnected;
    const uint8_t waiting = (boardOk ? 0 : 1) | (bleOk ? 0 : 2);
    if (waiting != 0) {
        if (waiting != s_selfTestWaiting) {
            s_selfTestWaiting = waiting;
            Serial.printf("[ota] self-test waiting:%s%s\n",
                          boardOk ? "" : " no power board online",
                          bleOk ? "" : " Bluetooth neither advertising nor connected");
        }
        return;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        Serial.printf("[ota] could not confirm %s (%s) — trying again in 5 s\n",
                      fw_version_full(), esp_err_to_name(err));
        s_selfTestRetryMs = (now + 5000) ? (now + 5000) : 1;
        return;
    }
    s_runningPending = false;
    Serial.printf("[ota] %s confirmed\n", fw_version_full());
    Preferences p;
    if (p.begin(kNvsNamespace, false)) {
        eraseUpdateRecord(p);
        p.end();
    }
    setNotice("Updated to " FW_VERSION, kRgbUpdated);
}

void ble_ota_tick() {
    const uint32_t now = millis();
    noteBoardsSeen();
    selfTest(now);
    if (!s_ready) return;

    // The longest pass while a session runs: from one call to the next.
    const bool wasActive = s_session.active();
    if (wasActive) {
        const uint32_t d = now - s_passPrevMs;
        if (d > s_passLongestMs) s_passLongestMs = d;
        s_passPrevMs = now;
    }

    // 1. The session's own work: notifications to retry, the verified-OFF
    //    wait, the frame timeout, the check after END, the restart.
    s_session.tick();
    syncGate();

    // 2. At most one Control frame, so BEGIN's early signature check runs at
    //    most once a pass.
    if (xQueueReceive(s_ctrlQ, &s_ctrlOut, 0) == pdTRUE) {
        s_session.onControl(s_ctrlOut.conn, s_ctrlOut.b, s_ctrlOut.len);
        syncGate();
    }

    // 3. Data frames until 4,096 payload bytes have been handed over: at most
    //    one 4 KB sector program (and, at a 64 KB boundary, one block erase)
    //    per pass, so the safety ticks keep their cadence.
    size_t handed = 0;
    while (s_session.acceptsData() && xQueuePeek(s_dataQ, &s_dataOut, 0) == pdTRUE) {
        const size_t payload = s_dataOut.len > 2 ? (size_t)s_dataOut.len - 2 : 0;
        if (handed > 0 && handed + payload > kDataBytesPerPass) break;   // the rest next pass
        xQueueReceive(s_dataQ, &s_dataOut, 0);
        s_session.onData(s_dataOut.conn, s_dataOut.b, s_dataOut.len);
        syncGate();
        handed += payload;
    }

    // 4. A Data frame the callback could not queue: the image has a hole.
    if (s_dataDropped && s_session.acceptsData()) {
        s_dataDropped = false;
        s_session.onDataDropped();
        syncGate();
    }
    if (!s_session.acceptsData()) {               // nothing queued outlives a session
        if (uxQueueMessagesWaiting(s_dataQ) > 0) xQueueReset(s_dataQ);
        s_dataDropped = false;
    }

    const uint32_t ctrlDropped = s_ctrlDropped;
    if (ctrlDropped != s_ctrlDroppedSeen) {
        Serial.printf("[ota] %lu Control frame(s) dropped: queue full\n",
                      (unsigned long)(ctrlDropped - s_ctrlDroppedSeen));
        s_ctrlDroppedSeen = ctrlDropped;
    }

    const bool active = s_session.active();
    if (active && !wasActive) {                   // a session began in this pass
        s_passPrevMs    = now;
        s_passLongestMs = 0;
    } else if (!active && wasActive) {            // ... or ended
        logLongestPass();
    }
}

bool ble_ota_session_active() { return s_sessionActive; }

bool ble_ota_status_text(char *buf, size_t len, uint32_t *rgb) {
    if (buf == nullptr || len == 0) return false;
    buf[0] = '\0';
    if (s_sessionText[0]) {
        snprintf(buf, len, "%s", s_sessionText);
        if (rgb) *rgb = kRgbUpdating;
        return true;
    }
    if (s_noticeText[0]) {
        const uint32_t now = millis();
        if (s_noticeSinceMs == 0) s_noticeSinceMs = now ? now : 1;
        if (now - s_noticeSinceMs < kNoticeMs) {
            snprintf(buf, len, "%s", s_noticeText);
            if (rgb) *rgb = s_noticeRgb;
            return true;
        }
        s_noticeText[0] = '\0';
    }
    return false;
}
