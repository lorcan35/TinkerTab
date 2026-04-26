/*
 * tool_log.c — see tool_log.h.
 */

#include "tool_log.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "tool_log";

/* Ring storage — lazy-allocated in PSRAM on first push (see header
 * for the BSS-budget reasoning).  s_head is the next write index;
 * s_count saturates at TOOL_LOG_CAP. */
static tool_log_event_t *s_ring  = NULL;
static int               s_head  = 0;
static int               s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

static bool ensure_ring(void)
{
    if (s_ring) return true;
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return false;
    }
    s_ring = heap_caps_calloc(TOOL_LOG_CAP, sizeof(*s_ring),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGW(TAG, "PSRAM alloc failed for tool_log ring");
        return false;
    }
    return true;
}

static void copy_in(tool_log_event_t *dst, const char *name,
                    const char *detail)
{
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->name, sizeof(dst->name), "%s",
             name && name[0] ? name : "tool");
    snprintf(dst->detail, sizeof(dst->detail), "%s",
             detail && detail[0] ? detail : dst->name);
    dst->started_at = time(NULL);
}

void tool_log_push_call(const char *name, const char *detail)
{
    if (!ensure_ring()) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    tool_log_event_t *slot = &s_ring[s_head];
    copy_in(slot, name, detail);
    slot->status  = TOOL_LOG_RUNNING;
    slot->exec_ms = 0;

    s_head = (s_head + 1) % TOOL_LOG_CAP;
    if (s_count < TOOL_LOG_CAP) s_count++;

    xSemaphoreGive(s_mutex);
}

void tool_log_push_result(const char *name, uint32_t exec_ms)
{
    if (!ensure_ring()) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    /* Walk newest → oldest and complete the first matching RUNNING. */
    bool matched = false;
    for (int i = 0; i < s_count && !matched; i++) {
        int idx = (s_head - 1 - i + TOOL_LOG_CAP) % TOOL_LOG_CAP;
        tool_log_event_t *e = &s_ring[idx];
        if (e->status == TOOL_LOG_RUNNING &&
            name && strncmp(e->name, name, sizeof(e->name)) == 0) {
            e->status  = TOOL_LOG_DONE;
            e->exec_ms = exec_ms;
            matched = true;
        }
    }
    if (!matched) {
        /* No matching RUNNING entry — synthesize a DONE row so the
         * surface still shows what just ran. */
        tool_log_event_t *slot = &s_ring[s_head];
        copy_in(slot, name, NULL);
        slot->status  = TOOL_LOG_DONE;
        slot->exec_ms = exec_ms;
        s_head = (s_head + 1) % TOOL_LOG_CAP;
        if (s_count < TOOL_LOG_CAP) s_count++;
    }

    xSemaphoreGive(s_mutex);
}

int tool_log_count(void)
{
    return s_count;
}

bool tool_log_get(int idx_back, tool_log_event_t *out)
{
    if (!s_ring || !out || idx_back < 0 || idx_back >= s_count) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    int idx = (s_head - 1 - idx_back + TOOL_LOG_CAP) % TOOL_LOG_CAP;
    *out = s_ring[idx];
    xSemaphoreGive(s_mutex);
    return true;
}
