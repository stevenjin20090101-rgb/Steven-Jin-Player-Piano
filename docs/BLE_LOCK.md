# Locking the piano's Bluetooth to the school tablet — design and app contract

**Status: built 2026-10-12, not published (app 1.25 first).** Designed the same day; Steven's choices: automatic
first claim (no visit), and a guest switch. Code: `src/ble_lock.cpp` (state, replies), `src/lock_crypto.cpp`
(claim and auth, proven on the Mac by `tools/lock-check`), the gates in `src/ble_console.cpp` and `src/main.cpp`,
ERR 11/12 and the version tag in `src/ota_session.cpp` (`tools/ota-sim`). "As built" below says where the code
chose a reading the design left open. Requested by the app session after Steven's audit (2026-10-10):
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
  Opened only by someone with access: the piano's screen (**press and hold any lighting button, then tap "Let a new tablet pair"**), the USB
  console (`lock open`), or an already-trusted device (`lock pairwindow`).
- Trust ends with the connection. Every new connection authenticates again (fresh nonce).
- **Guest play** (Steven, 2026-10-12): a trusted device (or the USB console) can let *anyone* play notes for
  a while — `lock guest <minutes>` (1–240; `lock guest 0` ends it). During guest play an untrusted connection
  may send notes and the pedal (as before the lock), but still cannot change settings or update. It ends at
  the time, at `lock guest 0`, or at a restart; it survives connections changing (the tablet lets go, the iPad
  connects). The piano's screen shows it.

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
!guest=<seconds left>           (0 = guest play off)
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
`lock forget` (remove every key → `open`), `lock guest <minutes>` (guest play; 0 ends it). USB only:
`lock open` (window), `lock reset` (= forget). Also `lock status` (one line, both), and on USB
`lock selftest` (the device's X25519 / SHA-256 / HMAC against a fixed vector, see below).

### Replies (pinned with the app, 2026-10-12)

Every reply line starts with two spaces except `!claim=`; the app matches these exactly.

| Line sent | When | Reply |
|---|---|---|
| `claim <hex>` | piano `claimed` and no pairing window | `  claim refused — not open (open pairing on the piano's screen)` |
| | already 4 keys (a 5th device; a key is never replaced silently) | `  claim refused — already 4 devices (lock forget first)` |
| | key not 64 hex, or an all-zero shared secret | `  claim refused — bad key` |
| | accepted | `!claim=<piano public key, 64 hex>` then `  claimed` |
| `auth <hex>` | piano `open` (nothing checked: open trusts everyone) | `  auth ok — piano is open` |
| | this connection is already trusted (no re-check; trust kept) | `  auth ok — already trusted` |
| | the HMAC matches a stored key | `  auth ok` |
| | it does not (a new nonce follows) | `  auth refused` |
| | less than 1 s after this connection's last attempt (not checked, not counted, nonce kept) | `  auth refused — wait 1 s` |
| | the 5th failure on this connection | `  auth refused`, then the piano disconnects it |
| `lock pairwindow` | trusted or USB (`lock open` on USB) | `  pairing open for 5 min` |
| `lock forget` | trusted or USB (`lock reset` on USB) | `  lock forgotten — piano is open` |
| `lock guest N` | N = 1..240 | `  guest play on for N min` |
| `lock guest 0` | | `  guest play off` |
| `lock guest …` | anything else | `  lock guest out of range (0..240)` |
| any `lock …` | from an untrusted connection (claimed piano) | `  refused — not authorised (send auth first)` |

As built, a few more lines the app does not need to match (each contains `refused` or `usage:`, so the app
already reads them as refusals): `  claim refused — wait 1 s` (a claim's X25519 work runs on the loop task,
so the piano does at most one a second, from anyone — SAFETY.md rule 6; a small-order key is refused as
`bad key` before any of it), `  claim refused — could not save (try again)` and
`  lock forget refused — could not save (try again)` (the NVS write failed; nothing changed),
`  refused — connection closed` (a line whose connection closed while it waited; it reaches no one),
`  refused over Bluetooth — use the USB console` (`lock open`, `lock reset`, `lock selftest` over Bluetooth),
and the `usage:` line for an unknown `lock` word.

**No unsolicited lines.** `!lock` and `!guest` change at once (the next `dump` / `get` shows them), but the
piano sends nothing when its screen opens or a window closes, or when guest play ends — the app learns from its
next `dump`.

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
   On the piano's screen press and hold a lighting button, tap Let a new tablet pair, then reconnect."
5. **Never block the stop:** the stop sequence and the crash-handler silence go out whether or not the
   connection is trusted (the piano honours them untrusted).
6. **Update errors:** ERR 11 "not authorised", ERR 12 "not newer than the piano's firmware".
7. **Firmware page:** show the lock state; "Let another device pair (5 min)" → `lock pairwindow`;
   "Unlock the piano" → `lock forget` (with a confirmation); **"Let anyone play for 1 hour"** (a switch with
   the time left from `!guest`) → `lock guest 60` / `lock guest 0`.
8. **Models:** `EmulatedConsole` / `OtaPiano` learn the facts, `claim`, `auth`, ERR 11/12.

## Rollout (no visit needed in the normal case)

1. App 1.25 published; the tablet updates itself (device owner).
2. Firmware 2.1.0 published; installed from the tablet's Firmware page like 2.0.1.
3. The piano restarts **open** (no key yet — nothing is locked, nothing can lock it out). The tablet
   reconnects within a minute and claims it. From then on only the tablet plays.
4. A visit is needed only if: someone else claimed first, the tablet is replaced or reset, or another
   device (Steven's phone) should be added — someone presses and holds a lighting button on the piano's screen and taps **Let a new tablet pair** on the
   piano's screen, then the device connects within 5 minutes.

## As built (2026-10-12): the readings the design left open

Each is the safer of the possible readings; none changes a fact name, command word, reply text or label.

- **Connections are (handle, generation).** A GAP event listener on the NimBLE host task numbers every new
  connection (it only counts and numbers — no parsing, hashing or NVS there), and every Nordic UART write is
  tagged with its connection's number. A handle the stack reuses for a new central therefore never inherits the
  old connection's trust, nonce or queued lines; a line whose writer has left still runs as before on an open
  piano, but its reply goes to no one, and a `claim` or `auth` from it is refused.
- **Trust and nonces are per connection** (up to 3 at once, NimBLE's limit), so another central asking for
  `dump` cannot replace the tablet's nonce under it. A nonce is made on the connection's first `dump` / `get
  nonce`, answers one `auth` attempt (it is renewed after every attempt, a success too) and ends with the link.
- **One claim per window.** A successful claim closes the pairing window, so each window admits exactly one
  device (open it again for another). The first claim of an open piano also ends guest play started while it
  was open (anyone could have switched it on then).
- **On an open piano** everyone is trusted (`!auth=1`), `lock pairwindow` / `forget` / `guest` work for anyone
  and nothing else changes from 2.0.x.
- **Notes:** BLE-MIDI bytes carry no connection, so on a claimed piano notes and pedal-down play only while
  exactly one central is connected and it is trusted, or during guest play. Program change and pitch bend are
  gated like a NoteOn (they do nothing yet).
- **`!auth` / `!nonce` over USB:** `!auth=1` (USB is never locked) and `!nonce=` 32 zeros.
- **An unreadable key store locks** (claimed, no usable key) instead of opening: a claim in a pairing window
  writes a fresh store, or `lock reset` over USB clears it. `reset` (the USB settings wipe, which clears the
  whole NVS namespace) writes the keys back: the lock is not a setting.
- **The screen button** "Let a new tablet pair" is in the settings popup that opens when you hold one of the
  three lighting-mode buttons; it closes the popup so the status line shows `Pairing open • 4:59`. The status
  line shows the pairing window (amber) or guest play (blue) after an update's and the bench signal test's
  texts. Guest play does not keep the screen awake (it can run for hours; the AMOLED must dim).
- **`lock selftest`** (USB) runs the claim derivation and the auth HMAC on RFC 7748 §6.1's keys (the piano is
  Bob, the app Alice) with nonce `00 01 … 0f` and compares with the values `tools/lock-check/check.py` computes
  on the app's side: K `44ce429a…0ee6dab9`, auth `e25c89e2…2afa2036`.
- **ERR 12 rules:** versions compare as MAJOR.MINOR.PATCH numbers, a `-…` or `+…` suffix ignored, so equal
  (including `2.1.0-rc.1` against `2.1.0`) is not newer; anything that does not parse is not newer. In the
  image, the first well-formed tag (1..16 characters of `0-9 A-Z a-z . + -`, NUL-ended) counts. The firmware
  holds the 6 marker bytes exactly once — the tag itself (the scanner stores them shifted by one).

## Residual risks, stated

- **The first claim** is first-come: in the minute between 2.1.0 starting and the tablet reconnecting, a
  stranger in range with custom software could claim first. The tablet would then show "locked to another
  device" and the fix is the screen button.
- **No link encryption:** an attacker with specialised radio hardware in range could, in principle, inject
  packets into the tablet's already-trusted connection. Bonding would close that, at the cost above.
- **Anyone in range can still stop the music** (release-only commands and all-off are open on purpose) and
  read the settings and diagnostics.
- **The iPad** (GarageBand and other MIDI apps) cannot authenticate: once claimed it plays only during guest
  play, which a trusted device switches on.
