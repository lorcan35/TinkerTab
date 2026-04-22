/*
 * task_worker.c — wave 14 W14-H06 shared job queue implementation.
 *
 * One persistent task on Core 1, 16 KB stack (matches the combined
 * worst case of the four job types: mode_switch_* + wifi_connect +
 * media_fetch + drawer_fetch).
 */

#include "task_worker.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "worker";

/* A single queue entry: function pointer + arg + short tag for the
 * trace log. */
typedef struct {
    tab5_worker_fn_t fn;
    void *arg;
    const char *tag;   /* stored as-is; must be a string literal or a
                          pointer that outlives the job — callers in
                          this repo pass literals from call sites. */
} job_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static bool s_inited = false;

static void worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "worker task started (queue depth=%d)",
             TAB5_WORKER_QUEUE_DEPTH);
    job_t job;
    for (;;) {
        /* Block indefinitely waiting for the next job. */
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;  /* spurious wakeup */
        }
        if (!job.fn) {
            ESP_LOGW(TAG, "skipped job with NULL fn (tag=%s)",
                     job.tag ? job.tag : "-");
            continue;
        }
        ESP_LOGD(TAG, "run job tag=%s", job.tag ? job.tag : "-");
        job.fn(job.arg);
        ESP_LOGD(TAG, "done job tag=%s", job.tag ? job.tag : "-");
    }
}

esp_err_t tab5_worker_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(TAB5_WORKER_QUEUE_DEPTH, sizeof(job_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }
    /* Wave 15 W15-C06: stack in PSRAM instead of internal SRAM.
     * The 16 KB stack was costing internal SRAM the UI/LVGL path
     * needs.  Worker jobs (mode_switch, wifi_connect, media_fetch,
     * drawer_fetch) run network / HTTP / JSON work — none are on
     * the LVGL render hot path, so the PSRAM access latency is
     * acceptable.  Other tasks in this codebase that already use
     * the same pattern: voice.c mic/detector/ingest and ui_memory.c.
     *
     * If the WithCaps variant fails (e.g. PSRAM exhausted which would
     * be bizarre — we have 21 MB), fall back to the internal-SRAM
     * create so the device still boots. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        worker_task, "tab5_worker",
        16384,
        NULL, 5, &s_task, 1,
        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "PSRAM stack alloc failed — falling back to internal");
        ok = xTaskCreatePinnedToCore(
            worker_task, "tab5_worker",
            16384,
            NULL, 5, &s_task, 1);
    }
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t tab5_worker_enqueue(tab5_worker_fn_t fn,
                              void *arg,
                              const char *tag)
{
    if (!s_inited || !s_queue) {
        ESP_LOGE(TAG, "enqueue(%s): worker not initialized",
                 tag ? tag : "-");
        return ESP_ERR_INVALID_STATE;
    }
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    job_t job = {.fn = fn, .arg = arg, .tag = tag};
    /* Non-blocking: if the queue is full, drop + log.  This is
     * preferable to blocking the caller (usually an LVGL event
     * handler) — jobs piling up past 16 indicates a dispatch
     * problem that should be visible in logs. */
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping job tag=%s",
                 tag ? tag : "-");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
