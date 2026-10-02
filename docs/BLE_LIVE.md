# Live playing from a keyboard — what the app asks of the firmware

**Purpose.** From app 1.11 the "Steven Piano" Android app accepts any Bluetooth or USB MIDI keyboard and
forwards what is played to the piano in real time (the tablet stays the hub: keyboard → tablet → piano, the
piano's BLE-MIDI service unchanged). Nothing here is needed for that to work; each item makes it feel tighter
or safer. Written by the app-side session on 2026-10-01 from a read of `src/main.cpp`, `src/power_boards.cpp`,
`src/pedal.cpp` and the linked BLE-MIDI library as they stood that morning, before the 2.0.0 runs; **functions
are named because line numbers have since moved**. This is a request list for the firmware chat, which owns
2.0.0 (`~/.claude/plans/humming-prancing-finch.md`): take what is cheap into 2.0.0, the rest into 2.1.0, and say
which. It changes neither `BLE_SETTINGS.md` nor `BLE_OTA.md`.

## What the app guarantees (so the firmware may rely on it)

- The stop sequence (CC64 = 0, then CC123) on every new connection, when live playing is switched off, when the
  keyboard is lost or goes silent, when the app leaves the foreground, and from its crash handler.
- Live notes only while the app's Keys screen is on; never from the web panel or the cloud.
- From a keyboard: at most 200 Note Ons a second and 32 keys held (beyond that the app releases everything and
  switches live playing off); keys folded into 24–107; the same key struck again no sooner than 100 ms; only
  CC64 forwarded (values 0–127, a change across 64 sent at once, the rest at most 20 a second).
- While live playing is on, the app holds `humantime` at 0 through the settings channel and restores it after.
  It never raises `hold`.

## Requests, in order of value

| # | Today | Ask | Accept when |
|---|---|---|---|
| 1 | `loop()` calls `MIDI.read()` once per pass (one message parsed) and ends with `delay(1)`, so a chord of N notes is spread over at least N ms; the library's 64-byte queue (about 21 messages) can fill and block the NimBLE task. | Read every waiting message in a pass, bounded (for example up to 32 messages or 2 ms), and skip the `delay(1)` while any wait. | A ten-note chord sent in one BLE packet is enqueued for dispatch within one pass. |
| 2 | After a disconnect or CC120/121/123 the loop calls `pedal_release()`, then `pedal_tick(g_sustainRaw)` presses the pedal again about 15 ms later because the stored value is never reset. | Set the stored sustain value to 0 on disconnect, on CC120/121/123 and on `off`; optionally lift the pedal after a few seconds with no MIDI at all. | Kill the app with the pedal down: the pedal lifts and stays up. |
| 3 | Deferred strikes and releases (`tickPendingFires`, the `gap`, `softrelease` and min-strike paths) run on a 10 ms tick, so a repeated key lands up to 10 ms late with a varying delay. | A 1–2 ms tick for the deferred-fire path (the auto-release and restrike ticks may stay at 10 ms). | A key repeated at 8 Hz strikes within ±3 ms of even spacing. |
| 4 | `humantime` adds a random 0..N ms deferral; the `snappy` preset leaves `humantime` and `humanvel` as they were. | `humantime 0` and `humanvel 0` mean no deferral and no scatter at all; `snappy` sets both to 0. | With `humantime 0`, arrival-to-strike time has no random part. |
| 5 | The name "Steven Piano" travels only in the scan response; a nameless MIDI advertiser has to be connected to and its GAP name read. | Put the name, or a manufacturer-data marker, in the advertisement itself (within the 31 bytes beside the 128-bit MIDI UUID a short marker fits; the full name may stay in the scan response). | A passive scan tells the piano from a MIDI keyboard without connecting. |
| 6 | The app's same-key minimum is a constant 100 ms; the firmware's real minimum follows `gap`, `minstrike` and the cushion. | A fact in `dump`, for example `!repeatms=<n>`, and a shorter minimum if the solenoids allow. | The app can follow the value instead of assuming it. |
| 7 | A held key is released after `hold` (2 s, at most 4 s), so a long note from a keyboard stops unless the pedal hardware is fitted. | Not a request for 2.0.0: a statement of whether a reduced holding current under a thermal budget could ever allow longer holds (it belongs with the per-channel duty budget proposed for 2.1.0). | A decision recorded in `docs/SAFETY.md`. |

Not asked: BLE-MIDI timestamps (item 1 removes the need), a second central (the tablet is the hub), MIDI out.

## What the app does when an item is missing

Item 1: the app already sends a chord as one packet; nothing more it can do. Item 2: it sends the stop
sequence on every new connection, which cannot help a tablet that never comes back. Items 3, 4, 6: the app keeps
its 100 ms rule and holds `humantime` at 0 where the settings channel exists. Item 5: the app keeps a list of
"foreign" addresses (keyboards it has seen or opened) and never treats a bonded device as the piano. Item 7:
the Keys screen says "The piano lets a held key go after 2 seconds to keep its coils cool."

## The firmware chat's answer (2026-10-01)

**In 2.0.0:**

- **1 — chords.** `loop()` reads up to 32 MIDI messages a pass, so a chord sent in one packet reaches dispatch
  in one pass. `delay(1)` and the 24-event dispatch cap stay. The read ends for the pass once a CC 120/121/123
  has been parsed or the link has dropped: the stop is serviced first, and whatever the source sent after its
  all-off is read after it, in order.
- **2 — pedal.** Every stop that lifts the pedal (disconnect, CC 120/121/123, `off`, `panic`, a firmware
  update) forgets the stored CC64, so the pedal stays up until a source sends CC64 again. The optional
  "lift after a few seconds without MIDI" is declined: a pedal held through a long chord is ordinary music.
- **4 — snappy.** `snappy` sets `humantime 0` and `humanvel 0`; with both at 0 no random call is reached on
  the strike path, so arrival-to-strike has no random part.
- **6 — repeat period.** `dump` and `get` carry `!repeatms` = `minstrike + gap + 2 × 10 ms`
  (`BLE_SETTINGS.md` §4). The first repeat after a lone note can wait up to `isostrike − minstrike` longer.
- **Beside the list.** On a disconnect, notes still unparsed in the BLE-MIDI library's queue are discarded
  by the disconnect stop instead of striking after it.

**Planned for 2.1.0:**

- **3 — the 1–2 ms tick.** It changes dispatch timing, so it wants a second tick mode in `tools/dispatch-sim`
  and a listen on the piano.
- **5 — a marker in the advertisement.** It changes what every client scans for and needs a test with the iPad.
- **7 — longer holds.** It belongs with the per-channel duty budget and needs a temperature measurement first.
