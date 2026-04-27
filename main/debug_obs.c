/**
 * TinkerTab — Debug Server Observability (#149 PR β)
 *
 * Three bounded PSRAM ring buffers:
 *   - EVENT_RING:   64 events, each kind[16] + detail[48] + ms timestamp
 *   - HEAP_RING :   60 heap snapshots, one per 30 s
 *   - LOG_RING  :   32 KB line-oriented ring for /logs/tail
 *
 * Thread-safety: a single mutex guards mutator paths.  Readers copy
 * under the lock and release it before JSON-ifying to keep the
 * critical section tight.
 */

#include "debug_obs.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "lvgl.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "debug_obs";

/* ── Config ──────────────────────────────────────────────────────── */
#define EVENT_RING_SIZE   64
#define HEAP_RING_SIZE    60      /* 60 × 30 s = 30 min of history */
#define HEAP_SAMPLE_MS    30000   /* sampling cadence */
#define LOG_RING_SIZE     (32 * 1024)

/* ── Event ring ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t ms;
    /* #293: bumped from 16→32 because PR #294 added 19-char names
     * (camera.record_start) that were truncating to "camera.record_s". */
    char     kind[32];
    char     detail[48];
} obs_event_t;

static obs_event_t  *s_events = NULL;
static uint16_t      s_event_head = 0;   /* next slot to write */
static uint32_t      s_event_total = 0;  /* ever written (monotonic) */

/* ── Heap ring ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t ms;
    uint32_t int_free_kb;
    uint32_t int_largest_kb;
    uint32_t psram_free_kb;
    uint32_t psram_largest_kb;
    uint32_t lvgl_used_kb;
    uint32_t lvgl_free_kb;
    uint8_t  lvgl_frag_pct;
} obs_heap_t;

static obs_heap_t   *s_heap = NULL;
static uint16_t      s_heap_head = 0;
static uint16_t      s_heap_count = 0;   /* grows to HEAP_RING_SIZE then caps */

/* ── Log ring ───────────────────────────────────────────────────── */
/* Byte-wise circular buffer storing lines separated by '\n'.  Old
 * bytes are overwritten as new data arrives.  Tail extraction walks
 * backwards from head to count N lines. */
static char         *s_log = NULL;
static uint32_t      s_log_head = 0;     /* next byte to write */
static bool          s_log_wrapped = false;

static SemaphoreHandle_t s_mu = NULL;
static vprintf_like_t    s_prev_vprintf = NULL;
static bool              s_inited = false;

static esp_timer_handle_t s_heap_timer = NULL;

/* ── Helpers ─────────────────────────────────────────────────────── */

static inline uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static inline void lock(void)   { if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY); }
static inline void unlock(void) { if (s_mu) xSemaphoreGive(s_mu); }

/* ── esp_log vprintf hook ───────────────────────────────────────── */
/* The log hook runs on whatever task emitted the log.  We append to
 * the log ring then forward to the previous handler (UART). */
static int log_hook_vprintf(const char *fmt, va_list ap)
{
    /* Forward to previous handler first so UART logs aren't blocked
     * by ring contention.  Copy ap because vprintf consumes it. */
    va_list ap2;
    va_copy(ap2, ap);
    int rv = s_prev_vprintf ? s_prev_vprintf(fmt, ap2) : vprintf(fmt, ap2);
    va_end(ap2);

    /* Format into a stack buffer (cheap) then ring-append under lock. */
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0 || !s_log) return rv;
    if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;

    lock();
    for (int i = 0; i < n; i++) {
        s_log[s_log_head] = line[i];
        s_log_head = (s_log_head + 1) % LOG_RING_SIZE;
        if (s_log_head == 0) s_log_wrapped = true;
    }
    unlock();
    return rv;
}

