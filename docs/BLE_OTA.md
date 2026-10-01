# Bluetooth firmware update — spec for the firmware

**Purpose.** Let the "Steven Piano" Android app (`Player Piano/android/`) update the piano's
firmware over the Bluetooth link it already holds. Nothing unsigned ever boots, a bad update
rolls itself back, and every layer of `docs/SAFETY.md` stays in force during the transfer. This
is the contract between the firmware and the app (plan M21, app 1.6.1). Written by the app-side
session on 2026-09-27 from a read of `src/`, `include/`, `platformio.ini`, `provenance/` and the
code actually linked — Arduino core 2.0.14 (ESP-IDF 4.4), NimBLE-Arduino 1.4.3, BLE-MIDI 2.2,
LVGL 8.4.0; line numbers are that day's working tree. Implement it together with
`docs/BLE_SETTINGS.md` (same threading rule; its `!fw` carries the version defined here) as **2.0.0**.

## 1. Scope and ground truth

- **In scope:** app-slot updates over BLE, a readable version, signing, rollback, the release
  manifest and script. **Out of scope:** Wi-Fi; bootloader or partition-table changes (USB only:
  `usbOnly`, §10); secure boot; flash encryption.
- **USB stays** the bench and bootstrap path: 2.0.0 goes on over USB once. A USB upload also
  writes `boot_app0.bin` to `otadata` (framework `tools/platformio-build.py:209–216`), so after
  any USB flash the piano boots `app0`; each Bluetooth update then writes the other slot.

| Fact | Verified in |
|---|---|
| 16 MB flash, `default_16MB.csv`: `nvs` 0x9000 (20 KiB), `otadata` 0xE000, `app0` 0x10000, `app1` 0x650000, 0x640000 = 6,553,600 B each | `platformio.ini:12`; `tools/partitions/default_16MB.csv` |
| `firmware.bin` 966,912 B (build of 2026-09-24): magic 0xE9, chip id 0x0009, SHA-256 appended | `.pio/build/lilygo-t4-s3/firmware.bin` |
| Settings live in NVS namespace `piano`, so an app-slot update keeps them | `settings.cpp:20` |
| Rollback compiled in, bootloader included (`bootloader_qio_80m.elf` has `write_otadata`); anti-rollback, secure boot, flash encryption off; bootloader log level 0 (silent) | framework `tools/sdk/esp32s3/sdkconfig:52, 60–61, 75–77` |
| The core marks every image valid at startup unless the sketch overrides `verifyRollbackLater()` | `cores/esp32/esp32-hal-misc.c:203–209, 222–239` |
| NimBLE-Arduino **1.4.3** linked: preferred MTU 255, 3 connections, host task on core 0 | `nimconfig.h:212–213, 224–225, 245–246`; `.pio/build/lilygo-t4-s3/lib6f3/` |
| **libsodium is linked**: `liblibsodium.a` exports `crypto_sign_verify_detached`; include paths and `-llibsodium` are already in the build, so no verifier is bundled | framework `tools/sdk/esp32s3/lib/`; `tools/platformio-build-esp32s3.py:246–247, 321` |

**Traps.** (1) `.pio/libdeps` also holds NimBLE-Arduino **2.5.0** (pulled by BLE-MIDI's
`depends=`); the build links 1.4.3 (`lib_deps` pins `^1.4.1`): write against 1.4.3, whose
callbacks take `ble_gap_conn_desc*`. (2) The rollback hook needs C linkage (§9). (3) NimBLE
1.4.3's templated `setValue(const T&)` stores `sizeof(T)` bytes for a `const char*` or a string
literal (`NimBLEAttValue.h:221–244`): pass `std::string`. (4) The AMOLED fonts lack "·" U+00B7
(`lv_font_montserrat_16.c:4`: 0x20–0x7F, 0xB0, 0x2022): screen texts use `LV_SYMBOL_BULLET` "•".

## 2. Versioning

**`FW_VERSION`** in `include/config.h`, exactly one line of this form (the release script
parses it): `#define FW_VERSION "2.0.0"` — semver; 2.0.0 is the first release with this spec.
**`FW_BUILD`** is the 7-character git hash, `-dirty` appended when the tree has changes
(untracked files included), `nogit` without git. A pre-script writes it into a generated header
(a `-D` flag would change every file's and library's flags per commit and rebuild LVGL).
`platformio.ini`: `extra_scripts = pre:tools/fw_build_id.py`

```python
# tools/fw_build_id.py — PlatformIO pre-script: <build dir>/generated/fw_build.h
Import("env")
import os, subprocess
def git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=env.subst("$PROJECT_DIR"),
                                       stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""
build = git("rev-parse", "--short=7", "HEAD") or "nogit"
if build != "nogit" and git("status", "--porcelain"):
    build += "-dirty"
gen = os.path.join(env.subst("$BUILD_DIR"), "generated")
os.makedirs(gen, exist_ok=True)
path, text = os.path.join(gen, "fw_build.h"), '#pragma once\n#define FW_BUILD "%s"\n' % build
if not os.path.exists(path) or open(path).read() != text:   # rewrite only on change
    open(path, "w").write(text)
env.Append(CPPPATH=[gen])
```

**The version string** is `FW_VERSION "+" FW_BUILD`, e.g. `2.0.0+a1b2c3d`, from
`const char *fw_version_full()` in `src/ble_ota.cpp`, the only file that includes `fw_build.h`
(not `config.h`: every project file would recompile per commit, and `tools/dispatch-sim`
compiles `config.h` without the generated directory). Reported identically in:

