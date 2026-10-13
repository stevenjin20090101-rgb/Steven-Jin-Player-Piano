# Locking the piano's Bluetooth to the school tablet — design and app contract

**Status: design, 2026-10-12 — not built yet.** Requested by the app session after Steven's audit (2026-10-10):
"Lock the piano's Bluetooth" — only the school tablet may play, change settings or update; updates never go
backwards. Firmware **2.1.0**. The app side ships first (app **1.25**); the firmware is published only after
the tablet runs it.

## Why an app-level lock, not Bluetooth bonding

Bonding would need an OS pairing dialog on the tablet (nobody at the school to tap it), brings back the
"forget this device" failures this install had with stale bonds, and Android 16 reports a lost bond as
`ACTION_KEY_MISSING` instead of re-pairing. The lock lives in the firmware and the app instead: no OS dialog,
no bond store, nothing the OS can lose. The link stays unencrypted as today (no change for BLE-MIDI); what is
locked is what an unproven connection may do.

## Model

- The piano keeps **up to 4 device keys** (32 bytes each, NVS namespace `piano`, key `lockKeys`; never
  wiped by an update).
- **`open`**: no key stored — the piano behaves exactly as 2.0.x. The first device to `claim` adds the first key.
- **`claimed`**: at least one key. A connection is **untrusted** until it proves a key with `auth`.
- **`pairing`**: claimed, and a 5-minute window is open in which another device may `claim` (adds a key).
  Opened only by someone with access: the piano's screen (**Settings › Let a new tablet pair**), the USB
  console (`lock open`), or an already-trusted device (`lock pairwindow`).
- Trust ends with the connection. Every new connection authenticates again (fresh nonce).

## What an untrusted connection may do (claimed piano)

| Path | Allowed | Refused |
|---|---|---|
| BLE-MIDI | NoteOff; CC 120/121/123 (all-off); CC64 below 64 (pedal up) | NoteOn, CC64 ≥ 64, everything else (dropped silently) |
| Console | `dump` `get` `status` `help` `off` `broadcast` `auth` (and `claim` while `open`/`pairing`) | everything else: `  refused — not authorised (send auth first)` |
| Update | — | BEGIN → **ERR 11 not authorised** |

Releasing is never locked: the stop sequence (CC64 = 0, CC123), `off`, the disconnect all-off and the hold
watchdog work for everyone, exactly as in `SAFETY.md`. `panic` needs a trusted connection (it forces Full
Power), and works on USB as always. The USB console is never locked (physical access).

## Protocol (console lines, ≤ 79 characters, as today)

New facts in `dump` / `get`, after `!trips`:

```
!lockproto=1
!lock=open|claimed|pairing
!auth=0|1                       (this connection has proved a key)
!nonce=<32 hex>                 (16 random bytes, new for every connection and after every failed auth)
```

**Claim** (only while `open` or `pairing`):

1. App: a fresh X25519 key pair; sends `claim <app public key, 64 lowercase hex>`.
2. Piano: a fresh X25519 key pair; `shared = X25519(piano_secret, app_public)` (all-zero result → refused);
   `K = SHA-256("steven-piano-lock-v1" ‖ shared ‖ app_public ‖ piano_public)`; stores K; replies
   `!claim=<piano public key, 64 hex>` then `  claimed`. This connection is trusted from here.
3. App: the same `K` from its side; stores it in the Android Keystore-wrapped secret store, keyed by the
   piano's address. A passive listener sees two public keys and cannot compute K.

**Authenticate** (every connection to a claimed piano):

1. App reads `!nonce` (from the `dump` it already sends on connect).
2. App sends `auth <HMAC-SHA256(K, "steven-piano-auth-v1" ‖ nonce bytes), 64 hex>`.
3. Piano tries each stored key (constant-time compare): `  auth ok`, or `  auth refused` and a new nonce.
   At most one attempt a second; five failures on one connection → the piano disconnects it.

**Management** (trusted connection or USB): `lock pairwindow` (5-minute window for another device),
`lock forget` (remove every key → `open`). USB only: `lock open` (window), `lock reset` (= forget).

## Updates never go backwards

BEGIN's `version` is not covered by the signature, so it cannot be trusted alone. From 2.1.0 every image
carries a version tag inside the signed bytes (`SJPV1:<FW_VERSION>`). The piano reads it from the stream as
the image arrives and, after the hash and signature check, refuses an image whose tag is missing or not
newer than the running `FW_VERSION`: **ERR 12 not newer**. BEGIN is also refused early with ERR 12 when its
own `version` is not newer (an honest client finds out in a second). Releases before 2.1.0 have no tag, so
nothing can take the piano back to 2.0.x over Bluetooth (USB still can, at the bench).

## What the app must do (1.25)

1. **Detect:** no `!lockproto` in `dump` → firmware 2.0.x, behave as today.
2. **`!lock=open` or `pairing`, no key for this piano:** claim (above), store K, carry on.
3. **`!lock=claimed`, key stored:** `auth` before any MIDI or setting write; hold the player until `auth ok`
   (3 s timeout → "The piano didn't accept this tablet"). If the piano holds several keys the app needs no
   slot number.
4. **`!lock=claimed`, no key (or `auth refused`):** read-only. Say: "This piano is locked to another device.
   On the piano's screen open Settings › Let a new tablet pair, then reconnect."
5. **Never block the stop:** the stop sequence and the crash-handler silence go out whether or not the
   connection is trusted (the piano honours them untrusted).
6. **Update errors:** ERR 11 "not authorised", ERR 12 "not newer than the piano's firmware".
7. **Firmware page:** show the lock state; "Let another device pair (5 min)" → `lock pairwindow`;
   "Unlock the piano" → `lock forget` (with a confirmation).
8. **Models:** `EmulatedConsole` / `OtaPiano` learn the facts, `claim`, `auth`, ERR 11/12.

## Rollout (no visit needed in the normal case)

1. App 1.25 published; the tablet updates itself (device owner).
2. Firmware 2.1.0 published; installed from the tablet's Firmware page like 2.0.1.
3. The piano restarts **open** (no key yet — nothing is locked, nothing can lock it out). The tablet
   reconnects within a minute and claims it. From then on only the tablet plays.
4. A visit is needed only if: someone else claimed first, the tablet is replaced or reset, or another
   device (Steven's phone) should be added — someone taps **Settings › Let a new tablet pair** on the
   piano's screen, then the device connects within 5 minutes.

## Residual risks, stated

- **The first claim** is first-come: in the minute between 2.1.0 starting and the tablet reconnecting, a
  stranger in range with custom software could claim first. The tablet would then show "locked to another
  device" and the fix is the screen button.
- **No link encryption:** an attacker with specialised radio hardware in range could, in principle, inject
  packets into the tablet's already-trusted connection. Bonding would close that, at the cost above.
- **Anyone in range can still stop the music** (release-only commands and all-off are open on purpose) and
  read the settings and diagnostics.
- **The iPad** (GarageBand and other MIDI apps) cannot authenticate: once claimed, it can no longer play.
