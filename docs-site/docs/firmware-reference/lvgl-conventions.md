---
title: LVGL conventions
sidebar_label: LVGL conventions
---

# LVGL conventions

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

The Tab5 firmware uses LVGL 9.2.2. Three rules to remember.

## 1. All LVGL config goes in `sdkconfig.defaults`, NOT `lv_conf.h`

The ESP-IDF LVGL component sets `CONFIG_LV_CONF_SKIP=1`, which means **`lv_conf.h` is completely ignored**. Any change to it has zero effect.

If you want to tune memory pool size, render mode, font caching, asserts, or *any* LVGL config, edit `sdkconfig.defaults` and use the `CONFIG_LV_*` symbols. Verify with:

```bash
grep "LV_MEM_SIZE_KILOBYTES" build/config/sdkconfig.h
```

after a build.

Current LVGL settings:

- **`CONFIG_LV_MEM_SIZE_KILOBYTES=64`** — BSS-allocated base pool. Soft ceiling for this firmware layout; 128+ aborts at boot in `main_task`.
- **`CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096`** — *per-pool* TLSF max-size ceiling. **Not** an auto-expand trigger. Auto-expand is something we do ourselves at boot via `lv_mem_add_pool()` with a 2 MB PSRAM chunk in `main.c`.
- **`CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=32`** — bumped from 4 because 5+ rounded objects rendering simultaneously crashed the cache.
- **`CONFIG_LV_USE_ASSERT_MALLOC=n`** + **`CONFIG_LV_USE_ASSERT_NULL=n`** — prevents `while(1)` hang on alloc failure (would trip a 60s WDT reboot). With asserts off, NULL propagates and crashes faster with a useful backtrace.
- **`CONFIG_LV_USE_TJPGD=y`** + **`CONFIG_LV_USE_FS_MEMFS=y`** — TJPGD decoder for in-memory JPEG rendering (used by rich-media chat).
- **`LV_DISPLAY_RENDER_MODE_PARTIAL`** — two 144 KB draw buffers in PSRAM. Direct mode causes tearing on the DPI panel.

## 2. Never call `lv_async_call()` directly — use `tab5_lv_async_call()`

LVGL 9.x's `lv_async_call()` is **not thread-safe**: it does `lv_malloc` + `lv_timer_create` against unprotected TLSF. The codebase had been treating it as thread-safe for years per a wrong comment in `ui_core.c` — that's the origin of a long-running stability investigation.

Always use the wrapper:

```c
#include "ui_core.h"
tab5_lv_async_call(my_callback, my_arg);
```

The wrapper takes `tab5_ui_lock` before calling LVGL primitives. There are 49 sites in firmware that previously called `lv_async_call` directly; all were converted in PR #259. Don't add new direct callers.

## 3. Hide/show overlays. Don't destroy/create.

Internal SRAM (~512 KB) fragments under repeated overlay create/destroy cycles. Total free might look healthy but the largest contiguous block shrinks until allocations fail.

The fix: **create overlays once, then hide/show via `lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)` / `lv_obj_remove_flag(...)`**.

Settings, Chat, Voice all use this pattern. `dismiss_all_overlays()` calls `ui_chat_hide()`, `ui_settings_hide()`, etc. — never destroy.

Destroy only when permanently replacing (e.g. New Chat button purges the chat history and recreates the chat overlay from scratch).

## The third option: `lv_obj_clean()`

Some screens *need* their children rebuilt — the camera viewfinder is the canonical example. `lv_obj_clean()` is the third option between destroy/recreate and naive hide/show:

```c
lv_obj_clean(scr_camera);          // wipes children, keeps the screen object
// ... rebuild children ...
```

The PSRAM-backed canvas buffers stay resident across cleans. Allocation patterns stay stable. See `ui_camera.c` (Wave 17) for the worked example.

## Object lifetime + background tasks

LVGL objects must NOT be accessed from background tasks after a screen is destroyed. Pattern:

```c
static volatile bool s_destroying = false;

void my_screen_destroy(void) {
    s_destroying = true;
    lv_obj_clean(scr);
    // ... or lv_obj_delete(scr); s_destroying = false later ...
}

// in a background task or callback:
if (s_destroying) return;
lv_label_set_text(my_label, "...");
```

The `volatile` keyword is essential — the compiler will otherwise hoist the read out of the loop and miss the flip.

## FONT_* defines, not raw `lv_font_montserrat_XX`

`config.h` defines `FONT_TITLE`, `FONT_HEADING`, `FONT_BODY`, `FONT_SMALL`, `FONT_TINY`, `FONT_NAV`. Use these consistently — don't reach for `lv_font_montserrat_22` directly. Single source of truth for typography.

## Gesture debounce

The touch layer has a `ui_tap_gate` debounce gate (300 ms minimum gap) to suppress rapid-fire taps that previously caused crashes when overlapping animation lifecycles raced. Don't bypass it. If your callback needs to fire faster than 300 ms, you're probably misusing the framework.