| Where | What |
|---|---|
| Device Information `0x180A` | Firmware Revision String `0x2A26` = the version string; Manufacturer Name `0x2A29` = `Steven Jin`; Model Number `0x2A24` = `Steven Piano` |
| Settings channel `dump`/`get` | `!fw=2.0.0+a1b2c3d` (replaces BLE_SETTINGS' "git short hash or build date") |
| Serial boot banner | after `logResetReason()` (`main.cpp:1287`): `[fw] 2.0.0+a1b2c3d  running app0  state UNDEFINED` (state names of `esp_flash_partitions.h:50–55`) |
| `status` | first line: `  firmware 2.0.0+a1b2c3d  running app0 (UNDEFINED)` |
| AMOLED status line | `lbl_status` (`ui.cpp:328–330`): `ui_refresh_status()` (`ui.cpp:955–966`) appends ` • ` and the version string to both BLE texts; §7 lists what replaces it during an update |

## 3. The GATT services

| Item | Value |
|---|---|
| Device Information | `0x180A`: `0x2A29`, `0x2A24`, `0x2A26`, READ, set once in `setup()` |
| Update service | `7D0A0001-3F5E-4B8C-9A2D-6E1F0C4B8A17` |
| `Control` | `7D0A0002-3F5E-4B8C-9A2D-6E1F0C4B8A17`: WRITE (with response) + NOTIFY (CCCD 0x2902), max length 118 |
| `Data` | `7D0A0003-3F5E-4B8C-9A2D-6E1F0C4B8A17`: WRITE_NR only, max length 252 |
| Security | none, like BLE-MIDI and NUS (no bonding, no encryption); the signature (§8) is the security |
| Advertising | **unchanged** (MIDI UUID, flags, appearance: 25 of 31 bytes); the app finds both services by discovery |

The server then holds BLE-MIDI, NUS, Device Information and Update (plus NimBLE's GAP/GATT).
Create DIS and Update in `setup()` after `MIDI.begin()` and the security calls
(`main.cpp:1407–1436`), next to NUS, before the advertising block (`main.cpp:1443–1450`) —
BLE_SETTINGS §1 explains why `createService()` → `resetGATT()` is harmless there. Keep
BLE-MIDI's server callbacks; use characteristic callbacks only.

**MTU.** `NimBLEDevice::setMTU(255)` (the `nimconfig.h` default, made explicit). At BEGIN, read
the writer's negotiated MTU with `NimBLEDevice::getServer()->getPeerMTU(conn)` (→ `ble_att_mtu()`:
23 if the app never exchanged MTU, 0 if the link is gone); `maxChunk = min(MTU − 5, 250)` — 3
bytes of ATT header, 2 of sequence number; 250 at MTU 255. The app requests MTU first (Android
asks 517, NimBLE answers 255); at MTU 23 the 118-byte BEGIN arrives as a long write, which
NimBLE reassembles (`NimBLECharacteristic.cpp:298–324`).

```cpp
// src/ble_ota.cpp — shape; the UUIDs and kOtaPublicKey are fixed, other names are yours
struct CtrlItem { uint16_t conn; uint8_t  len; uint8_t b[118]; };   // queue depth 4
struct DataItem { uint16_t conn; uint16_t len; uint8_t b[252]; };   // queue depth 34 (~8.7 KB)
static QueueHandle_t s_ctrlQ, s_dataQ;
static NimBLECharacteristic *s_ctrl;
static volatile uint16_t s_owner = BLE_HS_CONN_HANDLE_NONE;          // set by loop() at BEGIN
static volatile bool s_acceptData = false, s_dataDropped = false;

struct DataCb : NimBLECharacteristicCallbacks {     // CtrlCb: the same into s_ctrlQ, no filter
    void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *d) override {
        if (!s_acceptData || d->conn_handle != s_owner) return;      // stale or foreign frame
        NimBLEAttValue v = c->getValue();                            // <= max length (252)
        DataItem it = { d->conn_handle, (uint16_t)v.length(), {0} };
        memcpy(it.b, v.data(), it.len);
        if (xQueueSend(s_dataQ, &it, 0) != pdTRUE) s_dataDropped = true;   // never block; ERR 4
    }
};
void ble_ota_setup(NimBLEServer *srv) {   // setup(): after MIDI.begin() + security, before adv->start()
    s_ctrlQ = xQueueCreate(4, sizeof(CtrlItem));
    s_dataQ = xQueueCreate(34, sizeof(DataItem));
    NimBLEDevice::setMTU(255);
    NimBLEService *dis = srv->createService(NimBLEUUID((uint16_t)0x180A));
    const std::pair<uint16_t, std::string> kDis[] = {               // std::string, never const char*
        {0x2A29, "Steven Jin"}, {0x2A24, BLE_MIDI_DEVICE_NAME}, {0x2A26, fw_version_full()}};
    for (auto &c : kDis)
        dis->createCharacteristic(NimBLEUUID(c.first), NIMBLE_PROPERTY::READ)->setValue(c.second);
    dis->start();
    NimBLEService *svc = srv->createService("7D0A0001-3F5E-4B8C-9A2D-6E1F0C4B8A17");
    s_ctrl = svc->createCharacteristic("7D0A0002-3F5E-4B8C-9A2D-6E1F0C4B8A17",
                                       NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY, 118);
    s_ctrl->setCallbacks(new CtrlCb());
    svc->createCharacteristic("7D0A0003-3F5E-4B8C-9A2D-6E1F0C4B8A17", NIMBLE_PROPERTY::WRITE_NR, 252)
       ->setCallbacks(new DataCb());
    svc->start();
}
```

## 4. Threading: parse in `loop()`, never on the Bluetooth task

The rule of BLE_SETTINGS §2. `onWrite` runs on the NimBLE host task (core 0) and only copies the
frame into its queue with zero wait (queues are made in `setup()`, never in a callback).
Everything else — parsing, hashing, `Update`, flash, the signature, notifications, UI — runs in
`ble_ota_tick()`, called from `loop()` after the all-off service (`main.cpp:1520–1526`) and
before `MIDI.read()` (`main.cpp:1528`), so the safety ticks (`main.cpp:1484–1491`) come first.

- **Replies go to the writer only:** `om = ble_hs_mbuf_from_flat(buf, len); if (om) rc =
  ble_gattc_notify_custom(conn, s_ctrl->getHandle(), om);` (thread-safe host API; 1.4.3's
  `notify()` would reach every subscriber). Never pass a NULL `om`: NimBLE would then notify the
  characteristic's stored value — the last frame the app wrote (`ble_gattc.c:4174–4183`). No
  mbuf or `rc != 0` → keep the frame, retry next pass while that link exists. The app
  subscribes before BEGIN.
- **One pass, bounded work:** at most 4,096 payload bytes go to `Update.write()` per pass — at
  most one 4 KB sector program plus, at a 64 KB boundary, one block erase (`Updater.cpp:206–215`)
  — then the pass returns. Flash work stalls both cores (ESP-IDF disables the cache); a 64 KB
  erase typically takes 0.1–0.3 s, far below the 8 s task watchdog (`main.cpp:1304`), and no
  coil can be energized during a session (§7).
- **Keep calling `MIDI.read()` every pass.** BLE-MIDI blocks the host task on a full 64-byte
  queue (`xQueueSend(…, portMAX_DELAY)`, `BLEMIDI_ESP32_NimBLE.h:46–50`); undrained, one stray
  MIDI packet would stall every Data write. Notes are dropped after parsing (§7).
- **The owner's link** is checked every pass in PREPARING and RECEIVING:
  `ble_gap_conn_find(owner, nullptr) != 0` means it is gone → abandon (§6).

## 5. Frames

Little-endian. The first byte of every Control value is the opcode.

| App → piano (`Control`) | Bytes | Layout |
|---|---|---|
| `BEGIN` | 118 | `01` · size u32 · sha256[32] · sig[64] · version[16] · window u8 |
| `END` | 1 | `02` |
| `ABORT` | 1 | `03` |

BEGIN: `size` = image bytes; `sha256` = SHA-256 of the image; `sig` = Ed25519 signature of those
32 digest bytes (§8); `version` = the image's `FW_VERSION`, UTF-8, zero-padded, at least one
character from `0-9 A-Z a-z . + -`; `window` = Data frames per ACK, 1–32 (the app should use 16:
16 × 250 B ≈ one flash sector).

| Piano → app (`Control` notifications) | Bytes | Layout | Meaning |
|---|---|---|---|
| `READY` | 4 | `81` · maxChunk u16 · window u8 | safety done, slot open; `window` = BEGIN's (the piano may lower it; the app uses READY's) |
| `ACK` | 5 | `85` · bytesReceived u32 | after each full window and the final partial one: bytes hashed and handed to flash |
| `VERIFYING` | 1 | `82` | END accepted, checking |
| `OK` | 3 | `83` · restartInMs u16 | verified and activated; restarting in `restartInMs` (1500) |
| `ABORTED` | 1 | `84` | ABORT honoured (also the answer to ABORT while idle) |
| `ERR` | 2 | `E1` · code u8 | refused or failed; the session is over, the slot discarded |

