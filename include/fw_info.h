// ============================================================================
//  Player Piano - ESP32-S3 self-playing acoustic piano
//  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
//  Original author & creator: Steven Jin.
//  Licensed under the MIT License (see LICENSE). This copyright and attribution
//  notice MUST be preserved in all copies or substantial portions of the work.
//  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
// ============================================================================
// Which firmware is this, and how did it get here? (docs/BLE_OTA.md section 2)
//
// The version string is FW_VERSION (include/config.h) + "+" + FW_BUILD, the
// git hash written by tools/fw_build_id.py into a generated header before each
// build. fw_info.cpp is the ONLY file that includes that header, so a new
// commit recompiles one file — not config.h's every includer, and not
// tools/dispatch-sim, which compiles config.h without the generated directory.
//
// The same string shows in the boot banner, `status`, `dump`/`get` (!fw) and
// on the AMOLED status line.

#pragma once

// "2.0.0+a1b2c3d" ("-dirty" after the hash when built from a changed tree).
const char *fw_version_full();

// Label of the app partition this image runs from: "app0" or "app1".
const char *fw_running_slot();

// The running image's OTA state as esp_flash_partitions.h names it: NEW,
// PENDING_VERIFY, VALID, INVALID, ABORTED or UNDEFINED. UNDEFINED also when
// the state cannot be read. After a USB flash it reads UNDEFINED.
const char *fw_image_state_name();

// The settings channel's !ota fact (BLE_SETTINGS.md section 4): "pending" while
// the image is PENDING_VERIFY, "confirmed" once VALID, otherwise "none".
const char *fw_ota_word();

// The version tag a Bluetooth update is checked against (docs/BLE_LOCK.md,
// "Updates never go backwards"; docs/BLE_OTA.md section 5): the 6 marker
// bytes and FW_VERSION as one NUL-ended string inside the signed image. The
// update session (ota_session.cpp) reads the tag of the image it receives and
// activates it only if that version is newer than the running FW_VERSION.
// Kept in the image by __attribute__((used)) and a volatile read in setup().
// Never write the marker anywhere else in the firmware: the session takes
// the first well-formed tag it finds.
extern "C" const char g_fw_version_tag[];
