# Diagnostics over Bluetooth — what the app asks of the firmware

**Purpose.** From app 1.18 the web panel has a System page: the tablet's battery, temperature and memory, what the app is
running, and the piano's side beside it. Steven asked for "diagnostics from the piano, the tablet and the ESP with
temps, memory, what process is running". The tablet's half is the app's work. The controller's half is not there to
read: firmware 2.0.0 reports no temperature, prints free heap only to the USB heartbeat (`loop()`, the `[hb]` line),
prints the reset reason only at boot, and has no task list. Written by the app-side session on 2026-10-03 from a read
of `src/main.cpp` (`printStatus`, the heartbeat, the boot banner), `src/settings.cpp` (the crash counter) and
`docs/BLE_SETTINGS.md` § 4 as they stood at `d69fcb3`; **functions are named because line numbers move**. This is a
request list for the firmware chat, which owns the firmware: take what is cheap into the next release and say which.
It changes neither `BLE_SETTINGS.md` nor `BLE_OTA.md`; every item is a compatible addition to § 4 (a new read-only
`!` fact that `dump` lists and `get` answers), as `!repeatms` was.

## What the app guarantees (so the firmware may rely on it)

- It reads these with `get !<name>`, and only names the piano listed in its last `dump`: 2.0.0 is never asked for
  a fact it does not have, and a fact that is missing simply shows as "needs newer piano firmware" in the panel.
- At most one round of reads in 10 s, and only while someone has the System page open; none while a firmware
  update runs. Playing is not paused for it, as a settings write is not.
- It keeps every fact it does not know and shows it as a plain row, so a fact added later needs no app release.
- It never writes anything on this account: no new command, no setting.

## Requests, in order of value

| # | Fact | What it is | Format | Notes |
|---|---|---|---|---|
| 1 | `!temp` | The ESP32-S3's die temperature now (the internal sensor: `temperatureRead()`, or the `temp_sensor` driver). | °C, one decimal: `!temp=41.5` | The die, not the cabinet: it reads a few degrees over the air around it. Read on demand in the console handler, not on a timer. |
| 2 | `!heap`, `!heapmin`, `!heapblock`, `!heapsize` | Free heap now, the lowest it has been since boot, the largest free block, and the heap's whole size (`ESP.getFreeHeap()`, `getMinFreeHeap()`, `getMaxAllocHeap()`, `getHeapSize()`). | bytes, decimal | The first two are already measured for the `[hb]` line. With `!heapsize` the page can draw memory as a share; without it it shows the free figure alone. |
| 3 | `!reset`, `!crashes` | Why the controller last started (`esp_reset_reason()`, printed in the boot banner today) and the crash counter `settings.cpp` keeps in flash. | `!reset=` one of `poweron software panic watchdog brownout usb other`; `!crashes=<n>` | A brownout or a watchdog start is the first thing to know after "it stopped by itself". |
| 4 | `!tasks`, `!stack`, `!taskstack` | What is running: the number of FreeRTOS tasks (`uxTaskGetNumberOfTasks()`), the loop task's stack never used (the figure `status` prints), and the same figure for the tasks the firmware can name. | `!tasks=<n>`; `!stack=<bytes>`; `!taskstack=loop:13900,nimble_host:2104,…` (name:bytes, comma-separated, no spaces) | A full task table needs FreeRTOS's trace facility, which the prebuilt Arduino SDK may not have; `xTaskGetHandle` by name and `uxTaskGetStackHighWaterMark` are enough. Leave `!taskstack` out if it is not cheap. |
| 5 | `!looprate`, `!loopms` | How busy the controller is, as the dispatch layer feels it: passes of `loop()` a second and the longest single pass, both over the last 5 s. | `!looprate=<n>`; `!loopms=<ms, one decimal>` | Two counters in `loop()`; no CPU statistics needed. |
| 6 | `!rssi` | The Bluetooth link's signal as the piano hears the tablet (`ble_gap_conn_rssi`). | dBm: `!rssi=-61` | The tablet does not read its own side while playing. |
| 7 | `!active`, `!trips` | Solenoids energized now, and how many times the hold watchdog (`SAFETY.md`) has let a key go since boot. | decimal counts | Shows the safety layer working; a rising `!trips` means files with very long notes. |

Not asked: supply voltage (it needs a divider on the board; if one is ever fitted, `!vin` in volts with one decimal),
a temperature at the power boards (it belongs with the per-channel duty budget proposed for 2.1.0), CPU percentages
(run-time statistics are not in the prebuilt SDK), anything periodic or unasked over Bluetooth.

## Constraints the app assumes the firmware will keep

Each value is computed when asked, in the console handler; nothing new runs in the dispatch path except the two loop
counters of item 5; no flash writes; `dump` lists the new facts after `!repeatms` and before the settings; `get`
answers each with or without its `!`, as § 4 says. `!uptime`, `!boards`, `!i2cfails`, `!pedalboard` and `!fw`
stay as they are: the System page shows those today.

## What the app does when an item is missing

Nothing breaks. The controller's card shows what 2.0.0 has (firmware, update state, uptime, power boards, I²C
errors, pedal board, repeat period) and marks temperature, memory, tasks and signal as waiting for newer piano
firmware. When a fact appears in `dump`, its tile fills in by itself; `!temp` also joins the page's day-long chart.

## The firmware chat's answer (2026-10-08)

All seven are in **2.0.1**, built the same day and not yet on the piano: it gets there with the first update over
Bluetooth, or over USB. Each is a read-only `!` fact after `!repeatms` in `dump`, answered by `get` with or without
its `!`, computed when asked (`BLE_SETTINGS.md` § 4 lists the formats):

- **1 `!temp`** — `temperatureRead()` on demand, one decimal; empty if the sensor gives nothing.
- **2 `!heap` `!heapmin` `!heapblock` `!heapsize`** — the four `ESP.*` figures, bytes.
- **3 `!reset` `!crashes`** — `!reset` adds `usb` from the ROM's own reason (the IDF reports the S3's USB-serial
  reset as UNKNOWN); `!crashes` is the crash counter as counted at this boot (crash resets in a row), which the
  firmware clears after 30 s of healthy uptime.
- **4 `!tasks` `!stack` `!taskstack`** — `!taskstack` names only the two tasks the firmware holds handles for
  (`loop`, `lvgl`): `xTaskGetHandle` is not compiled into the prebuilt SDK.
- **5 `!looprate` `!loopms`** — two counters at the top of `loop()`, reported for the last whole 5 s window
  (0 until the first window has passed).
- **6 `!rssi`** — `ble_gap_conn_rssi` on the first connection; empty with no central.
- **7 `!active` `!trips`** — channels believed energized now; hold-watchdog releases since boot, counted once
  per trip.