**`Data`** (WRITE_NR): seq u16 · payload of 1 … maxChunk bytes. `seq` = the frame's index since
READY, modulo 65,536; the payloads concatenate to the image.

| ERR | Name | When |
|---|---|---|
| 1 | not quiet | coils not verified off within 1 s of the stop, or a coil energized < 500 ms before the check (§7) |
| 2 | too large | `size` > `esp_ota_get_next_update_partition(NULL)->size` (6,553,600) |
| 3 | already updating | a session is active (a second BEGIN from the owner), or the running image is not yet confirmed (PENDING_VERIFY, §9) |
| 4 | bad header or sequence gap | BEGIN not 118 B, size 0, window ∉ 1–32, bad version; Data seq ≠ expected, payload 0 or > maxChunk, bytes past `size`, Data queue overflow; END before `size` bytes; unknown opcode |
| 5 | hash mismatch | SHA-256 of the received bytes ≠ BEGIN.sha256 |
| 6 | bad signature | Ed25519 fails — at BEGIN (early, before anything stops) or after END |
| 7 | flash write failed | `Update.begin()` fails except on size; `Update.write()` short with `UPDATE_ERROR_WRITE/ERASE/READ`; `Update.end()` with `UPDATE_ERROR_READ` |
| 8 | image invalid | `UPDATE_ERROR_MAGIC_BYTE` (first byte ≠ 0xE9) or `UPDATE_ERROR_ACTIVATE` (`esp_ota_set_boot_partition()` refused the image) |
| 9 | timeout | no valid frame from the owner for 10 s in RECEIVING |
| 10 | refused by safety | crash-loop safe mode is active this boot (§7) |

**Worked example**, reproducible for tests: an image of 991,232 bytes (0x000F2000 → `00 20 0F 00`),
`byte[i] = (0xE9 + 7·i) & 0xFF`, version `2.1.0`, window 16, MTU 255, signed with the **RFC 8032
§7.1 TEST 1** key (secret `9d61b19d…1cae7f60`, public `d75a9801…f707511a`) — a published test
vector, so anyone can reproduce the signature and the piano and the app reject it. SHA-256
`6d872b9645d71995871a346d906e64c1b4ea4d92dd7f88cccea75f2792b212c6`; 3,965 frames = 247 full
windows, then 13 frames, the last carrying 232 bytes.

