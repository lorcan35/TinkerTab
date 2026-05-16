---
title: Power on the Tab5
sidebar_label: Power on the Tab5
---

# Power on the Tab5

This is the first 30 seconds of using a freshly-flashed Tab5.

## Step 1 — Connect power

Plug a USB-C cable into the port on the side of the Tab5. The other end goes into a 5 V power adapter rated at least 2 A — most modern phone chargers work. The internal battery charges while powered; ~45 min to 80%.

:::warning 5 V only
The Tab5 must be powered by a **5 V** supply. Higher voltages will damage the device. Use the supplied adapter or a known-good USB-C PD source negotiated to 5 V.
:::

## Step 2 — Long-press the power button

The power button is on the **top edge** of the device. Press and hold for **about 3 seconds** until the TinkerTab splash logo appears on the screen, then release.

The boot sequence runs for ~4 seconds:
- Hardware bring-up (display, audio, camera)
- LVGL UI init
- Wi-Fi service starts (if previously paired)
- Voice WebSocket connects to Dragon (silent — no overlay popup)

## Step 3 — Onboarding flow (first boot only)

If this is the first boot after flashing, you'll see the onboarding flow:

1. **Welcome** — tap *Continue*
2. **Wi-Fi** — pick your home network, enter the password using the on-screen keyboard, tap *Connect*
3. **Dragon discovery** — Tab5 looks for `tinkerclaw.local` via mDNS, falls back to manual `host:port` entry
4. **First voice prompt** — the orb pulses; tap and hold to talk

Once you've completed onboarding, this flow won't run again. Settings can be edited later via the Settings overlay (top-right gear icon on the home screen).

## Step 4 — You're on the home screen

![TinkerTab home screen](/img/home.jpg)

What you'll see:

- **Top bar**: clock, mode pill (Local / Hybrid / Cloud / TinkerClaw / Onboard), Wi-Fi indicator
- **Greeting**: "Good morning" / "Good afternoon" / "Good evening" based on the local time
- **Voice orb**: the green pulsing circle in the middle. Tap to start a voice command.
- **Now-card**: shows the active live widget — current timer, next reminder, etc.
- **Nav sheet**: tap the chevron at the bottom to open Chat / Camera / Notes / Settings / Files

## Hard reset (if anything's wrong)

Three layers, cheapest first:

1. **Soft reboot**: pinhole reset on the rear (use a paperclip, gentle press)
2. **Force OTA rollback**: from another device on the LAN, `curl -X POST http://<tab5-ip>:8080/ota/rollback -H "Authorization: Bearer $TOKEN"`
3. **Hard power-off**: long-press the power button for **8 seconds**

If even hard power-off doesn't help, see [Troubleshooting](/docs/troubleshooting).
