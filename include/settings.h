// Persistent settings backed by ESP32 NVS (built-in flash region with
// wear-leveling). Saves every tunable PWM, brightness, mode, and dropdown
// state across power cycles.
//
// Usage:
//   settings_load()       call ONCE in setup() before any consumer reads
//                         a tunable (so values come from flash, not defaults).
//   settings_save()       force an immediate save. Call from the "save"
//                         serial command and from major events (e.g. BLE
//                         disconnect, where the user might power off soon).
//   settings_tick()       call from loop() — internally rate-limits to
//                         once every 30 s, NVS-internal dedup means
//                         unchanged values cost zero flash wear.
//   settings_factory()    wipe NVS and revert to config.h defaults on next
//                         boot. Bound to the "reset" serial command.

#pragma once

#include <stdint.h>

void settings_load();
void settings_save();
void settings_tick();
void settings_factory();

// Crash-loop guard. Increments a NVS-backed counter at boot; after the
// firmware has been up for >30 s without crashing, the counter resets to 0.
// If the counter reaches 3, settings_load() forces g_fullPowerMode = true
// regardless of the saved value — gets the user out of a PWM-induced
// crash loop without manual intervention.
//
//   settings_boot_inc()     call early in setup, returns the new count
//   settings_boot_reset()   call after ~30 s of healthy uptime
extern uint8_t settings_boot_inc();
extern void    settings_boot_reset();