```
BEGIN, 118 B: opcode 0 · size 1-4 · sha256 5-36 · sig 37-100 · version 101-116 · window 117
  0: 01 00 20 0F 00 6D 87 2B 96 45 D7 19 95 87 1A 34
 16: 6D 90 6E 64 C1 B4 EA 4D 92 DD 7F 88 CC CE A7 5F
 32: 27 92 B2 12 C6 27 78 01 1C 44 97 C2 CC 29 C4 08
 48: 5B 46 68 99 0D 2E 58 F1 86 CB 93 5E B3 C6 04 41
 64: 26 F0 9A 79 93 BB 38 EC 4F 3C 63 E6 06 3B 81 92
 80: 88 74 BF 7C AD 29 2F F0 F4 F7 93 0C 93 41 8E 87
 96: B8 CA 2D E6 0A 32 2E 31 2E 30 00 00 00 00 00 00
112: 00 00 00 00 00 10
READY      81 FA 00 10                       maxChunk 250, window 16
Data 0     00 00 E9 F0 F7 FE 05 0C … B1 B8    252 B: seq 0, image[0..249]; Data 1 = 01 00 BF C6 …
ACK        85 A0 0F 00 00                    after seq 15: 4,000 B
ACK        85 60 13 0F 00                    after seq 3951 (window 247): 988,000 B
Data 3964  7C 0F 91 98 9F A6 AD B4 … DB E2    234 B: seq 3964, the last 232 image bytes
ACK        85 00 20 0F 00                    after the partial window: 991,232 B
END 02 · VERIFYING 82 · OK 83 DC 05 (1,500 ms) · ABORT 03 · ABORTED 84 · ERR 5 = E1 05 · ERR 6 = E1 06
```

## 6. Flow control and the state machine

The app sends `window` Data frames and waits for the ACK before sending more; the piano ACKs when
a window completes or `bytesReceived == size`; then the app sends END. Only the owner — the
connection whose BEGIN was accepted — is heard; frames from other connections get no reply.

| State | Entered on | Leaves on |
|---|---|---|
| IDLE | boot; every abort or error | a valid BEGIN → PREPARING |
| PREPARING | BEGIN passed checks 1–5 below | READY → RECEIVING; ERR 1/2/7; ABORT; owner gone |
| RECEIVING | READY sent | END with all bytes → VERIFYING; ABORT; ERR 4/7/8/9; owner gone |
| VERIFYING | VERIFYING sent | OK → REBOOTING; ERR 5/6/7/8 |
| REBOOTING | OK sent | `ESP.restart()` |

Every return to IDLE: `Update.abort()` (frees the buffer, erases nothing), clear the SHA context
and both queues, restore note input, LEDs and status line, log `[ota] <reason> — update
abandoned, running firmware unchanged`. The partial slot is harmless: `Update` holds back an
image's first 16 bytes until `end()` (`Updater.cpp:192–201`), so the slot starts 0xFF and cannot
boot, and `otadata` is untouched. **END is the point of no return:** from VERIFYING on, ABORT is
ignored and a lost link no longer aborts; after OK the piano restarts with or without the app.

**BEGIN**, in order — checks that touch no hardware come first, so a refused BEGIN never
interrupts music: (1) a session is active → ERR 3 to the owner (others: ignored); the running
image is PENDING_VERIFY → ERR 3. (2) Not 118 bytes, `size` 0, `window` ∉ 1–32, bad `version` →
ERR 4. (3) Crash safe mode → ERR 10. (4) `size` > slot → ERR 2. (5)
`crypto_sign_verify_detached(sig, sha256, 32, kOtaPublicKey) != 0` → ERR 6 — the early check,
so a wrong key costs a second, not a minute of transfer. (6) PREPARING and the safety sequence
of §7; failure → ERR 1. (7) `Update.begin(size, U_FLASH)` false: `UPDATE_ERROR_SIZE` → ERR 2,
else ERR 7. (8) SHA-256 started, expected seq 0, `maxChunk` from the MTU, READY → RECEIVING.

**Data** in RECEIVING: seq, length and remaining size (ERR 4); `mbedtls_sha256_update_ret()`;
`Update.write()` — short → ERR 8 on `UPDATE_ERROR_MAGIC_BYTE`, else ERR 7; `bytesReceived +=`;
ACK when due. **END:** `bytesReceived ≠ size` → ERR 4; else notify VERIFYING and verify on the
next pass, so the notification leaves first.

```
App                              NimBLE task (core 0)       loop() (core 1)
MTU 517→255 · priority HIGH · subscribe Control · MIDI CC64=0, CC123 ─► all-off path
… ≥ 500 ms …
Control BEGIN ────────────────► queue ─────────────────────► checks 3/4/10/2/6 → PREPARING: notes
                                                             blocked, LEDs dark, allKeysOff, pedal up,
                                                             verified OFF ≤ 1 s, quiet, Update.begin
◄────────────────────────────────────────────────────────── READY 81 FA 00 10
Data seq 0…15 ────────────────► queue ─────────────────────► SHA-256 + Update.write, ≤ 4 KB per pass
◄────────────────────────────────────────────────────────── ACK 85 A0 0F 00 00 (… 247 windows + 13 frames)
◄────────────────────────────────────────────────────────── ACK 85 00 20 0F 00
Control END ──────────────────► queue ─────────────────────► VERIFYING 82 · digest, Ed25519, Update.end
◄────────────────────────────────────────────────────────── OK 83 DC 05 · settings_save, otaPart/otaVer
link drops at 1.5 s ◄────────────────────────────────────── disconnect, ESP.restart()
                                 bootloader: app1 NEW → PENDING_VERIFY, boots it
reconnect ≤ 60 s, read 0x2A26 ► "2.1.0+a1b2c3d"             ≥ 30 s + a board + BLE → valid; "Updated to 2.1.0"
```

