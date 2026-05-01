---
title: Stability guide
sidebar_label: Stability guide
---

# Stability guide

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

The rules that keep TinkerTab booting reliably under stress. Hard-won. Each rule has a real bug behind it.

## Rule 1 — BSS-static caches >1.8 KB push the boot timer task over its SRAM canary

Symptom: `vApplicationGetTimerTaskMemory: pxStackBufferTemp != NULL` assert at boot. MCAUSE 0x1b on ESP32-P4 RISC-V = **stack protection fault** (per-task canary).

Why: FreeRTOS allocates the timer service task stack from internal SRAM at boot. Any large BSS-static allocation (a struct, a buffer, a cache) at file scope competes for the same SRAM. Above ~1.8 KB the timer-task allocation fails — *silently* — and the next call into the timer service trips the canary.

Fix: **Default to PSRAM-lazy.** Replace BSS structs with pointers initialized lazily on first use:

```c
// bad
static skills_payload_t s_kept_payload;        // 3.6 KB BSS — boot loops

// good
static skills_payload_t *s_kept_payload = NULL;  // pointer, no BSS
// ...lazy alloc on first use:
if (!s_kept_payload) {
    s_kept_payload = heap_caps_calloc(1, sizeof(*s_kept_payload),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
```

Same rule for any moderately-sized cache, lookup table, etc.

## Rule 2 — Never `xTimerCreate` from boot — use `esp_timer`

Symptom: same as Rule 1 (MCAUSE 0x1b at boot).

Why: `xTimerCreate` requires the FreeRTOS timer service task. The timer service task allocates a 16 KB internal SRAM stack on first use. Boot pressure means that stack alloc can fail — same canary trip.

`esp_timer`, by contrast, runs callbacks on a **single global dispatcher task** that's already created during the IDF init. Allocating an `esp_timer_handle_t` is ~16 bytes of struct, no extra task stack.

```c
// bad — can boot loop
TimerHandle_t t = xTimerCreate("retry", pdMS_TO_TICKS(60000), pdFALSE, NULL, cb);
xTimerStart(t, 0);

// good
esp_timer_handle_t t = NULL;
const esp_timer_create_args_t cfg = {
    .callback = &cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "retry",
};
esp_timer_create(&cfg, &t);
esp_timer_start_once(t, 60000 * 1000ULL);
```

## Rule 3 — Hide/show overlays, don't destroy/create

Symptom: random allocation failures hours into a session. `heap_caps_get_free_size()` shows healthy memory but `heap_caps_get_largest_free_block()` is below 30 KB.

Why: internal SRAM fragments under repeated create/destroy of overlays (Settings, Chat, Voice). Total free stays high, but the largest contiguous block shrinks below the size of the next overlay.

Fix: create overlays once at boot, hide/show via `LV_OBJ_FLAG_HIDDEN`. There's a fragmentation watchdog (`heap_watchdog.c`) that triggers a controlled reboot if the largest internal SRAM block stays under 30 KB for 3 minutes — but the primary fix is the hide/show pattern. See [LVGL conventions](/docs/firmware-reference/lvgl-conventions).

## Rule 4 — All LVGL `lv_async_call` MUST go through `tab5_lv_async_call`

Symptom: random crashes in `lv_malloc` after high-frequency screen transitions.

Why: LVGL 9.x's `lv_async_call` is not thread-safe — it does `lv_malloc` + `lv_timer_create` against unprotected TLSF. Calling it from a non-LVGL task races against the LVGL render thread.

Fix: always use the wrapper:

```c
#include "ui_core.h"
tab5_lv_async_call(my_callback, my_arg);  // takes tab5_ui_lock
```

Don't reintroduce direct `lv_async_call` from background tasks.

## Rule 5 — Big buffers (>4 KB) MUST go in PSRAM

Symptom: silent boot failures, OOM under stress.

Why: internal SRAM (~512 KB) is shared with FreeRTOS task stacks, framework drivers, LVGL BSS pool. PSRAM (32 MB) is plentiful.

Fix:

```c
void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
// ...
heap_caps_free(p);   // NOT plain free()!
```

For task stacks, prefer `xTaskCreatePinnedToCoreWithCaps` with `MALLOC_CAP_SPIRAM` so the stack itself is in PSRAM.

## Rule 6 — `vTaskDelete(NULL)` is broken on ESP32-P4

Don't call `vTaskDelete(NULL)` to self-delete a task — there's a P4-specific TLSP cleanup crash. Use `vTaskSuspend(NULL)` instead and let the task supervisor reap it on shutdown.

## Rule 7 — Network + LVGL callbacks need 8 KB+ stack

Any FreeRTOS task that handles network IO AND calls into LVGL needs a generous stack. The default 4 KB is not enough.

```c
xTaskCreatePinnedToCoreWithCaps(
    my_task_fn, "my_task", 8192,        // 8 KB minimum
    NULL, 5, NULL, 0,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
);
```

Voice WS task is at 8 KB. Mic task was at 8 KB until Wave 19 bumped it to 32 KB after the SILK NSQ stack canary trip. Pick generously.

## Rule 8 — sdkconfig changes always require `idf.py fullclean build`

Incremental builds cache stale config. If you change `sdkconfig.defaults` (LVGL settings, partition table, anything) and don't fullclean, you'll get baffling behavior because old config bits leak into the new build.

```bash
idf.py fullclean build
```

When in doubt, fullclean. It's not that slow.
