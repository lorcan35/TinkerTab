/**
 * TinkerTab — LVGL pool-pressure probe (Phase 1 instrumentation).
 * See pool_probe.h for rationale.
 */
#include "pool_probe.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui_core.h"

static const char *TAG = "pool_probe";

#define SAMPLE_COUNT   900     /* 15 min @ 1 Hz */
#define SAMPLE_PERIOD_MS 1000

typedef struct {
    uint32_t ms;                    /* esp_timer_get_time / 1000 */
    uint32_t internal_free_kb;
    uint32_t internal_largest_kb;
    uint32_t dma_free_kb;
    uint32_t dma_largest_kb;
    uint32_t psram_free_kb;
    uint32_t psram_largest_kb;
    uint32_t lvgl_total_kb;         /* from lv_mem_monitor, under tab5_ui_lock */
    uint32_t lvgl_used_kb;
    uint32_t lvgl_free_kb;
    uint8_t  lvgl_frag_pct;
} pool_sample_t;

static pool_sample_t *s_ring = NULL;    /* PSRAM, SAMPLE_COUNT entries */
static atomic_uint    s_head = 0;       /* next write index, wraps */
static atomic_uint    s_count = 0;      /* saturates at SAMPLE_COUNT */

/* Alloc-failure log is a separate, smaller ring — 64 entries. */
typedef struct {
    uint32_t ms;
    uint32_t requested_bytes;
    uint32_t caps;
    uint32_t internal_largest_at_failure_kb;
} alloc_fail_t;

#define FAIL_COUNT 64
static alloc_fail_t *s_fails = NULL;
static atomic_uint   s_fail_head = 0;
static atomic_uint   s_fail_count = 0;

static void fail_callback(size_t requested_size, uint32_t caps, const char *function_name)
{
    if (!s_fails) return;
    uint32_t idx = atomic_fetch_add(&s_fail_head, 1) % FAIL_COUNT;
    s_fails[idx].ms                             = (uint32_t)(esp_timer_get_time() / 1000);
    s_fails[idx].requested_bytes                = requested_size;
    s_fails[idx].caps                           = caps;
    s_fails[idx].internal_largest_at_failure_kb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
    atomic_fetch_add(&s_fail_count, 1);
    ESP_LOGE(TAG, "alloc FAIL %zu B caps=0x%08lx from %s (internal_largest=%luKB)",
             requested_size, (unsigned long)caps,
             function_name ? function_name : "?",
             (unsigned long)s_fails[idx].internal_largest_at_failure_kb);
}

static void sampler_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "sampler task running (1 Hz, ring=%d samples)", SAMPLE_COUNT);
    for (;;) {
        pool_sample_t s = {0};
        s.ms                  = (uint32_t)(esp_timer_get_time() / 1000);
        s.internal_free_kb    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
        s.internal_largest_kb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
        s.dma_free_kb         = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024;
        s.dma_largest_kb      = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024;
        s.psram_free_kb       = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
        s.psram_largest_kb    = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024;

        /* Phase 3-B: take the LVGL mutex so the monitor snapshot is
         * consistent.  Previous unlocked reads returned torn total/free
         * pairs (Phase 1 "used stuck at 14 KB" artifact).  Use try_lock
         * with a short timeout so a stuck UI task can't block the probe
         * entirely; on timeout we emit zeros and continue sampling. */
        lv_mem_monitor_t mon = {0};
        if (tab5_ui_try_lock(20)) {
            lv_mem_monitor(&mon);
            tab5_ui_unlock();
            s.lvgl_total_kb = mon.total_size / 1024;
            s.lvgl_used_kb = (mon.total_size - mon.free_size) / 1024;
            s.lvgl_free_kb = mon.free_size / 1024;
            s.lvgl_frag_pct = (uint8_t)mon.frag_pct;
        }

        uint32_t idx = atomic_fetch_add(&s_head, 1) % SAMPLE_COUNT;
        s_ring[idx] = s;
        uint32_t c = atomic_load(&s_count);
        if (c < SAMPLE_COUNT) atomic_store(&s_count, c + 1);

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

esp_err_t tab5_pool_probe_init(void)
{
    if (s_ring) return ESP_OK;  /* idempotent */

    s_ring = heap_caps_calloc(SAMPLE_COUNT, sizeof(pool_sample_t), MALLOC_CAP_SPIRAM);
    s_fails = heap_caps_calloc(FAIL_COUNT, sizeof(alloc_fail_t), MALLOC_CAP_SPIRAM);
    if (!s_ring || !s_fails) {
        ESP_LOGE(TAG, "probe buffer alloc failed — probe disabled");
        heap_caps_free(s_ring);
        heap_caps_free(s_fails);
        s_ring = NULL;
        s_fails = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = heap_caps_register_failed_alloc_callback(fail_callback);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register_failed_alloc_callback returned %s", esp_err_to_name(err));
    }

    BaseType_t r = xTaskCreatePinnedToCore(
        sampler_task, "pool_probe", 3072, NULL, 1, NULL, 1);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "sampler task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "initialized (samples=%d, fails=%d)", SAMPLE_COUNT, FAIL_COUNT);
    return ESP_OK;
}

esp_err_t tab5_pool_probe_http_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!s_ring) {
        httpd_resp_sendstr(req, "probe not initialised\n");
        return ESP_OK;
    }

    httpd_resp_sendstr_chunk(req,
        "# Samples\n"
        "ms,internal_free_kb,internal_largest_kb,dma_free_kb,dma_largest_kb,"
        "psram_free_kb,psram_largest_kb,"
        "lvgl_total_kb,lvgl_used_kb,lvgl_free_kb,lvgl_frag_pct\n");

    uint32_t count = atomic_load(&s_count);
    uint32_t head  = atomic_load(&s_head);
    /* If we've wrapped, oldest sample is at head; otherwise at 0. */
    uint32_t start = (count < SAMPLE_COUNT) ? 0 : head;
    char line[176];
    for (uint32_t i = 0; i < count; i++) {
        const pool_sample_t *s = &s_ring[(start + i) % SAMPLE_COUNT];
        int n = snprintf(line, sizeof(line),
            "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
            ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u\n",
            s->ms,
            s->internal_free_kb, s->internal_largest_kb,
            s->dma_free_kb, s->dma_largest_kb,
            s->psram_free_kb, s->psram_largest_kb,
            s->lvgl_total_kb,
            s->lvgl_used_kb, s->lvgl_free_kb, s->lvgl_frag_pct);
        if (n > 0) httpd_resp_send_chunk(req, line, n);
    }

    httpd_resp_sendstr_chunk(req, "\n# AllocFailures\n"
                                   "ms,requested_bytes,caps,internal_largest_kb\n");
    uint32_t fc = atomic_load(&s_fail_count);
    if (fc > FAIL_COUNT) fc = FAIL_COUNT;
    uint32_t fhead = atomic_load(&s_fail_head);
    uint32_t fstart = (atomic_load(&s_fail_count) < FAIL_COUNT) ? 0 : fhead;
    for (uint32_t i = 0; i < fc; i++) {
        const alloc_fail_t *f = &s_fails[(fstart + i) % FAIL_COUNT];
        int n = snprintf(line, sizeof(line),
            "%" PRIu32 ",%" PRIu32 ",0x%08" PRIx32 ",%" PRIu32 "\n",
            f->ms, f->requested_bytes, f->caps,
            f->internal_largest_at_failure_kb);
        if (n > 0) httpd_resp_send_chunk(req, line, n);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