## 7. Safety

**Before READY, in this order** (the disconnect stop's all-off path, `main.cpp:1499–1506`):
1. Block every energize source (below); `flushEventQueue(); allKeysOff(); clearLedReactive();`
2. `pedal_release();`
3. Wait ≤ 1,000 ms for **verified OFF** on every coil. The OTA adds no release logic: the normal
   passes run `tickAutoRelease()` (retry every 10 ms, ALLCALL after 4 failing passes, forced bus
   recovery if that fails; `power_boards.cpp:895–957`) and `tickChipHealth()`, and each pass polls
   a new read-only `bool coilsVerifiedOff()` in `power_boards.cpp`: every board, **online or
   offline**, has `s_onAt == 0 && !s_needOff` on all 12 channels, and no key has `pendingFireMs`,
   `pendingReleaseMs`, `cushionOffMs` or `held`. It clears nothing (SAFETY rule 1); an offline
   board's believed-on coil keeps it false (rule 5). Still false at 1,000 ms → ERR 1.
4. **Quiet:** `millis() − lastEnergizeMs() < 500` → ERR 1. The new `lastEnergizeMs()` returns a
   stamp taken on every call of `armOnAt()` (`power_boards.cpp:89–92`), which every energize path
   passes (strike, re-strike, deferred fire, cushion, `fire`, `rawnote`; the synchronous sweep
   ends in `allKeysOff()`); the anchoring is unchanged. The app's stop must precede BEGIN by ≥ 500 ms.

**During a session** (PREPARING → REBOOTING, `ble_ota_session_active()`):
- **Notes are dropped** at the funnel, `note_input_enqueue()` (`main.cpp:307`): BLE-MIDI and
  touch keys. `MIDI.read()` keeps running (§4). **CC 120/121/123 still run the all-off path**
  (`main.cpp:367–369, 1520–1526`) and the transfer goes on.
- **Serial and NUS energize commands are refused** (`  refused — updating firmware`): `note` with
  velocity > 0, `rawnote`, `fire`, `sweep`, `pwm`, `ramp`, `testmin`, `testmax`, `pedaltest`.
  `off`, `broadcast`, `panic`, `status` and the settings commands work.
- **Pedal up:** `pedal_tick(ble_ota_session_active() ? 0 : g_sustainRaw)` (`main.cpp:1677`) —
  0 is UP in both pedal modes (`pedal.cpp:63–67`) and retries a failed lift.
- **LED strip dark:** `updateLeds()` pushes one black frame, then returns. Afterwards force a full
  frame in the current mode: STATIC caches its last frame (`main.cpp:458–464`).
- **Screen:** `lbl_status` = `Updating • 38 %` (integer, set when it changes; `"Updating "
  LV_SYMBOL_BULLET " %u %%"`), then `Updating • checking`, `Updating • restarting`, set in
  `loop()`'s `ui_lock` block; the session counts as activity for the idle dimmer (`main.cpp:1621–1629`).
- **The safety ticks keep their cadence** — `tickAutoRelease()`, `tickPendingFires()`,
  `tickRestrike()` at ~100 Hz, `tickChipHealth()` every 5 s, the watchdog fed every pass. The OTA
  code must yield after its bounded work (§4) so they do: no loops or waits inside a pass (the
  1 s verified-OFF wait spans passes).
- **Crash safe mode:** keep the count that `main.cpp:1316` discards —
  `g_crashSafeMode = (settings_boot_inc() >= 3);`, true for the whole boot in which safe mode
  forced Full Power (`settings.cpp:266–269`): BEGIN → ERR 10. A clean power-on resets the counter
  (`settings.cpp:255–260`), so the remedy is switching the piano off and on.
- **Disconnect or timeout:** abort (§6); the old slot is untouched, the coils are still verified
  off, normal operation resumes — the next note plays.
- **No bootloader window:** unlike a USB flash (~40 s in download mode, hence CLAUDE.md's
  "release the coils first"), OTA never enters download mode; the slot switch happens at
  `ESP.restart()` with the coils verified off, the PCA9685s hold that through the reset, and the
  new image first sends the triple ALLCALL FULL_OFF (`main.cpp:1326–1332`).

**SAFETY.md's checklist:** no coil state cleared (rule 1); the stop is `allKeysOff()`, which wipes
every deferred timer (2); no energize path added, `armOnAt()`'s anchoring kept (3, 4, 7, 8);
offline believed-on coils count as ON (5); after the safety ticks, capped work (6). Tests: §13.

## 8. Verification before anything boots

**Streaming SHA-256**, mbedTLS 2.x with the hardware engine (`#include "mbedtls/sha256.h"`):
`mbedtls_sha256_init`, `mbedtls_sha256_starts_ret(&ctx, 0)` at READY, `mbedtls_sha256_update_ret()`
on each payload before `Update.write()`, `mbedtls_sha256_finish_ret()` after END,
`mbedtls_sha256_free`. **After END, in this order** (`fail(n)` = `Update.abort()`, ERR n, IDLE):

```cpp
uint8_t digest[32];
mbedtls_sha256_finish_ret(&s_sha, digest);
if (memcmp(digest, s_hdr.sha256, 32) != 0) return fail(5);
if (crypto_sign_verify_detached(s_hdr.sig, digest, 32, kOtaPublicKey) != 0)  // <sodium.h>; call
    return fail(6);                                                           // sodium_init() once
esp_task_wdt_reset();
if (!Update.end(false)) {    // only now: refuses unless all `size` bytes were written, writes the
    uint8_t e = Update.getError();   // 16 held-back bytes, re-reads the magic (UPDATE_ERROR_READ),
    return fail(e == UPDATE_ERROR_ACTIVATE || e == UPDATE_ERROR_MAGIC_BYTE ? 8 : 7);
}                            // then esp_ota_set_boot_partition(): IDF validation + otadata switch
// OK {1500}; settings_save(); NVS "piano": otaPart = target slot label, otaVer = BEGIN.version;
// at 1,500 ms NimBLEDevice::getServer()->disconnect(owner); ~100 ms later ESP.restart().
```

(`Updater.cpp:249–267, 278–305`; `esp_ota_ops.h:175`; `Update.setMD5()` unused.) **The ESP-IDF
checks are not security.** The magic byte (`Updater.cpp:187`) and the image validation (header,
segments, checksum, appended SHA-256) prove integrity to anyone who can run esptool. With secure
boot off, the Ed25519 signature is the only check of who built the image; the boot slot switches
only after it passes over the digest of the bytes received.

**The public key**, `include/ota_pubkey.h` (the release script checks it against the PEM), from
`provenance/author_ed25519_public.pem`; the fingerprint `eab16a502f679465` is the first 16 hex
digits of the SHA-256 of these 32 bytes, as `PROVENANCE.md` states:

```c
// ~/.platformio/penv/bin/python -c 'from cryptography.hazmat.primitives.serialization import load_pem_public_key as L, Encoding as E, PublicFormat as F; r=L(open("provenance/author_ed25519_public.pem","rb").read()).public_bytes(E.Raw,F.Raw); print(", ".join("0x%02x" % b for b in r))'
static const uint8_t kOtaPublicKey[32] = {
    0x94, 0xa0, 0xfc, 0x32, 0x52, 0x03, 0xab, 0x51, 0xdc, 0x28, 0xfb, 0xff, 0xc6, 0xc9, 0xce, 0x0f,
    0x9c, 0xda, 0x8f, 0x37, 0x60, 0xdc, 0xc0, 0x07, 0xb5, 0x6a, 0x4b, 0x65, 0xf8, 0x8f, 0x03, 0x19,
};
```

**What the signature does not stop:** a genuine older release installs (no anti-rollback; the
app offers only newer ones); anyone in range can connect, as with BLE-MIDI today, and replaying a
published BEGIN can stop the music once — no worse than CC 123 on the open MIDI service.

## 9. Rollback and the self-test

- **Keep new images unconfirmed.** `verifyRollbackLater` is declared in no header and defined
  weak in C (`esp32-hal-misc.c:207–208`); a C++ definition is name-mangled and silently ignored.
  As the framework's RainMaker does (`libraries/RainMaker/src/RMaker.cpp:10`):
  `extern "C" bool verifyRollbackLater() { return true; }` — after a build,
  `xtensa-esp32s3-elf-nm .pio/build/lilygo-t4-s3/firmware.elf | grep -w verifyRollbackLater`
  must show `T`, not `W`.
- **States** (`esp_flash_partitions.h:50–55`; ESP-IDF v4.4 OTA docs, "App rollback"):
  `esp_ota_set_boot_partition()` marks the slot NEW; the bootloader turns NEW into PENDING_VERIFY
  and boots it; `esp_ota_mark_app_valid_cancel_rollback()` makes it VALID. A reset of any kind
  while PENDING_VERIFY — panic, task watchdog, brownout, power cut, `ESP.restart()` — makes the
  bootloader mark it ABORTED and boot the previous slot. A USB-flashed image is UNDEFINED
  (`boot_app0.bin`) and needs no confirmation.
- **The self-test**, while the running image is PENDING_VERIFY, evaluated every pass from
  `millis() ≥ 30000` — the point that clears the crash counter (`main.cpp:1709–1715`): (a) 30 s up
  (setup finished; no panic or task-watchdog reset), (b) at least one power board online
  (`powerBoards[i].connected`: the image drives the I²C trunk), (c) BLE up:
  `NimBLEDevice::getAdvertising()->isAdvertising() || appState.bleConnected`. All true →
  `esp_ota_mark_app_valid_cancel_rollback()`; on `ESP_OK` log `[ota] 2.1.0+… confirmed`, erase
  `otaPart`/`otaVer`, show **`Updated to 2.1.0`** (`FW_VERSION`) on the status line for 60 s.
  Otherwise keep checking — boards that appear later (hot-reconnect, `power_boards.cpp:707–718`)
  still confirm it. No deadline: an image that never passes stays pending until the next reset.
- **At boot**, after `logResetReason()`: the banner of §2, from
  `esp_ota_get_running_partition()->label` and `esp_ota_get_state_partition()`. If `otaPart` names
  another slot than the running one, the update did not take: log `[ota] update to 2.1.0 (app1)
  was not confirmed — running app0`, show `Update to 2.1.0 rolled back` for 60 s, erase both keys.
- **Crash-loop safe mode stays.** A new image that crashes before confirming never reaches it:
  the first crash rolls it back and the old image counts one crash (reset reason PANIC/WDT),
  cleared after 30 s. The restart after OK is `ESP_RST_SW`, which resets the counter. While the
  running image is PENDING_VERIFY, BEGIN gets ERR 3 — `Update` bypasses `esp_ota_begin()`, which
  refuses exactly that (`esp_ota_ops.h:73–75`).
- **Settings across a rollback:** a rolled-back image reads the NVS the newer one wrote. A
  release may add keys to namespace `piano`, never change an existing key's type or meaning.

## 10. Releases

`releases/latest.json` in this repository; the app reads
`https://raw.githubusercontent.com/stevenjin20090101-rgb/Steven-Jin-Player-Piano/main/releases/latest.json`
(the repository must stay public; it is).

| Field | Meaning (→ BEGIN field) |
|---|---|
| `version` | `FW_VERSION`, ≤ 16 bytes (→ version) |
| `build` | the 7-character git hash built; the piano then reports `version+build` |
| `binUrl` | `https://github.com/stevenjin20090101-rgb/Steven-Jin-Player-Piano/releases/download/fw-v<version>/firmware-<version>.bin` |
| `sizeBytes` | image size (→ size) |
| `sha256` | 64 lowercase hex (→ sha256) |
| `sig` | base64 of Ed25519(private key, the 32 raw SHA-256 bytes), 88 characters (→ sig) |
| `minAppVersionCode` | the lowest app versionCode that may install it |
| `notes` | shown by the app, ≤ 1,000 characters |
| `usbOnly` | true when the release changes the partition table or the bootloader (or anything else an app-slot write cannot carry): the app says "This update needs a USB flash" and never sends it |

Example — the §5 image and test key, for parser tests (`build`, `minAppVersionCode`, `notes`
illustrative):

```json
{
  "version": "2.1.0",
  "build": "a1b2c3d",
  "binUrl": "https://github.com/stevenjin20090101-rgb/Steven-Jin-Player-Piano/releases/download/fw-v2.1.0/firmware-2.1.0.bin",
  "sizeBytes": 991232,
  "sha256": "6d872b9645d71995871a346d906e64c1b4ea4d92dd7f88cccea75f2792b212c6",
  "sig": "J3gBHESXwswpxAhbRmiZDS5Y8YbLk16zxgRBJvCaeZO7OOxPPGPmBjuBkoh0v3ytKS/w9PeTDJNBjoe4yi3mCg==",
  "minAppVersionCode": 11,
  "notes": "Softer pianissimo; the pedal lifts on stop.",
  "usbOnly": false
}
```

Sign by hand (key path from `PIANO_SIGNING_KEY`, default `~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem`;
the key is never printed), then verify against the repository's public key — from `firmware/`:

```sh
~/.platformio/penv/bin/python -c 'import base64,hashlib,os,sys; from cryptography.hazmat.primitives.serialization import load_pem_private_key as L; k=L(open(os.path.expanduser(os.environ.get("PIANO_SIGNING_KEY","~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem")),"rb").read(),None); print(base64.b64encode(k.sign(hashlib.sha256(open(sys.argv[1],"rb").read()).digest())).decode())' firmware-2.1.0.bin
~/.platformio/penv/bin/python -c 'import base64,hashlib,sys; from cryptography.hazmat.primitives.serialization import load_pem_public_key as L; d=hashlib.sha256(open(sys.argv[1],"rb").read()).digest(); L(open("provenance/author_ed25519_public.pem","rb").read()).verify(base64.b64decode(sys.argv[2]),d); print("signature OK", d.hex())' firmware-2.1.0.bin '<sig>'
```

**GitHub:** tag `fw-v<version>` on `stevenjin20090101-rgb/Steven-Jin-Player-Piano`, asset
`firmware-<version>.bin`, published by **`tools/publish-firmware.sh "<notes>" [--usb-only]
[--dry-run]`**, whose header lists every check and step. Two details matter: `gh`'s `file#text`
is only a display label, so the script copies `firmware.bin` to `firmware-<version>.bin` before
`gh release create … --target <commit>`; and it downloads the asset back through `binUrl` before
it writes, re-signs (provenance) and pushes `releases/latest.json`, so the app never sees a
manifest without its binary. `PIANO_MIN_APP_VERSION_CODE` is required. **Untested against the
real repository** (run end to end in a sandbox with a throwaway key and stand-ins for `pio`, `gh`
and GitHub): `--dry-run` first; 2.0.0 is its first real run.

## 11. What the app does (the firmware may rely on it)

From the plan (M21) and `android/BUILD_SPEC.md`: before BEGIN the link is up, playback is stopped
with the full stop sequence (CC64 = 0 then CC123, one packet), no channel or schedule runs, the
manifest's signature is verified and `usbOnly` is false. **Needed from the app:** ≥ 500 ms
between the stop sequence and BEGIN, or ERR 1 (retryable). During the transfer it sends nothing
else over BLE-MIDI, requests MTU 517 and `CONNECTION_PRIORITY_HIGH`, subscribes to Control,
honours READY's `maxChunk` and `window` and waits for every ACK; Cancel sends ABORT (until END).
After OK it expects the link to drop and return within 60 s, reads `0x2A26`, and shows "Updated
to 2.1.0" or "…reports 2.0.0 — it rolled back"; any ERR: "The update didn't finish. The piano
kept its old firmware."

## 12. Feature detection on the app side

The app treats the presence of service `7D0A0001-…` as "this piano can be updated" and `0x2A26`
as its version. Older firmware has neither: the app shows "Unknown — this firmware has no
version" and asks for the one USB flash of 2.0.0. Every build from 2.0.0 on exposes both,
whatever the hardware state. An incompatible change to §5 gets a new service UUID; compatible
additions get new opcodes.

## 13. Acceptance

**Test client** `tools/ota-test/ota_client.py` (Python 3, `bleak`, macOS): find "Steven Piano"
(`BleakScanner.find_device_by_name`), `start_notify(Control)` into an `asyncio.Queue`, write
BEGIN with `response=True`, then per window `window` × `write_gatt_char(Data, seq u16 LE + chunk,
response=False)` and wait ≤ 10 s for the ACK; END; wait ≤ 30 s for OK or ERR; print every
notification in hex. Flags: a tampered image byte, sha or sig; ABORT or disconnect after N
windows; a stall; raw frames. Sign test images with §10's one-liner and never publish them; if
macOS shows a stale service list after a USB flash, toggle its Bluetooth.

| # | Run | Expect |
|---|---|---|
| 1 | one image byte flipped, original sha/sig | READY, ACKs, `82`, `E1 05`; serial: hash mismatch; the old version after a reboot |
| 2 | one bit of `sig` flipped | `E1 06` right after BEGIN, no READY; a song playing from another central keeps playing |
| 3 | ABORT after 20 windows | `84`; the next note plays; LEDs and status line back |
| 4 | disconnect after 20 windows | serial: update abandoned; reconnect: `0x2A26` unchanged; a new update succeeds |
| 5 | stop sending for 11 s after 5 windows | `E1 09` |
| 6 | BEGIN twice; Data seq 5 first; `size` 7,000,000 | `E1 03`; `E1 04`; `E1 02` |
| 7 | a good update | `83 DC 05`, link drops, back ≤ 60 s, `0x2A26` = new version; serial `running app1  state PENDING_VERIFY`, ~30 s later `confirmed`; screen `Updated to …` |
| 8 | a good update, then cut the controller's power 10 s after the new image boots | the old version boots (the bootloader is silent — judge by the banner); `Update to … rolled back` |
| 9 | BEGIN within 30 s of run 7's restart | `E1 03` |

ERR 10 needs three crash resets in a row within 30 s of boot: test it once with a temporary
debug build, or skip it. **Bench checklist (SAFETY.md):**
- A dense song over BLE-MIDI from the phone, BEGIN from the Mac: the piano goes silent, `E1 01`,
  `status` shows every coil released, `i2cFails` flat. `fire 3 0 2000` on serial, BEGIN within
  0.5 s: the coil is released (verified), then `E1 01`.
- During a transfer: NoteOns from a second central and touch-key taps sound nothing; `fire` is
  refused; CC 123 logs `[midi] all-notes-off serviced` and the transfer continues, as with `panic`;
  `[hb]` every 30 s, no task-watchdog reset, `minHeap` steady; the session's longest `loop()`
  pass (log `[ota] longest pass N ms`) well under 1 s.
- After every error or abort: a song plays normally, the strip is back in its mode (STATIC too),
  the status line shows the BLE state and version again.
- `make check` in `tools/dispatch-sim` with a new scenario (strike; `allKeysOff()` with failing OFF
  writes; `coilsVerifiedOff()` true only after a verified OFF; `lastEnergizeMs()` = the strike),
  a full `sweep`, a dense song watching `status` → `i2cFails`, and run 8.

## 14. Things found while reading, to fix in the same change

- `testmax` behaves like `testmin`: `bool wantMax = (line[4] == 'a');` (`main.cpp:1041`) — index
  4 is `m` in both words; test `line[5]`.
- `pedalon 0` never lifts the servo: the handler clears `g_pedalEnabled`, then calls
  `pedal_release()` (`main.cpp:1135–1136`), which returns at once when the pedal is disabled
  (`pedal.cpp:107`). Release first, then clear.
- `-DLV_CONF_INCLUDE_SIMPLEjj` (`platformio.ini:27`) → `-DLV_CONF_INCLUDE_SIMPLE`. Harmless today
  only because LVGL 8.4 defines it itself when `lv_conf.h` is on the include path
  (`lvgl/src/lv_conf_internal.h:22–26`; `-I .`, `platformio.ini:30`).
- `keyforce`: `help` and the usage line say 0.0..2.0 (`main.cpp:588, 850`; the comment at
  `power_boards.h:76` too), the handlers accept 0..4 (`main.cpp:853, 864`). Say 0.0..4.0.
- `settings.h:16–18` says settings save on BLE disconnect; they deliberately do not
  (`main.cpp:1496–1498`) — fix the comment. `settings.h:37` also credits `settings_load()` with
  forcing Full Power in safe mode; it is `settings_boot_inc()`.

## 15. Decisions on the open questions (2026-09-27, app side)

1. **Repository name.** Everything uses the current name `stevenjin20090101-rgb/Steven-Jin-Player-Piano`
   — the manifest URL, `binUrl`, the release tag and `REPO` in the script — because the app's
   allow-list checks exact path prefixes and will not follow a redirect to a different path. The
   local checkout's `origin` may stay on the old name (git follows the redirect) or be moved with
   `git remote set-url origin https://github.com/stevenjin20090101-rgb/Steven-Jin-Player-Piano.git`.
2. **`minAppVersionCode` = 11** for 2.0.0 (updated 2026-09-28: the runs landed in a different order, and the
   app release that carries the Firmware page is **1.6 = build 11**; 1.5 = 9, 1.5.1 = 10; later builds count up from 12).
3. **Self-test condition (b).** Recommendation, the implementer decides: require at least one power
   board only when a persisted `boardsSeen` flag was set by an earlier boot on this piano. A bench
   board that has never seen the trunk then still confirms, while the installed piano keeps the
   stronger check.
4. **`!ota` fact adopted.** `dump` reports `!ota=none|pending|confirmed` (added to BLE_SETTINGS.md
   next to `!fw`): `none` for an image flashed over USB, `pending` until the self-test passes,
   `confirmed` after it. After a reconnect the app reads `0x2A26` and `!ota`; on `pending` it shows
   "Updated to 2.1.0 · confirming…" and re-reads after 30 s; if the old version comes back it says
   the update rolled back.
