# ota-sim — host-side test for the Bluetooth update session

Compiles the real [`src/ota_session.cpp`](../../src/ota_session.cpp) on your
Mac/Linux box with the device build's language settings (`-std=gnu++11
-fno-rtti`, `-Wall -Wextra`) against a fake `ota::Port`, and plays the app's
side of [`docs/BLE_OTA.md`](../../docs/BLE_OTA.md) §5–§8. No hardware, no
PlatformIO, no crypto library.

The fake port has a fake clock (one `tick()` = one loop pass = 1 ms), links
that can drop, an MTU per link, scripted failures (coils that never verify
off, a recent energize, a pending image, safe mode, a refused signature, each
flash call failing with 7 or 8, notifications refused for a while), and it
records every notification and port call. The SHA-256 is written in
`sim.cpp` and checked against the FIPS 180-4 vectors; the "signature" check
accepts §5's real Ed25519 signature for §5's digest and a digest-bound fake
for every other image.

```bash
cd firmware/tools/ota-sim
make check                          # every scenario; non-zero exit on any failure
./sim worked_example err5_hash      # only these
SIM_VERBOSE=1 ./sim err9_timeout    # also the session's log lines and notifications
```

Scenarios (`sim.cpp`, one OK/FAIL line each):

- `worked_example` — §5 byte for byte: BEGIN's 118 bytes, `81 FA 00 10`, the
  Data dumps, 3,965 frames (the last 232 bytes), ACK `85 A0 0F 00 00` after
  seq 15 and `85 60 13 0F 00` after seq 3951, `85 00 20 0F 00`, the image's
  SHA-256 `6d872b96…12c6`, `82`, `83 DC 05` on the next pass, `restart()`
  1,500 ms later; update mode entered once, never left; every whole percent
  reported once.
- One per row of §5's error table: `err1_not_verified`, `err1_not_quiet`,
  `err2_too_large`, `err2_flash_begin`, `err3_pending_image`,
  `err3_second_begin_prep|recv`, `err4_*` (BEGIN length, size 0, window,
  version; seq gap, short Data, payload over maxChunk, past size, queue
  overflow, early END, unknown opcode, empty frame), `err5_hash`,
  `err6_begin` (update mode never entered), `err6_after_end`,
  `err7_flash_begin|write|end`, `err8_flash_begin|write|end`, `err9_timeout`,
  `err10_safe_mode`, plus `begin_check_order`. Each asserts the exact
  notifications, the final state, and the `enterUpdateMode` /
  `leaveUpdateMode` / `flashAbort` counts.
- `abort_idle|preparing|receiving|after_end`,
  `owner_gone_preparing|receiving|after_end` (after END it still verifies and
  restarts), `foreign_conn`, `ignored_frames`, `second_session`.
- Notifications: `notify_retry` (refused for a while: nothing lost, order
  kept, the check after END still runs on the next pass), `outbox_full`
  (4 slots; the oldest go), `outbox_per_link` (a stuck link holds up only
  its own).
- Sizes: `mtu23` (maxChunk 18), `mtu517` (still 250), `mtu_zero`, `wrap`
  (65,592 frames: the 16-bit sequence number wraps), `window1`, `window32`,
  `partial_last_window`; and `sha256_selftest`.

Every scenario also checks the invariants: never an empty notification;
update mode entered and left in pairs, coils polled only inside it; READY
only on a pass whose `coilsVerifiedOff()` said true and the last energize is
≥ 500 ms old; flash and hash calls in order; each return to idle exactly one
`flashAbort()`, one `leaveUpdateMode()` and one "update abandoned" log line;
at most one activation and one restart.

Limits: the port is a model. Real NimBLE timing, the glue's queues and their
per-pass limits, `Update`, mbedTLS and libsodium are `src/ble_ota.cpp`'s, and
are proved on the bench (BLE_OTA.md §13).
