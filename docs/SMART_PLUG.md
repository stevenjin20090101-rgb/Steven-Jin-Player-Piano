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
  its Wi-Fi/Bluetooth mode, never Zigbee mode. **Join it to no Wi-Fi network and no cloud:** it is driven over
  Bluetooth only, and with no network nothing can change its firmware or settings under us.
- **Only the 24 V PSU goes on the plug.** The controller's own supply and the LED strip's 5 V supply stay on
  another outlet: the piano must keep its screen, Bluetooth, settings and console while the rail is off —
  that is the whole point. Label the plug: *PIANO 24 V — must stay on the smart plug*.
- The PSU is a 24 V / 40 A switch-mode supply; its inrush is switched by the plug's relay at every rail-up.
  The app keeps the rail up through a playlist's gaps so an evening costs a handful of cycles, not one per
  song. If the relay ever sticks closed the app sees it (`Switch.GetStatus` → `output` true after a cut).
- Pairing (the plug refusing unpaired phones) needs Shelly firmware **2.0.0 or later**
  (`BLE.StartPairing`). If the plug ships older, update it once over Wi-Fi with the Shelly app, then forget
  the network. On older firmware the plug still works, but anyone in range could switch the rail — harmless
  for safety (on = the controller holds every coil off; off = silence), not acceptable long-term.

## One-time setup (Steven, with the app's "Pair the power plug" screen)

RPC over Bluetooth is the plug's JSON-RPC carried over a GATT service (the Mongoose OS RPC service:
service `5f6d4f53-5f52-5043-5f53-56435f49445f`, data characteristic `…-5f64-6174615f5f5f`, TX control
`…-5f74-785f63746c5f`, RX control `…-5f72-785f63746c5f`; the client writes the frame length as 4 bytes
big-endian to TX control, writes the JSON frame to data in MTU-sized chunks, reads the response length from
RX control, then reads data until it has it all). **The app session verifies these against the Shelly
docs before coding** ("RPC over BLE"; the BLE component page documents `BLE.StartPairing`, the bond limit
of 20, and that RPC over Bluetooth is encrypted and authenticated once the plug's provisioning state is
`complete`; in `pending` / `confirmed` it answers without pairing, which is what makes first contact
possible).

1. Plug it in. A new plug is in provisioning state `pending`: it answers RPC over Bluetooth without pairing.
2. The app connects to `ShellyPlugUSG4-…`, then:
   - `Shelly.GetDeviceInfo` → record `id`, `model`, `ver` (firmware ≥ 2.0.0 wanted).
   - `Switch.SetConfig {id:0, config:{name:"Piano 24 V", initial_state:"off", auto_on:false,
     auto_off:false, in_mode:"detached", in_locked:true}}` — a mains blip leaves the rail off; the
     plug's own button cannot turn the rail on and leave it on (if the model rejects `in_mode`, `in_locked`
     alone does it).
   - `BLE.SetConfig {config:{rpc:{enable:true}}}` (restart if the reply says `restart_required`).
   - `BLE.StartPairing {timeout:180}`, then bond from Android (Just Works); `BLE.ListPairedDevices` shows
     the tablet. Finish the plug's provisioning so its state is `complete` (the BLE page's "Provisioning
     Integration" section says how; the app session checks it): from then on unpaired clients are refused.
3. Verify: `Switch.Set {id:0, on:true, toggle_after:5}` → the PSU comes up, the piano's System page shows
   "7 of 7 answering" within 5 s; the plug clicks off after 5 s and the page shows "0 of 7".

## The app's routine (the contract)

Terms: **ARM** = `Switch.Set {id:0, on:true, toggle_after:30}` · **CUT** = `Switch.Set {id:0, on:false}` ·
**CHECK** = the piano's `get boards` answered within 2 s (`!boards=OK,OK,…`, BLE_SETTINGS §4).

1. **Power wanted** is true while: a song is playing or about to (from the tap on Play), live playing is on,
   the sweep or listen test runs, a firmware update is in progress and until its outcome is known
   (`!ota` confirmed, or rolled back — BLE_OTA §9), or any other path through which the app sends notes. It
   stays true through a playlist's gaps and drops when playback ends.
2. **Rail-up.** ARM, then CHECK every 500 ms until every one of the seven boards reads OK — up to 10 s. With
   firmware 2.1.0 the boards answer within about 1 s of the PSU coming up; with 2.0.1 up to 5 s. The set of
   OK boards is the **baseline** for this rail-up (fewer than seven with the rail up is a trunk fault:
   show it). Only then send notes.
3. **While power is wanted:** every 10 s, CHECK first, then ARM only if the piano answered and every
   baseline board is still OK. **Never ARM from a timer that did not just see a good CHECK.** Two failed
   CHECKs in a row → stop playback, CUT (best effort), show "The piano stopped answering — 24 V cut".
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

## The firmware's side (2.1.0)

- A missing board is re-probed **every 1 s** (the 5 s health check of online boards is unchanged), so the
  boards are back within about a second of the PSU coming up. `!boards` is the readiness fact — already
  there since 2.0.0.
- A board that stops answering is marked offline after a second probe, without a re-init attempt on a chip
  that has no power; `!i2cfails` keeps meaning bus trouble, not a rail that is simply off.
- A cut mid-note: the coils lose their supply; the firmware's model keeps believing them on until a verified
  OFF, which cannot land while the chip is unpowered — `!active` may stay above 0 and the hold watchdog may
  count trips until the boards return and `begin()` verifies every channel off. Expected, not a fault.
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

## Residual risks, stated once

A channel stuck on during a song with everything alive (the song's length + 30 s; the current-sense node
fixes it); the plug's relay wearing or welding (equals today; watch `output`); the plug's firmware changing
(no Wi-Fi, no updates); Bluetooth range from the tablet on the piano to the plug at the wall (test it in
place); a person moving the PSU off the plug (the label).
