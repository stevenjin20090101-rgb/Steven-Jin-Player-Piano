// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
#include "fw_info.h"
#include "config.h"
#include "fw_build.h"      // generated: FW_BUILD (tools/fw_build_id.py). Only included here.

#include <esp_ota_ops.h>

const char *fw_version_full() {
    // One literal, so `strings firmware.bin` shows "2.0.0+a1b2c3d" whole —
    // tools/publish-firmware.sh checks the image for exactly that.
    return FW_VERSION "+" FW_BUILD;
}

const char *fw_running_slot() {
    const esp_partition_t *p = esp_ota_get_running_partition();
    // Never "unknown": that word marks a refusal for the app (status is a reply).
    return p ? p->label : "?";
}

// The running partition's otadata state; false when it cannot be read.
static bool runningState(esp_ota_img_states_t &state) {
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p && esp_ota_get_state_partition(p, &state) == ESP_OK;
}

const char *fw_image_state_name() {
    esp_ota_img_states_t state;
    if (!runningState(state)) return "UNDEFINED";
    switch (state) {
        case ESP_OTA_IMG_NEW:            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:          return "VALID";
        case ESP_OTA_IMG_INVALID:        return "INVALID";
        case ESP_OTA_IMG_ABORTED:        return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    }
    return "UNDEFINED";
}

const char *fw_ota_word() {
    esp_ota_img_states_t state;
    if (!runningState(state)) return "none";
    if (state == ESP_OTA_IMG_PENDING_VERIFY) return "pending";
    if (state == ESP_OTA_IMG_VALID)          return "confirmed";
    return "none";
}
