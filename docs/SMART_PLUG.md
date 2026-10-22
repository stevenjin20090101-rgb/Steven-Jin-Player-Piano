# The 24 V rail on a Bluetooth smart plug — the tablet as the dead-man's switch

**Status: design, 2026-10-21 — app side for the app session (Steven Piano 1.26 or later), firmware side
in 2.1.0 (built the same day, unpublished).** Steven's decision after the stuck-coil incident: a Bluetooth
smart plug under the tablet's control bounds every "hot while idle" fault *now*, before the cut-off relay
board of `SAFETY_CUTOFF.md` exists. The relay stays the proper fix; this is the same rule with the tablet
holding the rail on instead of the controller.

## The rule

> The 24 V solenoid supply is plugged into a smart plug that switches itself **off 30 s after the last
> time the tablet told it to stay on**. The tablet tells it only while something should be playing *and*
> the piano is answering with every board online. Everything else — tablet asleep, app dead, Bluetooth
> gone, controller dark or rebooting, trunk broken — leaves the plug to its timer.

## What it covers, and what it does not

| Situation | Today | With the plug |
|---|---|---|
| Piano idle (nothing playing) | 24 V on, coils as the last write left them | 24 V off — the normal state; nothing can be hot while idle (the incident) |
| Controller unpowered or dark, a coil held | stays until someone pulls the plug | the piano stops answering → no re-arm → 24 V off within 40 s |
| Controller crashed or hung mid-song | stays until the reboot's all-off | same, within 40 s |
| Trunk or isolator dead, a coil believed on | OFF retries fail for ever | `!boards` shows a board MISSING while the rail is up → no re-arm → off within 40 s |
| App crashed, tablet died, Bluetooth off | nothing notices | the timer: off within 30 s |
| Boot, before the first all-off | PCA9685s drive whatever they held | the rail is off at boot (`initial_state=off`); it comes up only for playing |
| Firmware update | coils verified off, energize inhibited | same; the rail is kept up through the update so the new image can confirm itself (below) |
| A channel stuck on **while a song plays** and everything answers | the hold watchdog, if the firmware can see it | **not covered** — bounded by the song's end + 30 s. The relay board does not cover it either; the per-board current sense of `RS485_TRUNK.md` will. Keep `hold` at 2000. |
| The plug's relay welds, or someone moves the PSU to a wall outlet | — | back to today, never worse (the app warns when `output` stays on after a cut) |

"Within 40 s": the app re-arms every 10 s for 30 s, and only after the piano answered; the last good
re-arm is at most 10 s before the fault, so 24 V stays on at most 10 + 30 s after it.

## Hardware