/* ── Heap sample timer ──────────────────────────────────────────── */
static void heap_sample_cb(void *arg)
{
    (void)arg;
    if (!s_heap) return;

    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t int_lrg  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_lrg   = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    lv_mem_monitor_t lvgl;
    lv_mem_monitor(&lvgl);

    obs_heap_t s = {
        .ms               = now_ms(),
        .int_free_kb      = int_free / 1024,
        .int_largest_kb   = int_lrg  / 1024,
        .psram_free_kb    = ps_free  / 1024,
        .psram_largest_kb = ps_lrg   / 1024,
        .lvgl_used_kb     = (lvgl.total_size - lvgl.free_size) / 1024,
        .lvgl_free_kb     = lvgl.free_size / 1024,
        .lvgl_frag_pct    = (uint8_t)lvgl.frag_pct,
    };

    lock();
    s_heap[s_heap_head] = s;
    s_heap_head = (s_heap_head + 1) % HEAP_RING_SIZE;
    if (s_heap_count < HEAP_RING_SIZE) s_heap_count++;
    unlock();
}

/* ── Public: init ───────────────────────────────────────────────── */
esp_err_t tab5_debug_obs_init(void)
{
    if (s_inited) return ESP_OK;

    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) return ESP_ERR_NO_MEM;

    s_events = heap_caps_calloc(EVENT_RING_SIZE, sizeof(obs_event_t), MALLOC_CAP_SPIRAM);
    s_heap   = heap_caps_calloc(HEAP_RING_SIZE,  sizeof(obs_heap_t),  MALLOC_CAP_SPIRAM);
    s_log    = heap_caps_malloc(LOG_RING_SIZE,                       MALLOC_CAP_SPIRAM);
    if (!s_events || !s_heap || !s_log) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        if (s_events) heap_caps_free(s_events);
        if (s_heap)   heap_caps_free(s_heap);
        if (s_log)    heap_caps_free(s_log);
        s_events = NULL; s_heap = NULL; s_log = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_log, 0, LOG_RING_SIZE);

    /* Install log hook — keep the previous one so UART output survives. */
    s_prev_vprintf = esp_log_set_vprintf(log_hook_vprintf);

    /* Heap sample timer — runs in the esp_timer service task. */
    const esp_timer_create_args_t targs = {
        .callback        = heap_sample_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "obs_heap",
    };
    esp_err_t r = esp_timer_create(&targs, &s_heap_timer);
    if (r == ESP_OK) {
        esp_timer_start_periodic(s_heap_timer, HEAP_SAMPLE_MS * 1000ULL);
    }

    /* Seed the first sample now so /heap/history isn't empty on boot. */
    heap_sample_cb(NULL);

    s_inited = true;
    tab5_debug_obs_event("obs", "init");
    ESP_LOGI(TAG, "observability ready: events=%d heap=%d log=%uKB",
             EVENT_RING_SIZE, HEAP_RING_SIZE, (unsigned)(LOG_RING_SIZE / 1024));
    return ESP_OK;
}

/* ── Public: event API ──────────────────────────────────────────── */
void tab5_debug_obs_event(const char *kind, const char *detail)
{
    if (!s_events) return;
    lock();
    obs_event_t *e = &s_events[s_event_head];
    e->ms = now_ms();
    snprintf(e->kind,   sizeof(e->kind),   "%s", kind   ? kind   : "");
    snprintf(e->detail, sizeof(e->detail), "%s", detail ? detail : "");
    s_event_head = (s_event_head + 1) % EVENT_RING_SIZE;
    s_event_total++;
    unlock();
}

