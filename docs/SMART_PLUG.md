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
| A channel stuck on **while a piece plays** and everything answers | the hold watchdog, if the firmware can see it | **caught by the app, not the firmware** — the floor watch of rule 11 (app 1.30) cuts within about 20 s of pedal-up playing, and the post-piece check refuses the next Play. The relay board does not cover it either; the per-board current sense of `RS485_TRUNK.md` will. Keep `hold` at 2000. |
| The plug's relay welds, or someone moves the PSU to a wall outlet | — | back to today, never worse (the app warns when `apower` stays up after a cut) |

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
  app detects (`Switch.GetStatus` → `apower` still above the threshold after a cut; `output` only reports the
  commanded state). If the PSU has no soft-start, an NTC
  inrush limiter in its mains lead is cheap insurance (吟晚风 to check the PSU's datasheet).
- Pairing (the plug refusing unpaired phones) needs Shelly firmware **2.0.0 or later**
  (`BLE.StartPairing`). If the plug ships older, the Pair screen updates it over Bluetooth through the phone's
  hotspot (Steven Piano 1.28); the Shelly app once if the plug refuses `Shelly.Update` over Bluetooth. On older
  firmware the plug still works, but anyone in range could switch the rail — harmless
  for safety (on = the controller holds every coil off; off = silence), not acceptable long-term. Steven chose
  to run his plug as it is (1.29, below), with the plug watch as the guard.

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

**Without the Shelly app (Steven Piano 1.28, M65).** Steven's plug shipped below 2.0.0, and he chose to skip the
Shelly app and its account: the Pair screen updates the plug itself, over Bluetooth, through the phone's hotspot
(with internet; 2.4 GHz — "Maximize Compatibility" on an iPhone). The hotspot's name and password are typed once
on the tablet and held in its memory only, never stored or logged. One caution: before 2.0.0 the plug cannot
encrypt its Bluetooth link, so that password travels in the clear for the one step that sends it — use a
hotspot password made for the day and change it after. Step 1 below becomes optional: after Find, the Pair
screen reads the plug and chooses the way.

- **Firmware older than 2.0.0, not set up with this tablet** (`pending`, `confirmed`, or no `provision` field
  before 1.7.5): the update, over Bluetooth.
  1. `Switch.Set {id:0, on:false}`, then `Switch.SetConfig {id:0, config:{initial_state:"off"}}`. The rail
     stays off through the restarts; if old firmware refuses the second call, that is logged and the update
     goes on.
  2. `WiFi.SetConfig {config:{sta:{ssid, pass, enable:true}}}`, then `WiFi.GetStatus` every 2 s until `status`
     is `got ip` (60 s at most). The plug goes `confirmed` here.
  3. `Shelly.CheckForUpdate` → `stable.version`. If there is none, or it is below 2.0.0: "no update right now",
     with the Shelly app as the way round.
  4. `Shelly.Update {stage:"stable"}`. On an error or no reply the plug will not take its update over
     Bluetooth (Shelly does not document it; bench check 11): use the Shelly app once (it needs a Shelly
     account), then the Pair screen again.
  5. The link drops. `Shelly.GetDeviceInfo` every 5 s until `ver` is 2.0.0 or later (6 minutes at most; past
     that, a power cycle and a new search).
  6. At once, `Shelly.FactoryReset` (the new firmware stays; the network, the bonds and the provisioning go):
     the plug went `confirmed` on the hotspot and must not reach `complete` unbonded. Then `Shelly.GetDeviceInfo`
     until `provision` reads `pending`, and `Switch.Set {id:0, on:false}` once more. If it is already
     `complete` and refusing: the factory reset by its button (bench check 11).
  7. On as for a new plug, with the same hotspot.
- **A new plug (`pending`) on 2.0.0 or later, or the plug just updated:** bond first, then provision, over
  Bluetooth — `BLE.StartPairing {timeout:180}` and Android's bond (`BLE.ListPairedDevices` before and after);
  the hotspot again with `WiFi.SetConfig {config:{sta:{ssid, pass, enable:true}}}`, then `confirmed` awaited;
  the `Switch.SetConfig`, `BLE.SetConfig` and `Cloud.SetConfig` of step 2; `complete` awaited, then at once
  `WiFi.SetConfig {config:{sta:{enable:false}, ap:{enable:false}}}`; the check of step 3.
- **`confirmed` on 2.0.0 or later** (set up in the Shelly app): step 2 as written.

The tablet keeps the plug as paired only once its network is off and its access point closed: a setup that
stops before that leaves the app not switching the rail. A `complete` plug this tablet is bonded with (set up
by another tablet, or a setup cut short after its bond) gets the settings, the cloud off and the network closed
once more before its check.

**As it is (Steven Piano 1.29, Steven's choice).** On firmware older than 2.0.0 the Pair screen offers *Use it
as it is* first and *Update and lock it* second.
- **The steps:** switch off; the same `Switch.SetConfig` (with the `in_mode` retry); `BLE.SetConfig` RPC on;
  cloud off.
- **Network:** only with `provision` pending or confirmed (1.7.5 and later) does it ask for the hotspot once,
  wait for `got ip` and `complete` (6 min at most), and check that the plug still answers unpaired. If it
  refuses, the words say it needs its update after all, the reset is shown, and nothing is stored.
- **Before 1.7.5, or already `complete`:** no network is needed.
- **Then:** the network and access point are closed before anything is stored. The pairing is stored with
  `locked=false` and no bond entry, then the check runs.
- **An unlocked plug:** any phone in range with the Shelly app can switch it, so **the plug watch** runs for
  every plug, locked or not. While nothing wants the rail (and never during an update or guest play),
  `Switch.GetStatus` every 60 s. If the plug is found on: its settings are put back, then a CUT with the weld
  check, and "The power plug had been switched on by something else: switched off". Settings are put back
  (`initial_state`, `auto_on`, `in_locked`, `in_mode`, the cloud, `sta`/`sta1`/`ap`) on the first read, after
  a missed read, whenever it is found on, and every 10 minutes.
- **Reset:** a factory reset only works within 60 s of plugging in, by holding the button 10 s (5 s only
  reboots), per Shelly's documentation.

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

In `locked` nothing answers over Bluetooth RPC — `Shelly.GetDeviceInfo` included: the GATT connection succeeds
and the RPC service is absent or silent. A plug with no bond that connects but gives no RPC reply within 5 s is
taken as `locked`, and the app says "Unplug the power plug for five seconds and plug it back in, then open Pair
the power plug again" — the setup above was never finished. A bonded plug in `complete` always answers, and the
app reads `provision` at every connect while it does. Whatever order turns out to work on the bench is written
back into this section.

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
2. **Rail-up.** CHECK (the piano answering), ARM, then CHECK every 500 ms until every one of the seven boards
   reads OK — up to 10 s. With firmware 2.1.0 the boards answer within about 1 s of the PSU coming up; with
   2.0.1 up to 5 s. The set of OK boards is the **baseline** for this rail-up (fewer than seven with the rail
   up is a trunk fault: show it). Only then send notes. Whether the boards read MISSING with the rail off is
   not known from the hardware notes — the trunk side of the ISO7721 barrier has its own supply, and nothing
   says whether it comes from the rail or from the master board — so both are handled: if every board already
   answers before ARM, the wait is a fixed 1.5 s after ARM for the PSU to come up, and `apower` above the
   threshold of rule 9 proves the rail is up. The bench settles it (the System page with the rail off).
3. **While power is wanted:** every 10 s, CHECK first, then ARM only if the piano answered and every
   baseline board is still OK. **Never ARM from a timer that did not just see a good CHECK.** A CHECK fails
   when the piano does not answer within 2 s *or* answers with a baseline board MISSING; a failed CHECK or ARM
   is retried after 2 s, and two failures in a row → stop playback, CUT (best effort), show "The piano stopped
   answering — 24 V cut" (or "… lost a power board").
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
   firmware verified every coil off and inhibits energizing for the whole session, so this window is safe. The
   window starts at the first acknowledged data window after BEGIN; after the cap the app returns to
   CHECK-then-ARM. If the rail drops before the new image has confirmed itself, nothing is lost: the self-test
   waits for a board online and never times out, so it completes at the next rail-up (`restart` is refused
   while `!ota=pending`, so the app cannot reset it into a rollback).
7. **Plug unreachable while playing:** if ARM fails twice in a row the rail will drop within 30 s anyway —
   stop playback and say "Lost the power plug".
8. **Without the plug feature** (no plug paired, or the switch off): the app behaves exactly as today.
9. **Show the plug** on the System page from `Switch.GetStatus {id:0}`: `output`, `timer_duration`, `apower`.
   `output` is the *commanded* state: a welded contact still reads off. `apower` is the proof both ways — above
   the threshold after ARM the rail is really up; above it after a CUT the relay has welded (show it; the piano
   is back to today's state). The threshold comes from the bench: half the PSU's idle draw with the piano
   silent, at least 2 W. The plug's clock has no network to set it, so `timer_started_at` is unusable: the app
   counts the 30 s from its own ARM.
10. **Safety check** (Steven, 2026-10-21; app 1.26): after every sixth piece played, with a full-screen card:
    the stop sequence; `get active` = 0 and `get boards` all OK; the 84-key sweep while the tablet's
    microphone listens. If any key is not heard: `restart` (firmware 2.1.1, below) and a repower — CUT, 3 s,
    ARM, then the rail-up CHECK of rule 2 until the baseline — then one more sweep; still failing, Play is
    refused until a check run at the tablet passes. `!active` above 0 after the release means CUT at once,
    stop, and Play locked. The restart is an *expected* link loss (app 1.27): once the piano has answered
    `  restarting`, a CHECK that finds it gone sends the rail quietly to off — no stop sequence, no CUT, no
    message; the plug's own 30 s timer bounds the relay, since the last ARM is at most 10 s old — and the
    reconnect runs the normal rail-up of rule 2. The check never passes without reading the coils: with no
    `dump` from the connection it asks once more, then ends unfinished ("The piano didn't report its keys.
    Reconnect it.") and Play waits for a completed check.
11. **Power watch** (Steven, 2026-10-23; app 1.30), two parts, no firmware change. **(a) The floor watch during the piece**
    (app 1.30, as built). *When:* while a piece plays with the rail up after a checked rail-up (not while kept
    for an update or guest play), the app reads `apower` every 1.5–2.5 s — 2 s on average, the steps varied so
    a steady beat cannot stay in step. *Expected draw:* idle + 12 W × the keys the app's router held; each key
    counts from its strike for at least 60 ms (`minstrike`) and at most the piano's `hold` — the hold watchdog
    releases every coil then, so a coil on longer is the fault. *Which second:* the second `apower` averaged is
    the plug's own, so the held count is taken over every 1 s window ending 0–1 s before the request; a reading
    counts when it is more than T (6 W) above expected even at the busiest window, restarts the run when it is
    not even at the quietest, and otherwise does neither. *The trip:* 8 counted readings in a row spanning at
    least 16 s → stop the piece, CUT, "A key stayed on — 24 V cut", and the safety check runs before the next
    Play — about 20 s after the fault with the pedal up. *The pedal and PWM:* a reading the pedal may have
    touched (CC64 down within 2.5 s of it) restarts the run; with Full power off (PWM drive: each coil draws its
    note's duty) the floor watch is off, and (b) still runs with T at its 3 W floor. *Idle:* the rail-up
    reading after the CHECK, then the lowest reading with no key held and the pedal up. *The rail-up cap:* more
    than 30 W with nothing sent, twice a second apart → CUT, "The 24 V draws N W with nothing playing — 24 V
    cut", and the same check. *Bench (check 12):* confirm `apower` is about a 1 s mean (a much shorter one
    could trip on block chords with short gaps), and note the idle draw at rail-up and with the pedal up.
    **(b) The post-piece check:** once per piece, between the stop sequence
    and the CUT. After CC64 = 0 and CC123 (the pedal node releases), two `apower`
    samples about 1 s apart, both expected back near the **pedal-up idle draw** learned at each rail-up right
    after the CHECK (every channel verified off by `begin()` then); also `!active` and `!i2cfails`. Then CUT as
    always, about 1 s later than before. A draw more than about 6 W above idle (half a coil: one coil is ~0.5 A,
    ~12 W at Full power, roughly duty × 12 W in PWM mode; scaled with a floor), or `!active` above 0, or new
    I²C failures → "A key stayed on after the piece — 24 V cut", and Play waits for a safety check. As built
    (1.30) it runs at any rail stop after keys were struck — the Keys tab, Test every key and the safety
    check's own sweep included — and the CUT comes about 2.2 s after the stop sequence. Together: a key stuck
    mid-piece is caught within about 20 s (or at the next pedal-up stretch), and the next piece never runs on a
    faulty channel. **Known gap:** a coil already on at rail-up is counted into idle, and only the 30 W cap
    catches it — with the PSU idling at 5–15 W plus the pedal node, idle + one coil (~12 W) can sit under
    30 W, which is exactly a shorted MOSFET at rail-up. Remembering the usual idle across rail-ups (and
    tripping when a rail-up's idle is more than T above it) closes it — built as (c), app 1.31. The pedal node is on the rail (MKS SERVO42C stepper, load-adapting:
    10–30 W holding the pedal down, a few watts up; it reboots at every rail-up, so the pedal must be up when the
    rail comes up — the app's rail-up precedes the piece's first CC64); the pedal-down baseline is recorded
    during the safety check for later tuning, not used for a cut.
    **(c) The usual idle** (app 1.31). Idle is learned at each rail-up, so a coil already on then (a shorted
    MOSFET, the burnt-coil fault) would count as idle, and one channel (~12 W over a 5–15 W supply and the pedal
    node) also stays under the 30 W rail-up cap. *What is kept:* the app keeps each plug's last 5 rail-up idle
    readings, with their times, with its pairing; a forgotten or re-paired plug starts empty. *The trip:* with at
    least 3 kept, a rail-up reading more than T above their median (6 W at Full power, 3 W in PWM) that is still
    so 1 s later with no notes held (or unread then) means CUT, "The 24 V draws N W more than usual with nothing
    playing — 24 V cut", and Play waits for a safety check. A reading that tripped is never kept. *After a trip:*
    a safety check that passes with the microphone sweep (every key heard) clears the kept readings — a shorted
    channel holds its key down, so its strike cannot sound and the sweep fails; a check without the sweep keeps
    them, so the trip repeats at each rail-up (annoying, but safe). *Every checked rail-up is weighed,* the
    safety check's own too: while the draw stays high the check cannot sweep, so a lasting new normal (a
    different supply) means forgetting and re-pairing the plug. No firmware change.

## Guest play (the iPad) — decided: (a), capped

During guest play (`BLE_LOCK.md`: `lock guest <minutes>`, 1–240) the tablet lets go of the piano so the iPad
can connect, and the piano takes one central at a time: the tablet cannot CHECK, so under rule 3 it would
never ARM and the iPad would play silence. The app session put two choices to Steven:

- **(a)** Guest play ARMs **without CHECK** for the window the tablet itself started, capped at 60 minutes
  while the plug feature is on (the firmware allows 240), with a countdown and an "End guest play" button
  that reconnects, sends `lock guest 0`, the stop sequence and CUT. Exposure: a controller or trunk that dies
  *during an attended guest session* keeps the rail up until the window ends — at most an hour, with a person
  at the iPad. The firmware's own layers (`SAFETY.md`) stay active throughout: the controller is alive and
  connected. The app lets go of the piano by itself when guest play starts with the plug feature on (the iPad
  needs the one connection). If End guest play cannot reconnect within 15 s (the iPad holds the piano), the
  app CUTs anyway without `lock guest 0`: the window runs out on the piano by itself, with no power — silence,
  not a hazard.
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
  firmware has not confirmed itself yet`: a reset then would roll the update back). The links are closed right before the reset, so the app sees the drop at once. A deliberate restart
  does not count toward crash-safe mode (the counter counts only panic, watchdog and brownout resets), and it
  does not save: settings changed in the last 30 s are kept only if `save` is sent first. `!restartproto=1` in `dump` says the command exists.
- **No fault-injection command.** The app asked for a bench-only `fault hold` that energizes a coil outside
  the safety model to prove rule 11; Steven decided "no firmware" (2026-10-23). The bench uses a dummy load
  instead (check 12). The firmware will never hold a coil its own model cannot see.
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
11. Unverified until the plug is on the bench, written here so they are not forgotten: the factory-reset
    gesture (Shelly's docs: hold the button 10 s within 60 s of plugging in, 5 s only reboots — confirm); the `BLE.ListPairedDevices` reply shape
    (`addr`, `ctime`, `atime` per the docs); whether `Shelly.Reboot` keeps the setup window; the PSU's idle
    `apower` with the piano silent, for rule 9's threshold; whether `Shelly.Update` is accepted over Bluetooth;
    the provisioning window across the update's restart (`confirmed` at the hotspot, `complete` about 5 minutes
    later; the update about 2, the reset straight after); the relay's state after `Shelly.FactoryReset`;
    whether firmware before 2.0.0 takes `initial_state`.
12. The post-piece power check (rule 11) with a **dummy load**: a 47 Ω / 25 W resistor on an XT60 pigtail,
    plugged into the 24 V chain — the same ~12 W step as a stuck coil, no coil heat. Plugged in mid-piece with
    the pedal up, the floor watch must cut within about 20 s; plugged in just before a piece ends, the
    post-piece check must cut and refuse the next Play. Unplug the load, run the safety check, Play again. An ordinary
    held note proves the `!active` and `!i2cfails` reads.

## Residual risks, stated once

A channel stuck on during a song with everything alive (the song's length + 30 s; the current-sense node
fixes it); the plug's relay wearing or welding (equals today; watch `apower`); the plug's firmware changing
(off every network after setup, so no updates); the plug ending `locked` because its setup never reached
`complete` (a power cycle and the Pair screen again); Bluetooth range from the tablet on the piano to the plug at the wall (test it in
place); a person moving the PSU off the plug (the label).
