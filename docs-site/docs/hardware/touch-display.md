---
title: Touch + display
sidebar_label: Touch + display
---

# Touch + display

## The panel

5-inch portrait MIPI-DSI panel running at 720×1280 @ 60 Hz. Driven by the ESP32-P4's built-in DSI controller (1-lane). The framebuffer lives in PSRAM; the DPI DMA reads PSRAM directly so we have to call `esp_cache_msync()` after CPU writes to keep the visible output coherent.

LVGL is configured for **partial-render** mode with two 144 KB draw buffers (also in PSRAM). The two buffers swap between draw and flush phases. Direct mode would be faster but causes tearing on the DSI panel.

## Touch

Capacitive multitouch via a Goodix GT911 controller on I2C bus 1. Up to 5 simultaneous touches reported. Tab5 firmware uses single-touch + LVGL's gesture recogniser for swipe / long-press / pinch.

The touch layer has a **debounce gate** (`ui_tap_gate`, 300 ms) to suppress rapid-fire taps that previously caused crashes when overlapping animation lifecycles raced. Don't bypass it.

## What you'll see

A single "home" screen with overlays. Most everyday flows happen on the home:

- The orb (voice trigger)
- The mode pill (current voice mode)
- The greeting + clock
- The live widget slot
- The nav sheet chevron at the bottom

Tap the chevron and you get a 3×3 nav grid: Chat / Camera / Notes / Settings / Files / Memory / Sessions / Agents / TinkerClaw.

Each of those is a fullscreen overlay (Chat / Settings / Voice) or a separate `lv_screen_load`'d screen (Notes / Files / Camera). The hide/show pattern over destroy/create is intentional — see [LVGL conventions](/docs/firmware-reference/lvgl-conventions) for why.

## Brightness

Adjust in Settings → Display → Brightness (0–100). The driver uses PWM via the LP I/O controller. Persisted in NVS as `brightness` (default 80%).

You can also set it remotely from the debug server:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/display/brightness?p=50"
```

## Quiet hours

Set in Settings → Voice → Quiet hours. Outside the configured range Tab5 dims the display, mutes notification chimes, and enters a lower-power orb breathing pattern. NVS keys: `quiet_on`, `quiet_start` (hour 0–23), `quiet_end` (hour 0–23, can wrap past midnight).

## Tear / flicker / black-screen

If the display is glitching:

- **Power-cycle** first. 90% of glitches are cache coherency on a startup race.
- Check `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` on serial — if PSRAM is fragmented below 1 MB, the framebuffer can't allocate cleanly.
- A boot loop is almost certainly NOT the display. See [stability guide](/docs/firmware-reference/stability-guide).