cJSON *tab5_debug_obs_events_json(uint64_t since_ms)
{
    cJSON *arr = cJSON_CreateArray();
    if (!s_events || !arr) return arr;

    /* Copy under lock to minimize the critical section. */
    obs_event_t snap[EVENT_RING_SIZE];
    uint32_t total;
    uint16_t head;
    lock();
    memcpy(snap, s_events, sizeof(snap));
    head  = s_event_head;
    total = s_event_total;
    unlock();

    /* Walk oldest → newest. */
    int count = total < EVENT_RING_SIZE ? (int)total : EVENT_RING_SIZE;
    for (int i = 0; i < count; i++) {
        int idx = (head + EVENT_RING_SIZE - count + i) % EVENT_RING_SIZE;
        const obs_event_t *e = &snap[idx];
        if (e->ms < since_ms) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ms",     (double)e->ms);
        cJSON_AddStringToObject(o, "kind",   e->kind);
        cJSON_AddStringToObject(o, "detail", e->detail);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* ── Public: heap history ───────────────────────────────────────── */
cJSON *tab5_debug_obs_heap_json(int n)
{
    cJSON *arr = cJSON_CreateArray();
    if (!s_heap || !arr) return arr;
    if (n <= 0 || n > HEAP_RING_SIZE) n = HEAP_RING_SIZE;

    obs_heap_t snap[HEAP_RING_SIZE];
    uint16_t count, head;
    lock();
    memcpy(snap, s_heap, sizeof(snap));
    count = s_heap_count;
    head  = s_heap_head;
    unlock();

    if (n > count) n = count;
    /* Most recent first — walk head-1, head-2, ... */
    for (int i = 0; i < n; i++) {
        int idx = (head - 1 - i + HEAP_RING_SIZE) % HEAP_RING_SIZE;
        const obs_heap_t *s = &snap[idx];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ms",               (double)s->ms);
        cJSON_AddNumberToObject(o, "int_free_kb",      s->int_free_kb);
        cJSON_AddNumberToObject(o, "int_largest_kb",   s->int_largest_kb);
        cJSON_AddNumberToObject(o, "psram_free_kb",    s->psram_free_kb);
        cJSON_AddNumberToObject(o, "psram_largest_kb", s->psram_largest_kb);
        cJSON_AddNumberToObject(o, "lvgl_used_kb",     s->lvgl_used_kb);
        cJSON_AddNumberToObject(o, "lvgl_free_kb",     s->lvgl_free_kb);
        cJSON_AddNumberToObject(o, "lvgl_frag_pct",    s->lvgl_frag_pct);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* ── Public: log tail ───────────────────────────────────────────── */
char *tab5_debug_obs_log_tail(int n, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!s_log) return NULL;
    if (n <= 0) n = 100;

    /* Snapshot — small enough that copying the whole ring is cheap. */
    char *snap = heap_caps_malloc(LOG_RING_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (!snap) return NULL;
    uint32_t head;
    bool wrapped;
    lock();
    memcpy(snap, s_log, LOG_RING_SIZE);
    head    = s_log_head;
    wrapped = s_log_wrapped;
    unlock();
    snap[LOG_RING_SIZE] = '\0';

    /* Logical-linear view: if wrapped, ring is [head .. end] + [0 .. head-1]
     * Otherwise: [0 .. head-1]. */
    size_t linear_len = wrapped ? LOG_RING_SIZE : head;
    char *linear = heap_caps_malloc(linear_len + 1, MALLOC_CAP_SPIRAM);
    if (!linear) { heap_caps_free(snap); return NULL; }
    if (wrapped) {
        size_t tail_len = LOG_RING_SIZE - head;
        memcpy(linear, snap + head, tail_len);
        memcpy(linear + tail_len, snap, head);
    } else {
        memcpy(linear, snap, head);
    }
    linear[linear_len] = '\0';
    heap_caps_free(snap);

    /* Walk from the tail backwards to count N '\n' separators. */
    size_t start = linear_len;
    int lines_seen = 0;
    while (start > 0 && lines_seen <= n) {
        start--;
        if (linear[start] == '\n') {
            lines_seen++;
            if (lines_seen > n) { start++; break; }
        }
    }

    size_t out_bytes = linear_len - start;
    char *out = heap_caps_malloc(out_bytes + 1, MALLOC_CAP_SPIRAM);
    if (!out) { heap_caps_free(linear); return NULL; }
    memcpy(out, linear + start, out_bytes);
    out[out_bytes] = '\0';
    heap_caps_free(linear);

    if (out_len) *out_len = out_bytes;
    return out;
}
