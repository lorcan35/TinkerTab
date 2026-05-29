---
audience: developer
type: explanation
prerequisites: none
last-verified: 2026-05-29
---
# LVGL on the ESP32-P4 — why the UI is built the way it is

## The question

The [Tab5](../../GLOSSARY.md)'s entire UI is [LVGL](../../GLOSSARY.md) v9.2.2 on
an ESP32-P4 — a microcontroller with ~512 KB of tight internal SRAM driving a
720×1280 display. This page explains the constraints that shaped how the UI is
written: where LVGL config lives, why overlays are hidden instead of destroyed,
why `lv_async_call` is banned, and the rendering patterns that quietly bust the
render budget. It is the *why* behind the rules; for the file layout see the
[firmware file map](../reference/firmware-file-map.md).

## The model

### Memory: a TLSF pool you grow yourself

LVGL allocates from a TLSF heap. On the Tab5 the base pool is a **64 KB**
BSS-allocated chunk (`CONFIG_LV_MEM_SIZE_KILOBYTES=64`), and `main.c` adds a
**2 MB PSRAM-backed pool** at boot with `lv_mem_add_pool()`. The key
misconception to unlearn:

```
CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096
```

is **not** an auto-expand trigger. LVGL 9.2.2 has **no** auto-expand; that value
is only the TLSF per-pool max-size ceiling
(`TLSF_MAX_POOL_SIZE = LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE`). The only way to
grow the heap is the explicit `lv_mem_add_pool()` call. The base pool was
dropped from 96 KB to 64 KB to free internal-SRAM headroom; pushing it to 128 KB+
aborts at boot.

### Config lives in `sdkconfig.defaults`, not `lv_conf.h`

The ESP-IDF LVGL component sets `CONFIG_LV_CONF_SKIP=1`, so **`lv_conf.h` is
completely ignored**. Every LVGL setting must go in `sdkconfig.defaults`. Verify
a setting actually took with `grep "SETTING" build/config/sdkconfig.h` after a
build. `sdkconfig` changes need `idf.py fullclean build` — incremental builds
cache stale config.

### Rendering: partial mode, two PSRAM buffers

The display runs `LV_DISPLAY_RENDER_MODE_PARTIAL` with two 144 KB draw buffers
in PSRAM (DIRECT mode tears on DPI). The DPI peripheral DMAs straight out of
PSRAM, so the firmware calls `esp_cache_msync()` after CPU framebuffer writes to
keep the cache coherent. Asserts are off
(`CONFIG_LV_USE_ASSERT_MALLOC=n`, `..._NULL=n`) so an alloc failure crashes fast
with a backtrace instead of hanging in a `while(1)` that trips the 60 s WDT.

### Internal-SRAM fragmentation and the hide/show pattern

`heap_caps_get_free_size()` *lies* — total free is not usable free. What matters
is `heap_caps_get_largest_free_block()`. Creating and destroying LVGL overlays
allocates and frees many small objects from internal SRAM; over hours this
fragments the heap until total free looks healthy but no single block is large
enough and allocations fail.

The fix is **hide/show, not create/destroy**. Settings, Chat, and Voice overlays
are created once and toggled with `lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)` /
`lv_obj_remove_flag(...)`. `dismiss_all_overlays()` hides; it does not destroy.
Overlays with live timers/animations *must* be hidden (destroying corrupts the
timer linked list). Destroy only when permanently replacing (e.g. New Chat).

A **fragmentation watchdog** (`heap_watchdog.c`) is the safety net: if the
internal-SRAM largest free block stays below ~30 KB for ~3 minutes it triggers a
controlled reboot (honoring a voice-active grace window).

### `lv_async_call` is not thread-safe — use `tab5_lv_async_call`

LVGL 9.x's `lv_async_call` does `lv_malloc` + `lv_timer_create` against the
unprotected TLSF heap. The codebase treated it as thread-safe for years (a wrong
comment in `ui_core.c`) and it was the real root cause of a long-residual crash
class. The rule now: **always call `tab5_lv_async_call(cb, arg)`** (it wraps the
site under `tab5_ui_lock`), never the LVGL primitive directly. All 49 call sites
were converted; a new direct call is a regression.

### Render-budget footguns

A handful of LVGL features quietly stall the UI task into a continuous
"mutex timeout" on large or frequently-invalidated objects:

| Pattern | Symptom | Use instead |
|---|---|---|
| `transform_scale` on a `bg_grad` object | re-rasterizes the gradient every tick | animate `bg_opa` on a sibling solid object |
| `bg_angles(270, 630)` for a full ring | wraps past 360, renders invisible | `bg_angles(0,360)` + `lv_arc_set_rotation(270)` |
| `shadow_width ≥16 px` on a ≥200 px / ≥5 Hz object | busts the render budget | `bg_opa` or a pre-rendered blur sprite |
| 5-stop gradient on a 280 px body @ 30 Hz | continuous mutex timeout | fewer stops, pre-rendered sprite, or a baked-dither canvas |

The broader lesson from the orb-aliveness arc: **subtraction over addition** —
each visual layer added made it worse; the keeper had ember/rings/counter-phase
*removed*.

## Why it's built this way

- **The P4 is memory-bound, not compute-bound, for UI.** The dominant failure
  mode is not "too slow to render" but "ran out of a contiguous block." Every
  pattern above — the PSRAM pool, hide/show, the watchdog — is about keeping the
  largest free block healthy, not about frame rate.

- **A single draw thread means a single lock.** The UI task is the only thread
  allowed to touch LVGL objects after a screen is created; background tasks must
  marshal through `tab5_lv_async_call`. This is why the thread-safety of that one
  primitive mattered so much.

- **Fail fast beats hang.** Disabling LVGL asserts trades a clean abort backtrace
  for a `while(1)` WDT hang — a deliberate choice that made the crash class
  debuggable.

## See also

- [Firmware file map](../reference/firmware-file-map.md) ·
  [Hardware reference](../reference/hardware.md)
- [Recover a stuck device](../how-to/recover-a-stuck-device.md) — the
  fragmentation watchdog in practice.
- [TinkerTab architecture](architecture.md) · deep dive:
  [`../STABILITY-INVESTIGATION.md`](../STABILITY-INVESTIGATION.md)
