/**
 * TinkerTab — Debug Server Observability (#149 PR β)
 *
 * Bounded PSRAM ring buffers feeding the new debug endpoints:
 *   - Event stream      (/events)  — notable state transitions
 *   - Heap sample ring  (/heap/history) — 60 × 30 s samples
 *   - Log tail          (/logs/tail) — last N lines via esp_log vprintf hook
 *
 * Call tab5_debug_obs_init() once at boot after heap_watchdog.
 * All public reads are thread-safe (lock-taken); call sites are the
 * httpd task.  Writes from any task.
 */
#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdint.h>
#include <stddef.h>

/** Initialize the observability subsystem.  Registers the esp_log hook,
 *  allocates PSRAM ring buffers, kicks off the heap-sample timer. */
esp_err_t tab5_debug_obs_init(void);

/* ── Events ─────────────────────────────────────────────────────── */

/* Log a notable event.  Called from any task; cheap (one memcpy + incr).
 * Examples: "wifi.kick:hard", "voice.ws:connected", "mode.switch:2",
 *           "coredump.captured", "ota.scheduled". */
void tab5_debug_obs_event(const char *kind, const char *detail);

/** Serialize events newer than `since_ms` (monotonic uptime) into a
 *  JSON array.  Pass since_ms=0 for the whole ring. */
cJSON *tab5_debug_obs_events_json(uint64_t since_ms);

/* ── Heap history ───────────────────────────────────────────────── */

/** Serialize last `n` heap samples (most recent first).  n clamped
 *  to ring size (60).  Each entry: {ms, int_free, int_largest, psram,
 *  psram_largest, lvgl_used}. */
cJSON *tab5_debug_obs_heap_json(int n);

/* ── Log tail ───────────────────────────────────────────────────── */

/** Copy up to `n` most-recent log lines into a newly-allocated
 *  string buffer.  Caller frees with heap_caps_free.  Returns NULL
 *  on error. */
char *tab5_debug_obs_log_tail(int n, size_t *out_len);