- **Shelly Plug US Gen4** (the UL Wi-Fi / Bluetooth / Zigbee / Matter plug from Steven's listing). Leave it in
  its Wi-Fi/Bluetooth mode, never Zigbee mode. It is driven over Bluetooth only: it goes on a network **once**,
  for the setup below (firmware update, provisioning), then forgets it — never the school Wi-Fi, no cloud — so
  nothing can change its firmware or settings under us.
- **Only the 24 V PSU goes on the plug.** The controller's own supply and the LED strip's 5 V supply stay on
  another outlet: the piano must keep its screen, Bluetooth, settings and console while the rail is off —
  that is the whole point. Label the plug: *PIANO 24 V — must stay on the smart plug*.
- The PSU is a 24 V / 40 A switch-mode supply; its inrush is switched by the plug's relay at every rail-up,
  about once per piece played: at thirty pieces a day that is some 6,000 cycles a year, against a plug relay
  good for tens of thousands of cycles at this inrush — years. A welded relay fails to today's state, which the
  app detects (`Switch.GetStatus` → `output` still true after a cut). If the PSU has no soft-start, an NTC
  inrush limiter in its mains lead is cheap insurance (吟晚风 to check the PSU's datasheet).
- Pairing (the plug refusing unpaired phones) needs Shelly firmware **2.0.0 or later**
  (`BLE.StartPairing`). If the plug ships older, update it once over Wi-Fi with the Shelly app, then forget
  the network. On older firmware the plug still works, but anyone in range could switch the rail — harmless
  for safety (on = the controller holds every coil off; off = silence), not acceptable long-term.

## One-time setup (Steven: the Shelly app once, then the app's "Pair the power plug" screen)

RPC over Bluetooth is the plug's JSON-RPC carried over a GATT service — the Mongoose OS RPC service, verified
by the app session against Shelly's and Mongoose OS's docs: service `5f6d4f53-5f52-5043-5f53-56435f49445f`;
data `…-5f64-6174615f5f5f`; TX control `…-5f74-785f63746c5f`; RX control `…-5f72-785f63746c5f`. The client
writes the frame length as 4 bytes big-endian to TX control (which drops any half-written frame: one frame in
flight), writes the JSON frame to data in chunks, reads the reply's length from RX control (read/notify), then
reads data until it has it all.

**Provisioning states** (`Shelly.GetDeviceInfo` → `provision`, firmware ≥ 1.7.5): `pending` (new) →
`confirmed` (once provisioned) → `complete` (about 5 minutes later). In `pending` and `confirmed` the plug
answers RPC over Bluetooth without pairing; from `complete` on, pairing is enforced for every RPC over
Bluetooth. **`locked`** is the trap: the setup window expired without the plug ever going online — the
access point and RPC over Bluetooth shut down until a power cycle, and the tablet could never ARM. So the plug
goes on a network once, reaches `complete`, and only then forgets the network. Two timings follow from this
and are confirmed on the bench (check 9): the tablet must bond **before** `complete` (unpaired RPC is refused
after it), and the plug must reach `complete` **before** its setup window runs out.

1. Plug it in. With the **Shelly app on Steven's phone**, add it to a phone hotspot that has internet,
   decline the cloud (or turn it off afterwards), and let it update its firmware (≥ 2.0.0 for pairing).
2. Straight after, from the Steven Piano app's **Pair the power plug** screen, over Bluetooth, while the
   state is still `confirmed`:
   - `Shelly.GetDeviceInfo` → record `id`, `model`, `ver`, `provision`.
   - `Switch.SetConfig {id:0, config:{name:"Piano 24 V", initial_state:"off", auto_on:false,
     auto_off:false, in_mode:"detached", in_locked:true}}` — a mains blip leaves the rail off; the plug's own
     button cannot turn the rail on and leave it on (if the model rejects `in_mode`, `in_locked` alone does it).
   - `BLE.SetConfig {config:{rpc:{enable:true}}}` (restart if the reply says `restart_required`).
   - `BLE.StartPairing {timeout:180}`, then bond from Android (Just Works); `BLE.ListPairedDevices` shows
     the tablet. (Later, the bonded tablet can call `BLE.StartPairing` again to admit a replacement tablet,
     and `BLE.DeletePairedDevice {addr}` to drop one.)
   - `Cloud.SetConfig {config:{enable:false}}`.
   - Wait until `provision` reads `complete`, then forget the network **and close the access point** — an
     open access point is an unauthenticated HTTP path to the relay:
     `WiFi.SetConfig {config:{sta:{enable:false}, ap:{enable:false}}}`. From here the bonded tablet is the
     only thing that can talk to the plug.
   If the Pair screen finds the plug already `complete` and refusing: factory-reset it (the firmware update
   survives; bonds and the network do not), bond first while it is `pending`, then provision it again — the
   Pair screen can do that over Bluetooth with `WiFi.SetConfig {config:{sta:{ssid:…, pass:…, enable:true}}}`
   — wait for `complete`, and forget the network as above.
3. Verify: `Switch.Set {id:0, on:true, toggle_after:5}` → the PSU comes up, the piano's System page shows
   "7 of 7 answering" within 5 s; the plug clicks off after 5 s and the page shows "0 of 7". Then check 9.

The app reads `provision` at every connect. On `locked` it says "Unplug the power plug for five seconds and
plug it back in, then open Pair the power plug again" — the setup above was never finished. Whatever order
turns out to work on the bench is written back into this section.

## The app's routine (the contract)

Terms: **ARM** = `Switch.Set {id:0, on:true, toggle_after:30}` · **CUT** = `Switch.Set {id:0, on:false}` ·
**CHECK** = the piano's `get boards` answered within 2 s (`!boards=OK,OK,…`, BLE_SETTINGS §4).

1. **Power wanted** is true while: a song is playing or about to (from the tap on Play), live playing is on,
   the sweep or listen test runs, a firmware update is in progress and until its outcome is known
   (`!ota` confirmed, or rolled back — BLE_OTA §9), or any other path through which the app sends notes. Since
   2026-10-21 the app plays one piece per Play (no playlists, no shuffle), so power is wanted from the tap on
   Play to the piece's end, then for the safety check (rule 10) — about one relay cycle per piece played. The one-line test: power is wanted
   exactly while the app may send a Note On — so Listen (microphone only) does not want it, Test every key
   does, and the web panel's Play goes through the same gate. The Keys tab wants power while it is open; it
   drops after 10 minutes without a key, and the next key wakes it ("Waking the piano…").
2. **Rail-up.** ARM, then CHECK every 500 ms until every one of the seven boards reads OK — up to 10 s. With
   firmware 2.1.0 the boards answer within about 1 s of the PSU coming up; with 2.0.1 up to 5 s. The set of
   OK boards is the **baseline** for this rail-up (fewer than seven with the rail up is a trunk fault:
   show it). Only then send notes. If the boards never read MISSING with the rail off — the trunk side of the
   isolator fed from the controller, not from the rail — the wait is a fixed 1.5 s after ARM for the PSU to come
   up instead; the bench says which (the System page with the rail off).
3. **While power is wanted:** every 10 s, CHECK first, then ARM only if the piano answered and every
   baseline board is still OK. **Never ARM from a timer that did not just see a good CHECK.** Two failed
   CHECKs in a row → stop playback, CUT (best effort), show "The piano stopped answering — 24 V cut".
   **Exception — guest play** (decided 2026-10-21, see below): while a guest window the tablet itself started
   is running, the tablet ARMs **without CHECK**, for at most the window it set, capped at 60 minutes while the
   plug feature is on (the firmware allows 240). It shows a countdown and an **End guest play** button —
   reconnect, `lock guest 0`, the stop sequence, then CUT. When the window ends the tablet reconnects and this
   rule resumes as written.
4. **Stop.** The stop sequence (CC64 = 0, CC123), one second, then CUT. If CUT fails the timer does it
   within 30 s. Show "24 V off" calmly; it is not an error.
5. **App exit, crash, background:** nothing to do — the timer. (The app already sends the stop sequence when
   it leaves the foreground; it may CUT as well.)
6. **Firmware update:** ARM every 10 s *without* CHECK from BEGIN until the outcome is known, at most
   3 minutes: the piano reboots during the update and cannot answer, and the new image confirms itself only
   with a power board online (BLE_OTA §15.3) — an update started with the rail off would roll back. The
   firmware verified every coil off and inhibits energizing for the whole session, so this window is safe.
7. **Plug unreachable while playing:** if ARM fails twice in a row the rail will drop within 30 s anyway —
   stop playback and say "Lost the power plug".
8. **Without the plug feature** (no plug paired, or the switch off): the app behaves exactly as today.
9. **Show the plug** on the System page from `Switch.GetStatus {id:0}`: `output`, `timer_duration`,
   `apower` (the PSU's draw proves the rail is really up).
10. **Safety check** (Steven, 2026-10-21; app 1.26): after every sixth piece played, with a full-screen card:
    the stop sequence; `get active` = 0 and `get boards` all OK; the 84-key sweep while the tablet's
    microphone listens. If any key is not heard: `restart` (firmware 2.1.1, below) and a repower — CUT, 3 s,
    ARM, then the rail-up CHECK of rule 2 until the baseline — then one more sweep; still failing, Play is
    refused until a check run at the tablet passes. `!active` above 0 after the release means CUT at once,
    stop, and Play locked.

## Guest play (the iPad) — decided: (a), capped

During guest play (`BLE_LOCK.md`: `lock guest <minutes>`, 1–240) the tablet lets go of the piano so the iPad
can connect, and the piano takes one central at a time: the tablet cannot CHECK, so under rule 3 it would
never ARM and the iPad would play silence. The app session put two choices to Steven:

- **(a)** Guest play ARMs **without CHECK** for the window the tablet itself started, capped at 60 minutes
  while the plug feature is on (the firmware allows 240), with a countdown and an "End guest play" button
  that reconnects, sends `lock guest 0`, the stop sequence and CUT. Exposure: a controller or trunk that dies
  *during an attended guest session* keeps the rail up until the window ends — at most an hour, with a person
  at the iPad. The firmware's own layers (`SAFETY.md`) stay active throughout: the controller is alive and
  connected.
- **(b)** Guest play is unavailable while the plug feature is on.

Steven had no preference and took the firmware chat's recommendation, **(a)**: the fault the plug exists for is
a coil hot *while nobody is there*; a guest session is attended by definition and ends by the clock. Rule 3
carries the exception the way rule 6 carries the update's. The full rule can be restored later by letting the
piano keep a second central (the tablet's console link while the iPad plays) — a 2.2.0 candidate, noted, not
promised.

## The firmware's side (2.1.0)

- A missing board is re-probed **every 1 s** (the 5 s health check of online boards is unchanged), so the
  boards are back within about a second of the PSU coming up. `!boards` is the readiness fact — already
  there since 2.0.0.
- A board that stops answering is marked offline after a second probe, without a re-init attempt on a chip
  that has no power; `!i2cfails` keeps meaning bus trouble, not a rail that is simply off.
- A cut mid-note: the coils lose their supply; the firmware's model keeps believing them on until a verified
  OFF, which cannot land while the chip is unpowered — `!active` may stay above 0 and the hold watchdog may
  count trips until the boards return and `begin()` verifies every channel off. In that state `!i2cfails`
  climbs (an all-off that never landed *is* bus trouble) and the USB log repeats the bus-recovery line once a
  second. It arises only when a stop failed before the cut — never in the normal stop-then-cut.
- **`restart`** (firmware 2.1.1, for rule 10): a console command on a trusted connection (or USB) that makes
  the piano quiescent the way an update does — ends a bench signal test, drops queued notes, releases every
  coil through the verified all-off, pedal up, strip dark, energizing inhibited — replies `  restarting`, and
  resets the controller about 200 ms later with one more ALLCALL all-off right before. Boot re-asserts the
  all-off; the lock keys and settings are in NVS, so the piano comes back claimed; guest play, in RAM, ends.
  Refused during an update (`  refused — updating firmware`) and while `!ota=pending` (`  refused — this
  firmware has not confirmed itself yet`: a reset then would roll the update back). A deliberate restart does
  not count toward crash-safe mode. `!restartproto=1` in `dump` says the command exists.
- Nothing else changes. Every layer of `SAFETY.md` stays; the update self-test keeps needing a board online.
- Not added: a "rail off" hint command. The firmware does not need to know *why* boards are gone, and the
  screen shows nothing for missing boards.

## Bench checks, with the real plug

1. Pair it (above); `BLE.ListPairedDevices` shows the tablet; a second phone cannot talk to it.
2. ARM → `Switch.GetStatus` says `output` true with a timer; ARM again at 10 s → the timer restarts; stop
   arming → the plug clicks off at 30 s.
3. Play a song, kill the app mid-song: the rail is off within 30 s, no key stays down, the piano's screen
   is still on, `status` over USB shows every board MISSING.
4. Play a song, press RST on the controller (or unplug its USB) mid-song: the rail is off within 40 s.
5. Rail-up timing: tap Play from idle → first note within 3 s on 2.1.0.
6. Pull the trunk's RJ45 while a song plays: the next CHECK shows boards MISSING → cut within 40 s.
7. A firmware update with the plug flow: the new image confirms itself (`!ota=confirmed`).
8. Press the plug's button: nothing happens to the rail. Pull the plug's mains and restore it: the rail
   stays off.
9. Power-cycle the plug with no known Wi-Fi in range: `provision` still reads `complete`, and the tablet
   ARMs over its bond — the setup survives offline. (If it reads `locked`, the setup section is wrong: fix it.)
10. With the rail off, `get loopms` over USB stays in single-digit milliseconds: an unpowered trunk must NACK
    the probes, not hang the bus — a hung transaction waits about 1 s in this IDF build, and seven of them a
    second would starve `loop()`. If it hangs, the fix is to probe one board and skip the rest when it hangs.

## Residual risks, stated once

A channel stuck on during a song with everything alive (the song's length + 30 s; the current-sense node
fixes it); the plug's relay wearing or welding (equals today; watch `output`); the plug's firmware changing
(off every network after setup, so no updates); the plug ending `locked` because its setup never reached
`complete` (a power cycle and the Pair screen again); Bluetooth range from the tablet on the piano to the plug at the wall (test it in
place); a person moving the PSU off the plug (the label).
