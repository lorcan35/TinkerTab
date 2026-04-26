/*
 * tool_log.{c,h} — Audit U7+U8 (#206)
 *
 * Single-process ring buffer of the last 16 tool_call / tool_result
 * events received from Dragon over the voice WS.  Lets ui_agents +
 * ui_focus render live activity instead of hand-curated demo rows.
 *
 * Producer: voice.c, on every "tool_call" / "tool_result" frame.
 * Consumers: ui_agents + ui_focus, on overlay show.
 *
 * Storage: PSRAM (lazy-allocated on first push) — the ring is ~3 KB,
 * which would tip the internal-SRAM budget if we put it in BSS (same
 * class of fault as ui_sessions per docs/STABILITY-INVESTIGATION.md
 * #185).  Reads from any task — single-writer, single-reader-per-call,
 * fields are read atomically per slot under a short FreeRTOS mutex.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define TOOL_LOG_CAP        16
#define TOOL_LOG_NAME_LEN   24
#define TOOL_LOG_DETAIL_LEN 80

typedef enum {
    TOOL_LOG_RUNNING = 0,   /* tool_call seen, no result yet */
    TOOL_LOG_DONE    = 1,   /* tool_result seen */
} tool_log_status_t;

typedef struct {
    char              name[TOOL_LOG_NAME_LEN];
    char              detail[TOOL_LOG_DETAIL_LEN];
    tool_log_status_t status;
    uint32_t          exec_ms;   /* 0 while running */
    time_t            started_at;
} tool_log_event_t;

/* Push a "tool started" event.  detail is a one-line human-readable
 * description (e.g. "Searching the web…") — pass NULL or "" to use
 * tool name verbatim. */
void tool_log_push_call(const char *name, const char *detail);

/* Mark the most-recent matching name as DONE with execution_ms.
 * If no matching RUNNING entry is found, append a synthetic DONE
 * row so the surface still shows what happened. */
void tool_log_push_result(const char *name, uint32_t exec_ms);

/* How many events are currently stored (0..TOOL_LOG_CAP). */
int  tool_log_count(void);

/* Snapshot copy of the event at index `idx_back` from newest:
 *  idx_back=0 → most recent, idx_back=count-1 → oldest.
 * Returns false if idx_back is out of range. */
bool tool_log_get(int idx_back, tool_log_event_t *out);
