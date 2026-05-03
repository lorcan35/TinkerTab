/**
 * TinkerTab — HTTP Debug Server
 *
 * Serves screenshots (BMP), device info, touch injection, and a live
 * remote-control HTML page over HTTP on port 8080.
 *
 * The BMP screenshot reads the DPI framebuffer (720x1280 RGB565 in PSRAM),
 * flushes the CPU cache first (esp_cache_msync M2C) so we see the latest
 * DMA data, then streams a bottom-up BMP with RGB565 bitmasks.
 */

#include "debug_server.h"

#include <dirent.h>
#include <math.h> /* TT #264 sin() for OPUS encoder synthetic test */
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "battery.h"
#include "cJSON.h"
#include "camera.h"
#include "chat_msg_store.h" /* Audit B2 (#202): chat_store_evictions_total */
#include "config.h"
#include "display.h"
#include "driver/jpeg_encode.h"
#include "esp_cache.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/atomic.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "ota.h"
#include "pool_probe.h"
#include "sdcard.h"
#include "settings.h"
#include "task_worker.h"
#include "ui_camera.h"
#include "ui_chat.h"
#include "ui_core.h"
#include "ui_files.h"
#include "ui_home.h"
#include "ui_keyboard.h"
#include "ui_settings.h"
#include "ui_wifi.h"
#include "voice.h"
/* Wave 23b (#332): per-family endpoint extracts + auth shim. */
#include "debug_server_call.h"
#include "debug_server_codec.h"
#include "debug_server_dictation.h"
#include "debug_server_internal.h"
#include "debug_server_m5.h"
#include "debug_server_mode.h"
#include "debug_server_ota.h"
#include "debug_server_settings.h"
#include "debug_server_voice.h"
#include "debug_server_wifi.h"
#include "widget.h" /* Audit C4 (#202): widget_store_evictions_total */
#include "wifi.h"

static const char *TAG = "debug_srv";

#define DEBUG_PORT 8080

/* Wave 15 W15-C02: file-scoped httpd handle so `tab5_debug_server_stop()`
 * can actually stop the server on demand.  The original code stashed the
 * handle in a function-local and leaked it — making the server a one-shot
 * with no recovery path. */
static httpd_handle_t s_httpd = NULL;

/* #74: TCP_NODELAY on every accepted socket.  Default Nagle batches
 * sub-MSS sends until either the buffer fills or the 200 ms tail
 * timer fires; for small JSON responses (/info / /touch / /heap)
 * that interacts badly with a chunked stream from another in-flight
 * handler and adds visible latency.  Run at accept time so all
 * subsequent send()s on the socket flush immediately. */
esp_err_t debug_open_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    int yes = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0) {
        ESP_LOGW(TAG, "TCP_NODELAY set failed on fd=%d", sockfd);
    }
    return ESP_OK;
}

/* ======================================================================== */
/*  Bearer token authentication                                              */
/* ======================================================================== */
static char s_auth_token[33] = {0}; /* 32 hex chars + null */

static void init_auth_token(void)
{
    /* Try reading from NVS */
    if (tab5_settings_get_auth_token(s_auth_token, sizeof(s_auth_token)) == ESP_OK && s_auth_token[0]) {
        ESP_LOGI(TAG, "Auth token loaded from NVS");
    } else {
        /* Generate new random token */
        uint32_t r[4];
        for (int i = 0; i < 4; i++) r[i] = esp_random();
        snprintf(s_auth_token, sizeof(s_auth_token), "%08lx%08lx%08lx%08lx",
                 (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2], (unsigned long)r[3]);
        tab5_settings_set_auth_token(s_auth_token);
        ESP_LOGI(TAG, "Generated new auth token");
    }
    /* v4·D audit P1 fix: don't dump the full token on every boot log.
     * The user can still recover it via serial by reading NVS, but
     * anyone over-shoulder-watching the console doesn't get it. */
    size_t tl = strlen(s_auth_token);
    ESP_LOGI(TAG, "Debug server auth token: %.*s****%.*s (%u chars, masked)",
             (int)(tl >= 4 ? 4 : tl), s_auth_token,
             (int)(tl >= 4 ? 4 : 0), s_auth_token + (tl >= 4 ? tl - 4 : 0),
             (unsigned)tl);
}

/* v4·D audit P1 fix: constant-time string compare for auth.  The
 * previous strcmp leaked information about the prefix match length
 * via response timing -- a local attacker on the LAN could use that
 * to narrow down the 32-hex-char token with O(16) probes per position.
 * This path iterates ALL bytes regardless of match -- ~100 ns extra. */
static int ct_token_cmp(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n  = la > lb ? la : lb;
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned)(ca ^ cb);
    }
    return diff == 0 ? 0 : 1;
}

/**
 * Check Authorization: Bearer <token> header.
 * Returns true if authorized, false if 401 was sent.
 */
static bool check_auth(httpd_req_t *req)
{
    char auth_header[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Authorization required\"}");
        return false;
    }
    /* Expect "Bearer <token>" */
    if (strncmp(auth_header, "Bearer ", 7) != 0
        || ct_token_cmp(auth_header + 7, s_auth_token) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid token\"}");
        return false;
    }
    return true;
}

/* Wave 23b (#332): public wrapper for the per-family extracted modules
 * (debug_server_m5.c, …) — see debug_server_internal.h.  The historic
 * 65 in-tree call sites still use the static `check_auth` to keep the
 * extraction diff minimal; new family files go through this wrapper. */
bool tab5_debug_check_auth(httpd_req_t *req) { return check_auth(req); }

/* Settings family extract (Wave 23b): the /nvs/erase factory-reset
 * guard compares ?confirm=<token> against the first 8 chars of the
 * file-static `s_auth_token`.  Expose just the comparison so the
 * settings family doesn't need access to the secret itself.
 *
 * Use memcmp on a fixed prefix length so we don't trip
 * -Werror=stringop-truncation (which fires on strncpy when the copy
 * length matches the destination buffer size) and so we don't depend
 * on the caller having null-terminated their query parameter beyond
 * the first 8 chars. */
bool tab5_debug_check_factory_reset_confirm(const char *confirm) {
   if (!confirm || strlen(confirm) < 8) return false;
   return memcmp(confirm, s_auth_token, 8) == 0;
}

/* ======================================================================== */
/*  Touch injection state — read by the LVGL indev read callback             */
/* ======================================================================== */
/* TT #328 Wave 2 — atomic-CAS read.  Pre-Wave-2 the four globals were read
 * independently in tab5_debug_touch_override; the LVGL thread could observe
 * a torn snapshot mid-write from the HTTP handler thread (e.g. swipe step
 * lands x=midpoint while pressed=false from the previous release).  Story
 * #294 + audit P0 #3 traced flaky story_onboard step 9 ("inject tap on orb
 * 100 ms after /navigate to chat") to exactly this race.
 *
 * Fix: seqlock pattern.  Writer increments seq twice around the four-field
 * publish (odd = mid-write, even = published).  Reader spins until seq is
 * even AND unchanged across the field read — guarantees a self-consistent
 * snapshot without taking a mutex on the LVGL hot path. */
typedef struct {
   int32_t x;
   int32_t y;
   bool active;
   bool pressed;
} debug_inject_state_t;

static debug_inject_state_t s_inject_state = {0};
static _Atomic uint32_t s_inject_seq = 0; /* odd while writing, even when stable */

static inline void inject_publish(int32_t x, int32_t y, bool active, bool pressed) {
   /* Begin write — make seq odd. */
   atomic_fetch_add_explicit(&s_inject_seq, 1, memory_order_relaxed);
   atomic_thread_fence(memory_order_release);
   s_inject_state.x = x;
   s_inject_state.y = y;
   s_inject_state.active = active;
   s_inject_state.pressed = pressed;
   atomic_thread_fence(memory_order_release);
   /* End write — make seq even. */
   atomic_fetch_add_explicit(&s_inject_seq, 1, memory_order_relaxed);
}

bool tab5_debug_touch_override(int32_t *x, int32_t *y, bool *pressed)
{
   debug_inject_state_t snap = {0};
   uint32_t seq_before = 0, seq_after = 0;
   /* Spin until seq is even (writer not in progress) and stable across read. */
   do {
      seq_before = atomic_load_explicit(&s_inject_seq, memory_order_acquire);
      if (seq_before & 1u) continue; /* writer mid-publish */
      snap = s_inject_state;
      atomic_thread_fence(memory_order_acquire);
      seq_after = atomic_load_explicit(&s_inject_seq, memory_order_relaxed);
   } while (seq_before != seq_after);

   if (!snap.active) return false;
   *x = snap.x;
   *y = snap.y;
   *pressed = snap.pressed;
   return true;
}

#define FB_W       TAB5_DISPLAY_WIDTH   /* 720  */
#define FB_H       TAB5_DISPLAY_HEIGHT  /* 1280 */
#define FB_BPP     2                    /* RGB565 = 2 bytes/pixel */

/* #148: the build_bmp_header helper was deleted — /screenshot is JPEG-only
 * (hardware encoder) and the /camera BMP path was retired in the same
 * pass.  If BMP comes back, revive from git history. */

/* ======================================================================== */
/*  GET /screenshot                                                          */
/* ======================================================================== */

/* Lazily-initialised hardware JPEG encoder. One engine is reused across
 * requests, serialised by s_jpeg_mux because jpeg_encoder_process is not
 * safe to share across concurrent callers. */
#include "freertos/semphr.h"
static jpeg_encoder_handle_t s_jpeg_enc = NULL;
static SemaphoreHandle_t     s_jpeg_mux = NULL;

static esp_err_t _ensure_jpeg_encoder(void)
{
    if (s_jpeg_enc) return ESP_OK;
    if (!s_jpeg_mux) {
        s_jpeg_mux = xSemaphoreCreateMutex();
        if (!s_jpeg_mux) return ESP_ERR_NO_MEM;
    }
    jpeg_encode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms    = 5000,
    };
    esp_err_t ret = jpeg_new_encoder_engine(&cfg, &s_jpeg_enc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(ret));
        s_jpeg_enc = NULL;
    }
    return ret;
}

/* #74: atomic busy-guard + async dispatch.
 *
 * Two layers of protection so /screenshot can't wedge the debug
 * server while it encodes + sends:
 *
 *   1. CAS busy flag: a second concurrent /screenshot request returns
 *      429 immediately instead of queueing.  Cheap, race-free.
 *
 *   2. Async handler: the entire encode + send happens on a spawned
 *      task via httpd_req_async_handler_begin, so the dispatch worker
 *      is freed within microseconds.  Other requests (/info /touch
 *      /heap /navigate) keep flowing even while a screenshot is mid-
 *      send over a slow WiFi link.  Dedicated task is fine because
 *      the busy-guard ensures at most one screenshot task ever exists.
 *
 *   3. send_wait_timeout dropped 90→15 s in tab5_debug_server_start.
 *      So a stuck client gets dropped fast, not in 90 s.
 */
static volatile uint32_t s_screenshot_busy = 0;
static esp_err_t screenshot_handler_inner(httpd_req_t *req);

static void screenshot_async_task(void *arg)
{
    httpd_req_t *async_req = (httpd_req_t *)arg;
    if (async_req) {
        screenshot_handler_inner(async_req);
        httpd_req_async_handler_complete(async_req);
    }
    /* Release the busy flag here, AFTER complete, so a follow-up
     * client can't squeeze in while the previous send is still
     * draining the kernel buffer. */
    Atomic_Decrement_u32(&s_screenshot_busy);
    /* #254: returns instead of vTaskSuspend(NULL).  This runs on the
     * shared tab5_worker now (see screenshot_handler), so the worker
     * task picks up the next job. */
}

static esp_err_t screenshot_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    if (Atomic_CompareAndSwap_u32(&s_screenshot_busy, 1, 0) != ATOMIC_COMPARE_AND_SWAP_SUCCESS) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"screenshot_in_flight\","
                                 "\"hint\":\"retry after current encode completes (~200-500ms)\"}");
        return ESP_OK;
    }
    /* Detach the request from the dispatch worker.  The kernel-side
     * socket is "checked out" to us; the worker returns to picking up
     * the next request immediately. */
    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        Atomic_Decrement_u32(&s_screenshot_busy);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "async handler begin failed");
        return ESP_FAIL;
    }
    /* #254: hand the JPEG encode + send to the shared tab5_worker.
     * Was a per-call xTaskCreate + vTaskSuspend(NULL) — even with
     * #247's WithCaps(SPIRAM) it accumulated ~870 B of internal-SRAM
     * task-list bookkeeping per call (+1 task per /screenshot).  The
     * busy-CAS above guarantees only one screenshot job is ever
     * queued, so we don't need a dedicated screenshot task.  Inline
     * fallback if the worker queue is full. */
    if (tab5_worker_enqueue(screenshot_async_task, async_req,
                            "screenshot_async") != ESP_OK) {
        ESP_LOGW(TAG, "screenshot worker enqueue failed; running inline");
        screenshot_handler_inner(async_req);
        httpd_req_async_handler_complete(async_req);
        Atomic_Decrement_u32(&s_screenshot_busy);
    }
    return ESP_OK;
}

static esp_err_t screenshot_handler_inner(httpd_req_t *req)
{
    esp_lcd_panel_handle_t panel = tab5_display_get_panel();
    if (!panel) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Display not initialized");
        return ESP_FAIL;
    }

    void *fb = NULL;
    esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
    if (ret != ESP_OK || !fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot get framebuffer");
        return ESP_FAIL;
    }

    if (_ensure_jpeg_encoder() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encoder init failed");
        return ESP_FAIL;
    }

    size_t fb_size = FB_W * FB_H * FB_BPP;

    /* DMA-aligned input buffer for the JPEG engine. */
    jpeg_encode_memory_alloc_cfg_t in_alloc = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    size_t in_capacity = 0;
    uint8_t *in_buf = jpeg_alloc_encoder_mem(fb_size, &in_alloc, &in_capacity);
    if (!in_buf || in_capacity < fb_size) {
        if (in_buf) free(in_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG input alloc failed");
        return ESP_FAIL;
    }

    /* Output buffer — 256 KB is plenty for a 720x1280 UI screenshot at q80
     * (measured ~60-120 KB for dark-UI content). */
    const size_t out_cap = 256 * 1024;
    jpeg_encode_memory_alloc_cfg_t out_alloc = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t out_capacity = 0;
    uint8_t *out_buf = jpeg_alloc_encoder_mem(out_cap, &out_alloc, &out_capacity);
    if (!out_buf) {
        free(in_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG output alloc failed");
        return ESP_FAIL;
    }

    /* HW03: Lock LVGL for the copy, unlock before encode (encode is long
     * but operates on our copy, so LVGL is free to keep rendering). */
    if (!tab5_ui_try_lock(2000)) {
        ESP_LOGW(TAG, "Screenshot: LVGL lock timeout (2s) — returning 503");
        free(in_buf); free(out_buf);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"display busy — LVGL lock timeout\"}");
        return ESP_OK;
    }
    esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(in_buf, fb, fb_size);
    tab5_ui_unlock();

    /* Encode. Serialize concurrent /screenshot calls — the engine is not
     * safe to share across simultaneous jpeg_encoder_process invocations. */
    jpeg_encode_cfg_t enc_cfg = {
        .height        = FB_H,
        .width         = FB_W,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = 80,
    };
    uint32_t out_size = 0;
    xSemaphoreTake(s_jpeg_mux, portMAX_DELAY);
    ret = jpeg_encoder_process(s_jpeg_enc, &enc_cfg, in_buf, fb_size,
                               out_buf, out_capacity, &out_size);
    xSemaphoreGive(s_jpeg_mux);

    free(in_buf);

    if (ret != ESP_OK || out_size == 0) {
        ESP_LOGE(TAG, "jpeg_encoder_process: %s out=%u", esp_err_to_name(ret), (unsigned)out_size);
        free(out_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encode failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    /* Typical payload 60-150 KB — fits comfortably in a single send with
     * the 32 KB LWIP window. No chunking needed. */
    ret = httpd_resp_send(req, (const char *)out_buf, out_size);
    free(out_buf);
    return ret;
}

/* ======================================================================== */
/*  GET /info                                                                */
/* ======================================================================== */

static esp_err_t get_wifi_ip(char *buf, size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        snprintf(buf, len, "0.0.0.0");
        return ESP_FAIL;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
        return ESP_OK;
    }
    snprintf(buf, len, "0.0.0.0");
    return ESP_FAIL;
}

static esp_err_t info_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "heap_free",  (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min",   (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "uptime_ms",  (double)(esp_timer_get_time() / 1000));
    cJSON_AddBoolToObject(root,   "wifi_connected", tab5_wifi_connected());

    char ip[20];
    get_wifi_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(root, "wifi_ip", ip);

    /* Post-streaming cleanup (#154): "Dragon connected" now means voice WS
     * (port 3502) is up — the CDP streaming link (port 3501) is gone.
     * We keep the `dragon_connected` field for backward compat with Dragon
     * E2E tests (e2e_full_suite.py, test_stress_40.py, test_soak.py) that
     * read info["dragon_connected"]. It's now an alias of voice_connected. */
    bool voice_up = voice_is_connected();
    cJSON_AddBoolToObject(root, "dragon_connected", voice_up);
    cJSON_AddBoolToObject(root, "voice_connected", voice_up);

    char display_str[16];
    snprintf(display_str, sizeof(display_str), "%dx%d", FB_W, FB_H);
    cJSON_AddStringToObject(root, "display", display_str);

    cJSON_AddNumberToObject(root, "lvgl_fps", (double)ui_core_get_fps());

    tab5_battery_info_t bat = {0};
    tab5_battery_read(&bat);
    cJSON_AddNumberToObject(root, "battery_pct", (double)bat.percent);

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    cJSON_AddNumberToObject(root, "tasks", (double)task_count);

    /* SD card */
    cJSON_AddBoolToObject(root, "sd_mounted", tab5_sdcard_mounted());
    if (tab5_sdcard_mounted()) {
        cJSON_AddNumberToObject(root, "sd_total_mb", (double)(tab5_sdcard_total_bytes() / (1024 * 1024)));
        cJSON_AddNumberToObject(root, "sd_free_mb", (double)(tab5_sdcard_free_bytes() / (1024 * 1024)));
    }

    const char *reset_reasons[] = {"UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT","TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO","USB","JTAG","EFUSE","PWR_GLITCH","CPU_LOCKUP"};
    esp_reset_reason_t reason = esp_reset_reason();
    cJSON_AddStringToObject(root, "reset_reason", reason < sizeof(reset_reasons)/sizeof(reset_reasons[0]) ? reset_reasons[reason] : "UNKNOWN");

    cJSON_AddBoolToObject(root, "auth_required", true);

    /* Audit B2 + C4 (#202): expose ring/store eviction counters so the
     * dashboard can spot a skill storm or chat-burst overflow without
     * grep'ing serial logs.  Both are monotonic counters since boot. */
    cJSON_AddNumberToObject(root, "chat_evictions_total",
                            (double)chat_store_evictions_total());
    cJSON_AddNumberToObject(root, "widget_evictions_total",
                            (double)widget_store_evictions_total());
    const char *last_evicted = widget_store_last_evicted_id();
    if (last_evicted && last_evicted[0]) {
        cJSON_AddStringToObject(root, "widget_last_evicted_id", last_evicted);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ======================================================================== */
/*  POST /touch                                                              */
/* ======================================================================== */

static esp_err_t touch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *jaction = cJSON_GetObjectItem(root, "action");
    const char *action = (jaction && cJSON_IsString(jaction)) ? jaction->valuestring : "tap";

    /* Swipe takes (x1,y1)->(x2,y2) over duration_ms.  Everything else uses
       a single (x,y) point.  Long-press uses (x,y) + duration_ms. */
    if (strcmp(action, "swipe") == 0) {
        cJSON *jx1 = cJSON_GetObjectItem(root, "x1");
        cJSON *jy1 = cJSON_GetObjectItem(root, "y1");
        cJSON *jx2 = cJSON_GetObjectItem(root, "x2");
        cJSON *jy2 = cJSON_GetObjectItem(root, "y2");
        cJSON *jdur = cJSON_GetObjectItem(root, "duration_ms");
        if (!cJSON_IsNumber(jx1) || !cJSON_IsNumber(jy1) ||
            !cJSON_IsNumber(jx2) || !cJSON_IsNumber(jy2)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "swipe needs x1,y1,x2,y2");
            return ESP_FAIL;
        }
        int x1 = jx1->valueint, y1 = jy1->valueint;
        int x2 = jx2->valueint, y2 = jy2->valueint;
        int dur = (jdur && cJSON_IsNumber(jdur)) ? jdur->valueint : 250;
        if (dur < 50)   dur = 50;
        if (dur > 3000) dur = 3000;

        ESP_LOGI(TAG, "Touch inject: swipe (%d,%d)->(%d,%d) %dms",
                 x1, y1, x2, y2, dur);

        /* 20 ms step cadence — enough for LVGL to register the gesture
           direction without flooding the indev read callback. */
        const int step_ms = 20;
        int steps = dur / step_ms;
        if (steps < 5) steps = 5;

        inject_publish(x1, y1, /*active=*/true, /*pressed=*/true);
        vTaskDelay(pdMS_TO_TICKS(40));   /* settle — let LVGL see the press */
        for (int i = 1; i <= steps; i++) {
           int32_t sx = x1 + (x2 - x1) * i / steps;
           int32_t sy = y1 + (y2 - y1) * i / steps;
           inject_publish(sx, sy, /*active=*/true, /*pressed=*/true);
           vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
        inject_publish(s_inject_state.x, s_inject_state.y, /*active=*/true, /*pressed=*/false);
        vTaskDelay(pdMS_TO_TICKS(100));  /* release + LVGL gesture dispatch */
        inject_publish(0, 0, /*active=*/false, /*pressed=*/false);

        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }

    cJSON *jx = cJSON_GetObjectItem(root, "x");
    cJSON *jy = cJSON_GetObjectItem(root, "y");

    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Need x and y");
        return ESP_FAIL;
    }

    int x = jx->valueint;
    int y = jy->valueint;

    ESP_LOGI(TAG, "Touch inject: x=%d y=%d action=%s", x, y, action);

    /* Inject via atomic seqlock-published state (polled by LVGL touch_read_cb).
     * TT #328 Wave 2: each transition is published atomically so the reader
     * never observes torn (x, y, active, pressed) tuples. */
    if (strcmp(action, "release") == 0) {
       inject_publish(x, y, /*active=*/true, /*pressed=*/false);
       vTaskDelay(pdMS_TO_TICKS(100));
       inject_publish(0, 0, /*active=*/false, /*pressed=*/false);
    } else if (strcmp(action, "press") == 0) {
       inject_publish(x, y, /*active=*/true, /*pressed=*/true);
       /* Leave active — caller must POST release to end it */
    } else if (strcmp(action, "long_press") == 0) {
        /* Hold long enough for LVGL LV_EVENT_LONG_PRESSED (default 400 ms) +
           a little slack so LONG_PRESSED_REPEAT doesn't mis-fire. Caller can
           override via duration_ms. */
        cJSON *jdur = cJSON_GetObjectItem(root, "duration_ms");
        int dur = (jdur && cJSON_IsNumber(jdur)) ? jdur->valueint : 800;
        if (dur < 500)  dur = 500;
        if (dur > 5000) dur = 5000;
        inject_publish(x, y, /*active=*/true, /*pressed=*/true);
        vTaskDelay(pdMS_TO_TICKS(dur));
        inject_publish(x, y, /*active=*/true, /*pressed=*/false);
        vTaskDelay(pdMS_TO_TICKS(100));
        inject_publish(0, 0, /*active=*/false, /*pressed=*/false);
    } else {
        /* tap: press, hold 200ms, release (needs 2+ LVGL ticks for CLICKED) */
        inject_publish(x, y, /*active=*/true, /*pressed=*/true);
        vTaskDelay(pdMS_TO_TICKS(200));
        inject_publish(x, y, /*active=*/true, /*pressed=*/false);
        vTaskDelay(pdMS_TO_TICKS(100));
        inject_publish(0, 0, /*active=*/false, /*pressed=*/false);
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ======================================================================== */
/*  POST /reboot                                                             */
/* ======================================================================== */

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"rebooting\":true}");
    ESP_LOGW(TAG, "Reboot requested via debug server");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  /* unreachable */
}

/* ======================================================================== */
/*  GET /log                                                                 */
/* ======================================================================== */

static esp_err_t log_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "heap_free",  (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min",   (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "uptime_s",   (double)(esp_timer_get_time() / 1000000));

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    cJSON_AddNumberToObject(root, "tasks", (double)task_count);

    /* Task list requires configUSE_TRACE_FACILITY — just report count */

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "print");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ======================================================================== */
/*  GET /  — Interactive multi-tab SPA (#150 PR γ)                           */
/* ======================================================================== */
/*
 * The HTML + CSS + JS lives in main/debug_ui.html and is embedded into
 * flash via EMBED_TXTFILES in CMakeLists.txt.  This keeps the ~22 KB
 * SPA editable without the C string-literal horrors.
 *
 * NOTE: index is NOT auth-gated — the page itself has no secrets and
 * prompts for the Bearer token in the UI (stored in localStorage).
 * Every endpoint the SPA calls IS auth-gated.
 */
extern const char debug_ui_html_start[] asm("_binary_debug_ui_html_start");
extern const char debug_ui_html_end[]   asm("_binary_debug_ui_html_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    const size_t len = debug_ui_html_end - debug_ui_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, debug_ui_html_start, len);
}

/* ======================================================================== */
/*  GET /crashlog — last core dump summary                                   */
/* ======================================================================== */
/*
 * #148: /open was removed (merged into /navigate which already handled
 * more screens — see async_navigate).  The only target /open covered
 * that /navigate didn't was "wifi", which is now a case in async_navigate.
 */

static esp_err_t crashlog_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
        return ESP_FAIL;
    }

    /* Reset reason */
    const char *reset_reasons[] = {"UNKNOWN","POWERON","EXT","SW","PANIC",
        "INT_WDT","TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO","USB",
        "JTAG","EFUSE","PWR_GLITCH","CPU_LOCKUP"};
    esp_reset_reason_t reason = esp_reset_reason();
    cJSON_AddStringToObject(root, "reset_reason",
        reason < sizeof(reset_reasons)/sizeof(reset_reasons[0])
        ? reset_reasons[reason] : "UNKNOWN");
    cJSON_AddBoolToObject(root, "was_crash",
        reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
        reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);

    /* Check if a core dump exists in flash */
    esp_core_dump_summary_t summary;
    esp_err_t cd_ret = esp_core_dump_get_summary(&summary);
    if (cd_ret == ESP_OK) {
        cJSON_AddBoolToObject(root, "coredump_present", true);
        cJSON_AddNumberToObject(root, "exc_pc", (double)summary.exc_pc);
        cJSON_AddStringToObject(root, "exc_task", summary.exc_task);

        /* RISC-V: no on-device backtrace, provide stack dump size */
        cJSON_AddNumberToObject(root, "stackdump_size",
            (double)summary.exc_bt_info.dump_size);
        cJSON_AddStringToObject(root, "hint",
            "Use 'espcoredump.py info_corefile' on host for full backtrace");
    } else {
        cJSON_AddBoolToObject(root, "coredump_present", false);
        cJSON_AddStringToObject(root, "note",
            cd_ret == ESP_ERR_NOT_FOUND
            ? "No core dump in flash"
            : "Core dump read error");
    }

    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min", (double)esp_get_minimum_free_heap_size());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "print");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── Coredump binary download ─────────────────────────────────────────── */

/* Wave 15 W15-C05: stream the raw coredump partition bytes so we can
 * decode them offline with `espcoredump.py info_corefile -t elf`.
 * Partition reads are chunked into 4 KiB so the httpd task stack stays
 * tiny regardless of the dump size (~3 MB on this board).  Erase is
 * NOT performed here — the operator explicitly calls /crashlog?erase=1
 * or runs the esptool after confirming the dump was captured.  This
 * avoids the pathological loop where a corrupted dump keeps crashing
 * the decoder and gets wiped on every fetch attempt. */
static esp_err_t coredump_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    size_t addr = 0, size = 0;
    esp_err_t err = esp_core_dump_image_get(&addr, &size);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_send(req, "{\"error\":\"no coredump\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "coredump_image_get failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read error");
        return ESP_FAIL;
    }
    if (size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "{\"error\":\"empty coredump\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Find the coredump partition so we can read via partition-relative
     * offsets.  `esp_core_dump_image_get` returns absolute flash addrs
     * but `esp_partition_read` wants offset-from-partition-start. */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no coredump partition");
        return ESP_FAIL;
    }
    if (addr < part->address || addr + size > part->address + part->size) {
        ESP_LOGE(TAG, "coredump range 0x%zx+%zu outside partition 0x%lx+%lu",
                 addr, size, (unsigned long)part->address, (unsigned long)part->size);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "range mismatch");
        return ESP_FAIL;
    }
    const size_t rel = addr - part->address;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    httpd_resp_set_hdr(req, "Content-Disposition",
                      "attachment; filename=\"tab5_coredump.elf\"");
    /* Advertise size via X-Content-Length so the client can track
     * progress without conflicting with the chunked Transfer-Encoding
     * that httpd_resp_send_chunk() emits. */
    char len[32];
    snprintf(len, sizeof(len), "%zu", size);
    httpd_resp_set_hdr(req, "X-Content-Length", len);

    /* Stream in 4 KiB chunks. */
    const size_t CHUNK = 4096;
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DEFAULT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t sent = 0;
    while (sent < size) {
        size_t to_read = size - sent;
        if (to_read > CHUNK) to_read = CHUNK;
        err = esp_partition_read(part, rel + sent, buf, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "partition_read@%zu: %s", rel + sent, esp_err_to_name(err));
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, to_read) != ESP_OK) {
            /* Client dropped — bail silently. */
            break;
        }
        sent += to_read;
    }
    heap_caps_free(buf);
    /* Terminate chunked stream. */
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "/coredump: sent %zu/%zu bytes", sent, size);
    return ESP_OK;
}

/* ── Heap trace (Wave 15 leak hunt) ───────────────────────────────────── */
/* Standalone heap tracing — ESP-IDF tracks every heap_caps_malloc /
 * heap_caps_free inside a fixed-size ring buffer.  HEAP_TRACE_LEAKS
 * mode keeps only allocations that haven't been freed yet.  The
 * matching serial dump reveals the caller's PC and the bytes held.
 * Flow:
 *   1. POST /heap_trace_start  → reset + start tracking
 *   2. ... run activity for N minutes ...
 *   3. GET  /heap_trace_dump   → stop + print to serial + JSON summary
 * Operator captures serial output for the full stack / caller PCs;
 * the JSON summary returns high-level stats so a scripted probe can
 * tell when tracking filled up.
 *
 * Requires CONFIG_HEAP_TRACING_STANDALONE=y. */
#include "esp_heap_trace.h"

#define TAB5_HEAP_TRACE_NUM_RECORDS  300
static heap_trace_record_t *s_heap_trace_buf = NULL;
static bool s_heap_trace_active = false;

static esp_err_t heap_trace_start_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (!s_heap_trace_buf) {
        s_heap_trace_buf = heap_caps_calloc(
            TAB5_HEAP_TRACE_NUM_RECORDS, sizeof(heap_trace_record_t),
            MALLOC_CAP_SPIRAM);
        if (!s_heap_trace_buf) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "oom: trace buffer (PSRAM)");
            return ESP_FAIL;
        }
        esp_err_t err = heap_trace_init_standalone(
            s_heap_trace_buf, TAB5_HEAP_TRACE_NUM_RECORDS);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "heap_trace_init_standalone failed");
            heap_caps_free(s_heap_trace_buf);
            s_heap_trace_buf = NULL;
            return ESP_FAIL;
        }
    }
    if (s_heap_trace_active) {
        heap_trace_stop();
    }
    heap_trace_resume();   /* wipes unfreed-set, ready for fresh record */
    esp_err_t err = heap_trace_start(HEAP_TRACE_LEAKS);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "heap_trace_start failed");
        return ESP_FAIL;
    }
    s_heap_trace_active = true;
    ESP_LOGI(TAG, "Heap trace STARTED (capacity=%d records, PSRAM)",
             TAB5_HEAP_TRACE_NUM_RECORDS);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true,\"records\":300,\"mode\":\"leaks\"}");
    return ESP_OK;
}

static esp_err_t heap_trace_dump_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (!s_heap_trace_active || !s_heap_trace_buf) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"error\":\"heap_trace not running — POST /heap_trace_start first\"}");
        return ESP_OK;
    }

    /* Stop collection, then emit the outstanding-allocations report to
     * serial.  Full info (caller PCs + size) goes to UART via ESP_LOG;
     * we also compute a quick summary for the HTTP response. */
    heap_trace_stop();

    size_t count = heap_trace_get_count();
    /* Walk outstanding records and split by whether the alloc lives in
     * internal SRAM (by address range) vs PSRAM.  The record struct
     * itself doesn't carry caps, so classify via the actual pointer. */
    size_t internal_bytes = 0, psram_bytes = 0;
    heap_trace_record_t rec;
    for (size_t i = 0; i < count; i++) {
        if (heap_trace_get(i, &rec) != ESP_OK) break;
        /* ESP32-P4 PSRAM is mapped in the 0x48000000–0x49000000
         * and 0x4ff00000–... windows; internal SRAM lives below.
         * Simplest reliable split: ask the heap subsystem. */
        uint32_t caps = heap_caps_get_allocated_size(rec.address) > 0
                        ? (uintptr_t)rec.address >= 0x48000000
                          && (uintptr_t)rec.address < 0x4c000000
                            ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL
                        : 0;
        if (caps == MALLOC_CAP_SPIRAM) {
            psram_bytes += rec.size;
        } else {
            internal_bytes += rec.size;
        }
    }

    /* Dump the full record list to UART for offline analysis.
     * Operator tails `idf.py monitor` or captures via serial script. */
    ESP_LOGW(TAG, "── Heap trace dump (%zu records, internal=%zu B, psram=%zu B) ──",
             count, internal_bytes, psram_bytes);
    heap_trace_dump();

    char body[256];
    snprintf(body, sizeof(body),
        "{\"ok\":true,\"records\":%zu,\"internal_bytes\":%zu,"
        "\"psram_bytes\":%zu,\"hint\":\"full dump in serial log\"}",
        count, internal_bytes, psram_bytes);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, body);

    s_heap_trace_active = false;
    return ESP_OK;
}

/* ── SD card file listing ─────────────────────────────────────────────── */

static void _list_dir_to_json(cJSON *arr, const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);

        struct stat st;
        stat(full, &st);

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddStringToObject(item, "path", full);
        cJSON_AddBoolToObject(item, "dir", entry->d_type == DT_DIR);
        if (entry->d_type != DT_DIR) {
            cJSON_AddNumberToObject(item, "size", (double)st.st_size);
        }
        cJSON_AddItemToArray(arr, item);
    }
    closedir(d);
}

static esp_err_t sdcard_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "mounted", tab5_sdcard_mounted());

    if (tab5_sdcard_mounted()) {
        cJSON_AddNumberToObject(root, "total_mb", (double)(tab5_sdcard_total_bytes() / (1024*1024)));
        cJSON_AddNumberToObject(root, "free_mb", (double)(tab5_sdcard_free_bytes() / (1024*1024)));

        /* List /sdcard root */
        cJSON *files = cJSON_AddArrayToObject(root, "files");
        _list_dir_to_json(files, "/sdcard");

        /* List /sdcard/rec if it exists */
        struct stat st;
        if (stat("/sdcard/rec", &st) == 0) {
            cJSON *recs = cJSON_AddArrayToObject(root, "recordings");
            _list_dir_to_json(recs, "/sdcard/rec");
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── OTA endpoint family extracted to debug_server_ota.c (Wave 23b, #332) ─ */

/* ── ADB-style debug APIs ─────────────────────────────────────────────── */

/* ── /settings (GET + POST) family extracted to debug_server_settings.c
 *    (Wave 23b, #332) — handlers + register live there now. ───────────── */

/* ── /mode endpoint family extracted to debug_server_mode.c (Wave 23b, #332) ─ */


/* POST /navigate?screen=settings|notes|chat|camera|files|home
 * Uses lv_async_call to avoid LVGL lock deadlock from HTTP context */
static char s_nav_target[16] = {0};

static volatile bool s_navigating = false;

/* TT #328 Wave 10 follow-up — public setter so non-/navigate code paths
 * (the persistent home button in ui_chrome, swipe-right-back gestures
 * in chat/camera/files) can keep s_nav_target in sync with the actually
 * loaded screen.  Pre-fix /screen would return the stale last-/navigate
 * target while the device was on a different screen, which broke the
 * harness's persistent-home-button assertion. */
void tab5_debug_set_nav_target(const char *name) {
   if (!name || !name[0]) return;
   size_t cap = sizeof(s_nav_target) - 1;
   strncpy(s_nav_target, name, cap);
   s_nav_target[cap] = '\0';
   extern void tab5_debug_obs_event(const char *, const char *);
   tab5_debug_obs_event("screen.navigate", s_nav_target);
}

static void async_navigate(void *arg)
{
    (void)arg;
    if (s_navigating) {
        ESP_LOGW("nav", "Navigation already in progress — skipping");
        return;
    }
    s_navigating = true;
    ESP_LOGI("nav", "async_navigate executing, target='%s'", s_nav_target);
    /* Navigate using the home tileview for pages 0-3.
     * For secondary screens (camera, files) create them separately.
     * This avoids the bug where ui_settings_create() replaces the
     * tileview screen entirely, breaking subsequent navigation. */

    /* Always dismiss ALL overlays before any navigation — HIDE not destroy.
     * Destroy+recreate corrupts LVGL timer linked list (lv_ll_remove crash). */
    extern void ui_chat_hide(void);
    extern void ui_settings_hide(void);
    extern void ui_notes_hide(void);
    extern void ui_agents_hide(void);
    extern void ui_memory_hide(void);
    extern void ui_focus_hide(void);
    extern void ui_sessions_hide(void);
    ui_chat_hide();
    ui_settings_hide();
    ui_notes_hide();
    ui_agents_hide();
    ui_memory_hide();
    ui_focus_hide();
    ui_sessions_hide();
    ui_keyboard_hide();
    /* Wave 4 UX fix: if the voice overlay is up but voice is idle, a
     * nav request means the user has moved on.  Dismiss so the user
     * sees the screen they just navigated to instead of a stale
     * "Tap to speak." card blocking the view.  Active states
     * (LISTENING / PROCESSING / SPEAKING) are left alone. */
    extern void ui_voice_dismiss_if_idle(void);
    ui_voice_dismiss_if_idle();

    /* #167: camera + files use destroy/create semantics (they own big
     * PSRAM canvas buffers + LVGL screen trees, not hide/show overlays).
     * If the user navigates away from them without hitting the back
     * chevron (e.g. /navigate or nav-sheet), we must tear them down here
     * or we leak ~1.8 MB + a zombie preview timer per camera cycle.
     * Both functions are NULL-guarded internally. */
    extern void ui_camera_destroy(void);
    extern void ui_files_destroy(void);
    ui_camera_destroy();
    ui_files_destroy();

    if (strcmp(s_nav_target, "home") == 0) {
        ui_home_go_home();
    } else if (strcmp(s_nav_target, "notes") == 0) {
        extern lv_obj_t *ui_notes_create(void);
        ui_notes_create();
    } else if (strcmp(s_nav_target, "chat") == 0) {
        extern lv_obj_t *ui_chat_create(void);
        ui_chat_create();
    } else if (strcmp(s_nav_target, "settings") == 0) {
        /* Settings is too heavy for lv_async_call — it blocks the LVGL render loop.
         * Use direct call since async_navigate already runs in LVGL context via lv_async_call. */
        extern void ui_home_nav_settings(void);
        ui_home_nav_settings();
    } else if (strcmp(s_nav_target, "camera") == 0) {
        extern lv_obj_t *ui_camera_create(void);
        ui_camera_create();
    } else if (strcmp(s_nav_target, "files") == 0) {
        extern lv_obj_t *ui_files_create(void);
        ui_files_create();
    } else if (strcmp(s_nav_target, "sessions") == 0) {
        extern void ui_sessions_show(void);
        ui_sessions_show();
    } else if (strcmp(s_nav_target, "agents") == 0) {
        extern void ui_agents_show(void);
        ui_agents_show();
    } else if (strcmp(s_nav_target, "skills") == 0) {
       /* TT #328 Wave 10 — dedicated tools-catalog viewer. */
       extern void ui_skills_show(void);
       ui_skills_show();
    } else if (strcmp(s_nav_target, "memory") == 0) {
       extern void ui_memory_show(void);
       ui_memory_show();
    } else if (strcmp(s_nav_target, "focus") == 0) {
       extern void ui_focus_show(void);
       ui_focus_show();
    } else if (strcmp(s_nav_target, "wifi") == 0) {
       /* #148: folded in from the removed /open endpoint so /navigate
        * is the single source of truth for all screen lists. */
       extern lv_obj_t *ui_wifi_create(void);
       ui_wifi_create();
    }
    s_navigating = false;
}

/* ── /widget — testing-only hook that injects a widget_live into the store
 *   without requiring Dragon. Body is JSON matching widget_live schema (see
 *   TinkerBox docs/protocol.md §17.2). Used by the Widget Platform test
 *   harness before the Dragon Tab5Surface is wired. */
#include "widget.h"
static void async_widget_refresh(void *arg) {
    (void)arg;
    extern void ui_home_update_status(void);
    ui_home_update_status();
}

static esp_err_t widget_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* Read body */
    int len = req->content_len;
    if (len <= 0 || len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body 1..2048 bytes");
        return ESP_FAIL;
    }
    char *buf = malloc(len + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read"); return ESP_FAIL; }
        got += r;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    if (action && !strcmp(action, "dismiss")) {
        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
        if (cid) widget_store_dismiss(cid);
        cJSON_Delete(root);
        tab5_lv_async_call(async_widget_refresh, NULL);
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }

    /* Build widget_t from JSON */
    widget_t w = {0};
    const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "skill_id"));
    const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
    const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
    const char *icn = cJSON_GetStringValue(cJSON_GetObjectItem(root, "icon"));
    const char *tn  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
    cJSON *prog     = cJSON_GetObjectItem(root, "progress");
    cJSON *pri      = cJSON_GetObjectItem(root, "priority");
    cJSON *act      = cJSON_GetObjectItem(root, "action");

    if (!cid) cid = "debug_1";
    if (!sid) sid = "debug";
    strncpy(w.card_id,  cid, WIDGET_ID_LEN - 1);
    strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
    if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
    if (bdy) strncpy(w.body,  bdy, WIDGET_BODY_LEN  - 1);
    if (icn) strncpy(w.icon,  icn, WIDGET_ICON_LEN  - 1);
    /* v4·D Phase 4c: optional "type" + "items" fields let the debug
     * endpoint inject widget_list payloads for visual testing. */
    const char *tstr = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    w.type = tstr ? widget_type_from_str(tstr) : WIDGET_TYPE_LIVE;
    if (w.type == WIDGET_TYPE_NONE) w.type = WIDGET_TYPE_LIVE;
    w.tone     = widget_tone_from_str(tn);
    w.progress = cJSON_IsNumber(prog) ? (float)prog->valuedouble : 0.0f;
    w.priority = cJSON_IsNumber(pri)  ? (uint8_t)pri->valueint   : 50;
    if (cJSON_IsObject(act)) {
        const char *al = cJSON_GetStringValue(cJSON_GetObjectItem(act, "label"));
        const char *ae = cJSON_GetStringValue(cJSON_GetObjectItem(act, "event"));
        if (al) strncpy(w.action_label, al, WIDGET_ACTION_LBL_LEN - 1);
        if (ae) strncpy(w.action_event, ae, WIDGET_ACTION_EVT_LEN - 1);
    }
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        int cnt = cJSON_GetArraySize(items);
        if (cnt > WIDGET_LIST_MAX_ITEMS) cnt = WIDGET_LIST_MAX_ITEMS;
        for (int i = 0; i < cnt; i++) {
            cJSON *it = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(it)) continue;
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
            const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "value"));
            if (t) strncpy(w.items[i].text,  t, WIDGET_LIST_ITEM_TEXT_LEN  - 1);
            if (v) strncpy(w.items[i].value, v, WIDGET_LIST_ITEM_VALUE_LEN - 1);
        }
        w.items_count = (uint8_t)cnt;
    }
    /* v4·D Phase 4f: optional "values" + "max" fields for widget_chart. */
    cJSON *vals = cJSON_GetObjectItem(root, "values");
    if (cJSON_IsArray(vals)) {
        int cnt = cJSON_GetArraySize(vals);
        if (cnt > WIDGET_CHART_MAX_POINTS) cnt = WIDGET_CHART_MAX_POINTS;
        for (int i = 0; i < cnt; i++) {
            cJSON *v = cJSON_GetArrayItem(vals, i);
            w.chart_values[i] = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.0f;
        }
        w.chart_count = (uint8_t)cnt;
    }
    cJSON *mx = cJSON_GetObjectItem(root, "max");
    w.chart_max = cJSON_IsNumber(mx) ? (float)mx->valuedouble : 0.0f;
    /* v4·D Phase 4g: media + prompt fields for /widget debug injection. */
    const char *media_url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    const char *media_alt = cJSON_GetStringValue(cJSON_GetObjectItem(root, "alt"));
    if (media_url) strncpy(w.media_url, media_url, WIDGET_MEDIA_URL_LEN - 1);
    if (media_alt) strncpy(w.media_alt, media_alt, WIDGET_MEDIA_ALT_LEN - 1);
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices)) {
        int cnt = cJSON_GetArraySize(choices);
        if (cnt > WIDGET_PROMPT_MAX_CHOICES) cnt = WIDGET_PROMPT_MAX_CHOICES;
        for (int i = 0; i < cnt; i++) {
            cJSON *it = cJSON_GetArrayItem(choices, i);
            if (!cJSON_IsObject(it)) continue;
            const char *t  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
            const char *ev = cJSON_GetStringValue(cJSON_GetObjectItem(it, "event"));
            if (t)  strncpy(w.choices[i].text,  t,  WIDGET_PROMPT_CHOICE_LEN - 1);
            if (ev) strncpy(w.choices[i].event, ev, WIDGET_PROMPT_EVENT_LEN - 1);
        }
        w.choices_count = (uint8_t)cnt;
    }
    widget_store_upsert(&w);
    cJSON_Delete(root);
    tab5_lv_async_call(async_widget_refresh, NULL);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t navigate_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* Debounce: reject rapid navigation requests (same as UI 300ms debounce).
     * Without this, rapid /navigate calls queue multiple lv_async_call entries
     * that create/destroy LVGL overlays simultaneously → Load access fault. */
    static int64_t s_last_nav_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - s_last_nav_us < 500000) {  /* 500ms debounce */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"navigation too fast, wait 500ms\"}");
        return ESP_OK;
    }
    s_last_nav_us = now;

    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"use ?screen=settings|notes|chat|camera|files|home\"}");
        return ESP_OK;
    }

    httpd_query_key_value(query, "screen", s_nav_target, sizeof(s_nav_target));

    /* #293: emit obs event for e2e harness BEFORE the async dispatch so
     * the harness sees the navigation intent even if the dispatch is
     * queued.  s_current_screen also gets updated on the LVGL side via
     * async_navigate completion.  Forward-declared because debug_obs.h
     * is included further down the file (after this handler). */
    extern void tab5_debug_obs_event(const char *kind, const char *detail);
    tab5_debug_obs_event("screen.navigate", s_nav_target);

    /* Schedule on LVGL thread (#258 helper takes the recursive LVGL
     * mutex internally — lv_async_call itself is NOT thread-safe). */
    tab5_lv_async_call(async_navigate, NULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "navigated", s_nav_target);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* #148: /navtouch removed.  It was a thin wrapper that computed a bogus
 * tap_x coordinate and then called the same /navigate path anyway.
 * Callers should POST /navigate?screen=<name> directly. */

/* ── Camera debug endpoint ────────────────────────────────────────────── */

static esp_err_t camera_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (!tab5_camera_initialized()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"camera not initialized\"}");
        return ESP_OK;
    }

    tab5_cam_frame_t frame;
    esp_err_t err = tab5_camera_capture(&frame);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"capture failed: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, buf);
        return ESP_OK;
    }

    /* #148: was ~1.8 MB RGB565 BMP — now JPEG-encoded via the same
     * hardware engine as /screenshot (~40-80 KB typical).  Only the
     * RGB565 format path supports encoding; other formats fall back
     * to a clear error so callers can diagnose. */
    if (frame.format != TAB5_CAM_FMT_RGB565) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"error\":\"unsupported camera format (expected RGB565)\"}");
        return ESP_OK;
    }
    if (_ensure_jpeg_encoder() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encoder init failed");
        return ESP_FAIL;
    }

    size_t fb_size = (size_t)frame.width * frame.height * 2;

    jpeg_encode_memory_alloc_cfg_t in_alloc  = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    size_t in_capacity = 0;
    uint8_t *in_buf = jpeg_alloc_encoder_mem(fb_size, &in_alloc, &in_capacity);
    if (!in_buf || in_capacity < fb_size) {
        if (in_buf) free(in_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG input alloc failed");
        return ESP_FAIL;
    }
    memcpy(in_buf, frame.data, fb_size);

    const size_t out_cap = 256 * 1024;
    jpeg_encode_memory_alloc_cfg_t out_alloc = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t out_capacity = 0;
    uint8_t *out_buf = jpeg_alloc_encoder_mem(out_cap, &out_alloc, &out_capacity);
    if (!out_buf) {
        free(in_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG output alloc failed");
        return ESP_FAIL;
    }

    jpeg_encode_cfg_t enc_cfg = {
        .height        = frame.height,
        .width         = frame.width,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = 80,
    };
    uint32_t out_size = 0;
    xSemaphoreTake(s_jpeg_mux, portMAX_DELAY);
    err = jpeg_encoder_process(s_jpeg_enc, &enc_cfg, in_buf, fb_size,
                               out_buf, out_capacity, &out_size);
    xSemaphoreGive(s_jpeg_mux);
    free(in_buf);

    if (err != ESP_OK || out_size == 0) {
        ESP_LOGE(TAG, "camera jpeg_encoder_process: %s out=%u",
                 esp_err_to_name(err), (unsigned)out_size);
        free(out_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encode failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    err = httpd_resp_send(req, (const char *)out_buf, out_size);
    free(out_buf);
    return err;
}

/* ── Chat/Text input endpoint ─────────────────────────────────────────── */

static esp_err_t chat_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* POST /chat — send text to Dragon via voice WS.
     * #148: was capped at a 256-byte stack buffer which silently truncated
     * anything longer than ~200 bytes of JSON after the `{"text":"..."}`
     * overhead.  Now accepts up to 4 KB via PSRAM-backed body buffer with
     * the text itself up to 1 KB (voice.c accepts up to 2 KB). */
    const size_t MAX_BODY = 4096;
    const size_t MAX_TEXT = 1024;
    char text_buf[MAX_TEXT + 1];
    text_buf[0] = '\0';

    /* First try query string — cheap for short text, no body needed. */
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "text", text_buf, sizeof(text_buf));
    }

    /* If no query text, parse JSON body from heap. */
    if (text_buf[0] == '\0') {
        int total = req->content_len;
        if (total > 0) {
            if (total > (int)MAX_BODY) total = MAX_BODY;
            char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
            if (!body) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
                return ESP_OK;
            }
            int got = 0;
            while (got < total) {
                int r = httpd_req_recv(req, body + got, total - got);
                if (r <= 0) break;
                got += r;
            }
            body[got] = '\0';
            cJSON *root = cJSON_Parse(body);
            heap_caps_free(body);
            if (root) {
                cJSON *t = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(t) && t->valuestring) {
                    snprintf(text_buf, sizeof(text_buf), "%s", t->valuestring);
                }
                cJSON_Delete(root);
            }
        }
    }

    if (text_buf[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"use ?text=hello or POST {\\\"text\\\":\\\"hello\\\"}\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Debug chat: %.80s%s (len=%u)",
             text_buf, strlen(text_buf) > 80 ? "..." : "",
             (unsigned)strlen(text_buf));

    /* TT #317 P5: don't short-circuit on !voice_is_connected — let
     * voice_send_text decide.  When WS is down, the K144 failover (Local
     * mode) or VMODE_LOCAL_ONBOARD path may still complete the turn. */
    extern void ui_chat_push_message(const char *role, const char *text);
    ui_chat_push_message("user", text_buf);
    esp_err_t send_err = voice_send_text(text_buf);
    bool sent_ok = (send_err == ESP_OK);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", text_buf);
    cJSON_AddNumberToObject(root, "text_len", (double)strlen(text_buf));
    cJSON_AddBoolToObject(root, "sent", sent_ok);
    cJSON_AddBoolToObject(root, "voice_connected", voice_is_connected());
    if (!sent_ok) {
       cJSON_AddStringToObject(root, "send_status", esp_err_to_name(send_err));
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* Forward decl — definition is further down the file. */
static esp_err_t send_json_resp(httpd_req_t *req, cJSON *root);

/* ── POST /input/text — type into a *named* LVGL textarea ─────────────
 *
 * Body: {"text": "string", "submit": true|false, "target": "<name>"}
 * (Also accepts ?text= and ?submit= and ?target= query params.)
 *
 * #299 fix (was #296): the original implementation walked
 * lv_screen_active() + lv_layer_top() and wrote into the first
 * focused-or-found textarea.  That meant when the harness happened
 * to be on the Settings screen with the dragon_host textarea
 * focused, the test write went into NVS — corrupted dragon_host
 * overnight and broke voice WS reconnect on next boot.
 *
 * The new contract: the caller MUST name a target widget that the
 * firmware exposes via a registered accessor.  Currently registered
 * targets:
 *
 *   "chat"   (default if `target` omitted) — chat input bar's hidden
 *            textarea.  Requires the chat overlay to be active;
 *            otherwise returns {"ok":false,"error":"chat_not_active"}.
 *
 * Future targets (notes_edit, settings_dragon_host, wifi_pass, etc.)
 * land here when a real harness scenario needs them.  Keeping the
 * target list small means the harness can't accidentally clobber
 * credentials.
 *
 * Returns:
 *   {"ok": true,  "accepted": N, "submitted": bool, "target": "<name>"}
 *   {"ok": false, "error": "chat_not_active" | "unknown_target" }
 */
typedef struct {
    char text[1024];
    char target[24];     /* widget name; "" → defaults to "chat" */
    bool submit;
    /* Reply slot — written by the LVGL-thread executor. */
    bool ok;
    int  accepted;
    bool submitted;
    char  resolved_target[24];
    char  err[40];
    SemaphoreHandle_t done;
} input_text_args_t;

/* Resolve a target name to its LVGL textarea, or NULL.  Each target
 * is responsible for its own visibility/state checks (e.g. "chat"
 * returns NULL when the chat overlay isn't active). */
static lv_obj_t *resolve_input_target(const char *name)
{
    extern lv_obj_t *ui_chat_get_input_textarea(void);
    if (!name || !name[0] || strcmp(name, "chat") == 0) {
        return ui_chat_get_input_textarea();
    }
    return NULL;  /* unknown_target */
}

static void input_text_apply_lvgl(void *arg)
{
    input_text_args_t *a = (input_text_args_t *)arg;
    const char *name = a->target[0] ? a->target : "chat";
    snprintf(a->resolved_target, sizeof(a->resolved_target), "%s", name);

    /* Distinguish "unknown name" from "name resolved but widget
     * unavailable" so the harness gets actionable diagnostics. */
    bool known = (!a->target[0] || strcmp(name, "chat") == 0);
    if (!known) {
        a->ok = false;
        snprintf(a->err, sizeof(a->err), "unknown_target");
        xSemaphoreGive(a->done);
        return;
    }

    lv_obj_t *ta = resolve_input_target(name);
    if (!ta) {
        a->ok = false;
        snprintf(a->err, sizeof(a->err),
                 strcmp(name, "chat") == 0 ? "chat_not_active"
                                           : "target_unavailable");
        xSemaphoreGive(a->done);
        return;
    }

    lv_textarea_set_text(ta, a->text);
    a->ok = true;
    a->accepted = (int)strlen(a->text);
    if (a->submit) {
        lv_obj_send_event(ta, LV_EVENT_READY, NULL);
        a->submitted = true;
    }
    xSemaphoreGive(a->done);
}

static esp_err_t input_text_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    input_text_args_t a = {0};
    a.done = xSemaphoreCreateBinary();
    if (!a.done) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
        return ESP_OK;
    }

    /* Query string first (small payloads). */
    char query[1280] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "text",   a.text,   sizeof(a.text));
        httpd_query_key_value(query, "target", a.target, sizeof(a.target));
        char sv[8] = {0};
        if (httpd_query_key_value(query, "submit", sv, sizeof(sv)) == ESP_OK) {
            a.submit = (sv[0] == '1' || sv[0] == 't');
        }
    }

    /* Body fallback (JSON). */
    if (a.text[0] == '\0' && a.target[0] == '\0') {
        int total = req->content_len;
        if (total > 0 && total < 4096) {
            char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
            if (body) {
                int got = 0;
                while (got < total) {
                    int r = httpd_req_recv(req, body + got, total - got);
                    if (r <= 0) break;
                    got += r;
                }
                body[got] = '\0';
                cJSON *root = cJSON_Parse(body);
                heap_caps_free(body);
                if (root) {
                    cJSON *t   = cJSON_GetObjectItem(root, "text");
                    cJSON *s   = cJSON_GetObjectItem(root, "submit");
                    cJSON *tgt = cJSON_GetObjectItem(root, "target");
                    if (cJSON_IsString(t) && t->valuestring) {
                        snprintf(a.text, sizeof(a.text), "%s", t->valuestring);
                    }
                    if (cJSON_IsString(tgt) && tgt->valuestring) {
                        snprintf(a.target, sizeof(a.target), "%s", tgt->valuestring);
                    }
                    if (cJSON_IsBool(s)) a.submit = cJSON_IsTrue(s);
                    cJSON_Delete(root);
                }
            }
        }
    }

    /* Even an empty string is a valid clear-the-field operation, so
     * we don't reject; just dispatch. */
    tab5_lv_async_call(input_text_apply_lvgl, &a);
    /* Wait for the LVGL-thread to fill the reply (cap at 2 s — should
     * be sub-millisecond in practice). */
    xSemaphoreTake(a.done, pdMS_TO_TICKS(2000));
    vSemaphoreDelete(a.done);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", a.ok);
    cJSON_AddStringToObject(root, "target",
                            a.resolved_target[0] ? a.resolved_target
                                                 : (a.target[0] ? a.target : "chat"));
    if (a.ok) {
        cJSON_AddNumberToObject(root, "accepted", a.accepted);
        cJSON_AddBoolToObject(root, "submitted", a.submitted);
    } else {
        cJSON_AddStringToObject(root, "error",
                                a.err[0] ? a.err : "unknown");
    }
    return send_json_resp(req, root);
}

/* ── Wi-Fi kick endpoint (test harness) ──────────────────────────────── */
/* ── /wifi/kick + /wifi/status family extracted to debug_server_wifi.c
 *    (Wave 23b, #332) — handlers + register live there now. ─────────── */

/* ── /dictation family extracted to debug_server_dictation.c
 *    (Wave 23b, #332). ─────────────────────────────────────────── */

/* ── Voice state endpoint ─────────────────────────────────────────────── */

/* #293: GET /screen — current screen + overlay visibility for the e2e
 * harness.  s_nav_target is the last-requested screen via /navigate;
 * overlay state comes from the per-module ui_*_is_visible() / is_active()
 * helpers. */
static esp_err_t screen_state_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    extern bool ui_chat_is_active(void);
    extern bool ui_voice_is_visible(void);
    extern bool ui_settings_is_visible(void);

    cJSON *root = cJSON_CreateObject();
    /* Last-requested screen via /navigate.  Defaults to "home" until
     * the first navigate fires (Tab5 boots into home). */
    cJSON_AddStringToObject(root, "current",
        s_nav_target[0] ? s_nav_target : "home");
    cJSON *overlays = cJSON_CreateObject();
    cJSON_AddBoolToObject(overlays, "chat",     ui_chat_is_active());
    cJSON_AddBoolToObject(overlays, "voice",    ui_voice_is_visible());
    cJSON_AddBoolToObject(overlays, "settings", ui_settings_is_visible());
    cJSON_AddItemToObject(root, "overlays", overlays);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── Voice endpoint family extracted to debug_server_voice.c (Wave 23b, #332) ─ */


/* ── #266: live video streaming control (POST /video/start, /video/stop,
 *           GET /video).  #268 adds /video/show + /video/hide for the
 *           downlink pane. ──────────────────────────────────────────── */
#include "voice_video.h"
#include "ui_video_pane.h"
#include "ui_core.h"   /* tab5_lv_async_call (#258) */
static esp_err_t send_json_resp(httpd_req_t *req, cJSON *root);

/* Video + call control endpoints (12 handlers) extracted to
 * debug_server_call.c — Wave 23b (#332) tenth per-family extract.
 * Registered via debug_server_call_register() below. */

/* ── Wave 12 observability: /heap endpoint ─────────────────────────────
 *
 * Exposes the per-pool heap state + last reboot reason in one call so
 * dashboards and post-mortem scripts can track device health over time.
 * This pairs with the wave 11 coredump path — if last_reboot_reason is
 * something like "abort" or a heap_wd string, the coredump partition
 * at 0x620000 has a forensic dump available via
 *     esptool read_flash 0x620000 0x40000 cd.bin
 *     espcoredump.py info_corefile -t elf ... cd.bin
 *
 * No auth-gate — the debug server overall requires auth per
 * reference_tab5_debug_access.md, but /heap is READ-ONLY and useful
 * to have behind the same bearer token.
 */
esp_err_t heap_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    extern void media_cache_stats(int *used_slots, unsigned *resident_kb);
    int mc_used = 0; unsigned mc_kb = 0;
    media_cache_stats(&mc_used, &mc_kb);

    size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t int_free      = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t int_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t dma_free      = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t dma_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    lv_mem_monitor_t lvgl;
    lv_mem_monitor(&lvgl);

    const char *reset_str = "unknown";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   reset_str = "poweron"; break;
        case ESP_RST_EXT:       reset_str = "external_pin"; break;
        case ESP_RST_SW:        reset_str = "esp_restart"; break;
        case ESP_RST_PANIC:     reset_str = "panic_abort"; break;   /* wave 11 coredump path */
        case ESP_RST_INT_WDT:   reset_str = "int_wdt"; break;
        case ESP_RST_TASK_WDT:  reset_str = "task_wdt"; break;
        case ESP_RST_WDT:       reset_str = "other_wdt"; break;
        case ESP_RST_DEEPSLEEP: reset_str = "deepsleep_wake"; break;
        case ESP_RST_BROWNOUT:  reset_str = "brownout"; break;
        case ESP_RST_SDIO:      reset_str = "sdio"; break;
        default:                reset_str = "unknown"; break;
    }

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime_ms\":%llu,\"reset_reason\":\"%s\","
        "\"psram\":{\"free_kb\":%u,\"largest_kb\":%u},"
        "\"internal\":{\"free_kb\":%u,\"largest_kb\":%u,\"frag_pct\":%d},"
        "\"dma\":{\"free_kb\":%u,\"largest_kb\":%u},"
        "\"lvgl\":{\"used_kb\":%u,\"free_kb\":%u,\"frag_pct\":%u},"
        "\"media_cache\":{\"slots_used\":%d,\"resident_kb\":%u},"
        "\"coredump_available\":%s}",
        (unsigned long long)(esp_timer_get_time() / 1000),
        reset_str,
        (unsigned)(psram_free / 1024), (unsigned)(psram_largest / 1024),
        (unsigned)(int_free / 1024), (unsigned)(int_largest / 1024),
        int_free ? (int)(100 - (int_largest * 100 / int_free)) : 0,
        (unsigned)(dma_free / 1024), (unsigned)(dma_largest / 1024),
        (unsigned)((lvgl.total_size - lvgl.free_size) / 1024),
        (unsigned)(lvgl.free_size / 1024), (unsigned)lvgl.frag_pct,
        mc_used, mc_kb,
        (esp_core_dump_image_check() == ESP_OK) ? "true" : "false");
    (void)n;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── Full self-test endpoint ──────────────────────────────────────────── */

static esp_err_t selftest_handler(httpd_req_t *req)
{
    /* GET /selftest — run through all subsystem checks and return results */
    cJSON *root = cJSON_CreateObject();
    cJSON *tests = cJSON_AddArrayToObject(root, "tests");
    int pass = 0, fail = 0;

    /* WiFi */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = tab5_wifi_connected();
        cJSON_AddStringToObject(t, "name", "wifi");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* Voice WS connected */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = voice_is_connected();
        cJSON_AddStringToObject(t, "name", "voice_ws");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* SD card */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = tab5_sdcard_mounted();
        cJSON_AddStringToObject(t, "name", "sd_card");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* PSRAM available (>1MB) + fragmentation check */
    {
        cJSON *t = cJSON_CreateObject();
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        bool ok = free_psram > (1 * 1024 * 1024);
        cJSON_AddStringToObject(t, "name", "psram");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddNumberToObject(t, "free_mb", (double)(free_psram / 1024 / 1024));
        cJSON_AddNumberToObject(t, "largest_free_block_kb", (double)(largest_block / 1024));
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* Display initialized (if we got this far, display is running) */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = true;  /* If debug server is responding, display is up */
        cJSON_AddStringToObject(t, "name", "display");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* Camera initialized */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = tab5_camera_initialized();
        cJSON_AddStringToObject(t, "name", "camera");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* Internal heap — free + fragmentation */
    {
        cJSON *t = cJSON_CreateObject();
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        int frag_pct = (free_internal > 0) ? (int)(100 - (largest_internal * 100 / free_internal)) : 0;
        bool ok = free_internal > (30 * 1024) && largest_internal > (8 * 1024);
        cJSON_AddStringToObject(t, "name", "internal_heap");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddNumberToObject(t, "free_kb", (double)(free_internal / 1024));
        cJSON_AddNumberToObject(t, "largest_block_kb", (double)(largest_internal / 1024));
        cJSON_AddNumberToObject(t, "fragmentation_pct", (double)frag_pct);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* NVS settings readable */
    {
        cJSON *t = cJSON_CreateObject();
        char host[64] = {0};
        tab5_settings_get_dragon_host(host, sizeof(host));
        bool ok = (host[0] != '\0');
        cJSON_AddStringToObject(t, "name", "nvs_settings");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddStringToObject(t, "dragon_host", host);
        cJSON_AddNumberToObject(t, "nvs_writes_this_session",
                                (double)tab5_settings_get_nvs_write_count());
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    cJSON_AddNumberToObject(root, "passed", pass);
    cJSON_AddNumberToObject(root, "failed", fail);
    cJSON_AddNumberToObject(root, "total", pass + fail);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ==================================================================== */
/*  #149 PR β — new capability endpoints                                */
/* ==================================================================== */

#include "debug_obs.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "chat_msg_store.h"
#include "audio.h"
#include "battery.h"
#include "display.h"

/* Shared JSON response helper used by the new handlers.  Takes ownership
 * of `root` (Delete'd here). */
static esp_err_t send_json_resp(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json print failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    return r;
}

/* Wave 23b (#332): public wrapper for the per-family extracts —
 * see debug_server_internal.h.  The 57 in-file callers still use
 * the static; new family files go through this. */
esp_err_t tab5_debug_send_json_resp(httpd_req_t *req, struct cJSON *root) { return send_json_resp(req, (cJSON *)root); }

/* ── GET /tasks — FreeRTOS task snapshot ────────────────────────── */
/* Full per-task dump requires configUSE_TRACE_FACILITY + the
 * run-time-stats infra to be enabled in sdkconfig.  Most ESP-IDF
 * projects leave this off to save ~96 B per task; degrade gracefully. */
static esp_err_t tasks_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    cJSON *root  = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", (double)uxTaskGetNumberOfTasks());

#if (configUSE_TRACE_FACILITY == 1) && (configGENERATE_RUN_TIME_STATS == 1)
    UBaseType_t n = uxTaskGetNumberOfTasks();
    size_t slots = n + 8;
    TaskStatus_t *status = heap_caps_malloc(slots * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (status) {
        uint32_t total_runtime = 0;
        UBaseType_t got = uxTaskGetSystemState(status, slots, &total_runtime);
        cJSON *arr = cJSON_AddArrayToObject(root, "tasks");
        cJSON_AddNumberToObject(root, "total_runtime", (double)total_runtime);
        const char *state_names[] = {"running","ready","blocked","suspended","deleted","invalid"};
        for (UBaseType_t i = 0; i < got; i++) {
            const TaskStatus_t *t = &status[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name",     t->pcTaskName ? t->pcTaskName : "");
            cJSON_AddNumberToObject(o, "prio",     t->uxCurrentPriority);
            cJSON_AddStringToObject(o, "state",
                t->eCurrentState <= eInvalid ? state_names[t->eCurrentState] : "?");
            cJSON_AddNumberToObject(o, "stack_free",
                                    (double)(t->usStackHighWaterMark * sizeof(StackType_t)));
            cJSON_AddNumberToObject(o, "runtime",  (double)t->ulRunTimeCounter);
            cJSON_AddItemToArray(arr, o);
        }
        heap_caps_free(status);
    }
#else
    cJSON_AddStringToObject(root, "note",
        "per-task dump requires configUSE_TRACE_FACILITY + "
        "configGENERATE_RUN_TIME_STATS in sdkconfig");
#endif
    return send_json_resp(req, root);
}

/* ── GET /logs/tail?n=100 ───────────────────────────────────────── */
static esp_err_t logs_tail_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    int n = 100;
    char q[64] = {0}, v[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
        && httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
        int x = atoi(v);
        if (x > 0 && x <= 5000) n = x;
    }
    size_t out_len = 0;
    char *tail = tab5_debug_obs_log_tail(n, &out_len);
    if (!tail) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no log buffer");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, tail, out_len);
    heap_caps_free(tail);
    return ESP_OK;
}

/* ── POST /voice/text — send Dragon text, no 200-byte limit ────── */
static esp_err_t voice_text_handler(httpd_req_t *req)
{
    /* α already fixed /chat to heap-allocate — /voice/text is an
     * explicit alias so the REST surface reads cleaner.  Forward
     * wholesale to the same handler. */
    return chat_handler(req);
}

/* /wifi/status extracted to debug_server_wifi.c (Wave 23b, #332). */

/* ── GET /battery ──────────────────────────────────────────────── */
static esp_err_t battery_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    tab5_battery_info_t bat = {0};
    esp_err_t r = tab5_battery_read(&bat);
    cJSON *root = cJSON_CreateObject();
    if (r == ESP_OK) {
        cJSON_AddNumberToObject(root, "voltage",  (double)bat.voltage);
        cJSON_AddNumberToObject(root, "current",  (double)bat.current);
        cJSON_AddNumberToObject(root, "power",    (double)bat.power);
        cJSON_AddNumberToObject(root, "percent",  (double)bat.percent);
        cJSON_AddBoolToObject(root,   "charging", bat.charging);
    } else {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
    }
    return send_json_resp(req, root);
}

/* ── POST /display/brightness?pct=0..100 ───────────────────────── */
static esp_err_t display_brightness_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char q[32] = {0}, v[8] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "pct", v, sizeof(v));
    int pct = atoi(v);
    if (pct < 0 || pct > 100) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", "pct must be 0..100");
        return send_json_resp(req, r);
    }
    esp_err_t er = tab5_settings_set_brightness((uint8_t)pct);
    /* Apply live via display driver so the user sees the change without
     * needing to open the Settings screen. */
    tab5_display_set_brightness(pct);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", er == ESP_OK);
    cJSON_AddNumberToObject(resp, "brightness", pct);
    tab5_debug_obs_event("display.brightness", v);
    return send_json_resp(req, resp);
}

/* ── GET/POST /audio ───────────────────────────────────────────── */
static esp_err_t audio_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (req->method == HTTP_POST) {
        char q[64] = {0}, v[16] = {0};
        httpd_req_get_url_query_str(req, q, sizeof(q));

        /* action=volume&pct=50 | action=mute&on=0|1 */
        char action[16] = {0};
        httpd_query_key_value(q, "action", action, sizeof(action));
        cJSON *resp = cJSON_CreateObject();
        if (strcmp(action, "volume") == 0) {
            httpd_query_key_value(q, "pct", v, sizeof(v));
            int pct = atoi(v);
            if (pct < 0 || pct > 100) {
                cJSON_AddStringToObject(resp, "error", "pct must be 0..100");
            } else {
                tab5_settings_set_volume((uint8_t)pct);
                tab5_audio_set_volume((uint8_t)pct);
                cJSON_AddBoolToObject(resp, "ok", true);
                cJSON_AddNumberToObject(resp, "volume", pct);
                tab5_debug_obs_event("audio.volume", v);
            }
        } else if (strcmp(action, "mute") == 0) {
            httpd_query_key_value(q, "on", v, sizeof(v));
            int on = atoi(v);
            tab5_settings_set_mic_mute(on ? 1 : 0);
            cJSON_AddBoolToObject(resp, "ok", true);
            cJSON_AddBoolToObject(resp, "mic_mute", on != 0);
            tab5_debug_obs_event("audio.mic_mute", v);
        } else {
            cJSON_AddStringToObject(resp, "error", "action must be volume|mute");
        }
        return send_json_resp(req, resp);
    }

    /* GET — current state. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "volume",   tab5_settings_get_volume());
    cJSON_AddNumberToObject(root, "mic_mute", tab5_settings_get_mic_mute());
    return send_json_resp(req, root);
}

/* ── GET /metrics — Prometheus text format ─────────────────────── */
static esp_err_t metrics_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    size_t int_free   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t int_lrg    = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t ps_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_lrg     = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    lv_mem_monitor_t lv_m; lv_mem_monitor(&lv_m);
    tab5_battery_info_t bat = {0}; tab5_battery_read(&bat);

    char out[1536];
    int n = snprintf(out, sizeof(out),
        "# HELP tab5_uptime_ms Milliseconds since boot\n"
        "# TYPE tab5_uptime_ms counter\n"
        "tab5_uptime_ms %llu\n"
        "# HELP tab5_heap_free_bytes Free heap per pool\n"
        "# TYPE tab5_heap_free_bytes gauge\n"
        "tab5_heap_free_bytes{pool=\"internal\"} %u\n"
        "tab5_heap_free_bytes{pool=\"psram\"} %u\n"
        "tab5_heap_free_bytes{pool=\"lvgl\"} %u\n"
        "tab5_heap_largest_bytes{pool=\"internal\"} %u\n"
        "tab5_heap_largest_bytes{pool=\"psram\"} %u\n"
        "# HELP tab5_wifi_connected 1 if STA associated\n"
        "# TYPE tab5_wifi_connected gauge\n"
        "tab5_wifi_connected %d\n"
        "tab5_voice_connected %d\n"
        "tab5_voice_mode %u\n"
        "tab5_voice_state %u\n"
        "tab5_fps_lvgl %lu\n"
        "tab5_battery_percent %u\n"
        "tab5_battery_voltage %.3f\n"
        "tab5_battery_current %.3f\n"
        "tab5_nvs_writes %lu\n",
        (unsigned long long)(esp_timer_get_time() / 1000),
        (unsigned)int_free, (unsigned)ps_free, (unsigned)lv_m.free_size,
        (unsigned)int_lrg, (unsigned)ps_lrg,
        tab5_wifi_connected() ? 1 : 0,
        voice_is_connected() ? 1 : 0,
        tab5_settings_get_voice_mode(),
        (unsigned)voice_get_state(),
        (unsigned long)ui_core_get_fps(),
        bat.percent, bat.voltage, bat.current,
        (unsigned long)tab5_settings_get_nvs_write_count());
    (void)n;
    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* ── GET /events?since=<ms> ─────────────────────────────────────── */
static esp_err_t events_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    uint64_t since = 0;
    char q[48] = {0}, v[24] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
        && httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
        since = (uint64_t)strtoull(v, NULL, 10);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = tab5_debug_obs_events_json(since);
    cJSON_AddItemToObject(root, "events", arr);
    return send_json_resp(req, root);
}

/* #296 follow-up: a `block_until=<kind>&timeout_ms=N` long-poll
 * variant of /events was experimentally added and reverted.  ESP-IDF
 * httpd is single-task — the long-poll vTaskDelay blocked all other
 * debug-server requests for the wait duration, and a multi-minute
 * test run accumulated enough heap churn from the per-iteration
 * cJSON alloc/free to crash the device with a PANIC reset.  Polling
 * /events at 250-500 ms from the harness side is the safe pattern. */

/* ── GET /heap/history?n=60 ─────────────────────────────────────── */
static esp_err_t heap_history_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    int n = 60;
    char q[32] = {0}, v[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
        && httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
        int x = atoi(v); if (x > 0) n = x;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "samples", tab5_debug_obs_heap_json(n));
    return send_json_resp(req, root);
}

/* ── GET /chat/messages?n=50 ────────────────────────────────────── */
static esp_err_t chat_messages_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    int n = 50;
    char q[32] = {0}, v[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
        && httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
        int x = atoi(v); if (x > 0 && x <= 500) n = x;
    }
    int total = chat_store_count();
    if (n > total) n = total;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "messages");
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "returned", n);

    /* Return the last `n` messages, oldest first. */
    for (int i = total - n; i < total; i++) {
        const chat_msg_t *m = chat_store_get(i);
        if (!m) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "role", m->is_user ? "user" : "assistant");
        const char *types[] = {"text","image","card","audio","system"};
        cJSON_AddStringToObject(o, "type",
            (int)m->type < (int)(sizeof(types)/sizeof(types[0])) ? types[m->type] : "?");
        cJSON_AddStringToObject(o, "text", m->text);
        if (m->media_url[0]) cJSON_AddStringToObject(o, "media_url", m->media_url);
        if (m->subtitle[0])  cJSON_AddStringToObject(o, "subtitle",  m->subtitle);
        cJSON_AddNumberToObject(o, "timestamp", (double)m->timestamp);
        if (m->receipt_mils > 0) {
            cJSON *rcpt = cJSON_AddObjectToObject(o, "receipt");
            cJSON_AddNumberToObject(rcpt, "mils",        m->receipt_mils);
            cJSON_AddNumberToObject(rcpt, "prompt_tok",  m->receipt_ptok);
            cJSON_AddNumberToObject(rcpt, "compl_tok",   m->receipt_ctok);
            cJSON_AddStringToObject(rcpt, "model",       m->receipt_model_short);
            cJSON_AddBoolToObject(rcpt,   "retried",     m->receipt_retried);
        }
        cJSON_AddItemToArray(arr, o);
    }
    return send_json_resp(req, root);
}

/* ── GET /keyboard/layout ───────────────────────────────────────
 * Issue #161: returns the live keyboard key positions so tests can
 * derive tap coords instead of hardcoding them in a memory file that
 * rots on layout drift.  Empty array when keyboard isn't built yet. */
#include "ui_keyboard.h"
static esp_err_t keyboard_layout_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON *keys = cJSON_AddArrayToObject(root, "keys");
    cJSON_AddBoolToObject(root, "visible", ui_keyboard_is_visible());
    /* Cap matches the busiest layer (letters has ~33 keys). */
    enum { CAP = 48 };
    static ui_keyboard_key_info_t buf[CAP];
    int n = ui_keyboard_dump_layout(buf, CAP);
    cJSON_AddNumberToObject(root, "count", n);
    static const char *kTypeName[] = {
        "char", "backspace", "shift", "enter", "space", "layer",
    };
    for (int i = 0; i < n && i < CAP; i++) {
        cJSON *k = cJSON_CreateObject();
        cJSON_AddStringToObject(k, "label", buf[i].label);
        cJSON_AddNumberToObject(k, "x",  buf[i].x);
        cJSON_AddNumberToObject(k, "y",  buf[i].y);
        cJSON_AddNumberToObject(k, "w",  buf[i].w);
        cJSON_AddNumberToObject(k, "h",  buf[i].h);
        /* center-of-key tap targets — most useful for scripts */
        cJSON_AddNumberToObject(k, "cx", buf[i].x + buf[i].w / 2);
        cJSON_AddNumberToObject(k, "cy", buf[i].y + buf[i].h / 2);
        const char *tn = (buf[i].type < (sizeof(kTypeName) / sizeof(*kTypeName)))
                       ? kTypeName[buf[i].type] : "unknown";
        cJSON_AddStringToObject(k, "type", tn);
        cJSON_AddItemToArray(keys, k);
    }
    return send_json_resp(req, root);
}

/* ── POST /chat/llm_done?text=<> ────────────────────────────────
 * #78 + #160 verification: feed `text` through the same
 * md_strip_tool_markers + ui_chat_push_message path voice.c's
 * llm_done handler runs.  Verifies the Tab5-side defensive scrub
 * without needing Dragon to actually emit a tool-call. */
extern void md_strip_tool_markers(const char *in, char *out, size_t out_cap);
static void url_pct_decode_inplace(char *s);   /* defined further below */
static esp_err_t chat_llm_done_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char q[1024] = {0}, text[800] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "text", text, sizeof(text));
    url_pct_decode_inplace(text);
    cJSON *root = cJSON_CreateObject();
    if (!text[0]) {
        cJSON_AddStringToObject(root, "error", "need ?text=<llm response>");
        return send_json_resp(req, root);
    }
    char clean[800];
    md_strip_tool_markers(text, clean, sizeof(clean));
    if (clean[0]) {
        ui_chat_push_message("assistant", clean);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "raw", text);
    cJSON_AddStringToObject(root, "after_strip", clean);
    return send_json_resp(req, root);
}

/* ── POST /chat/partial?text=<> ─────────────────────────────────
 * U12 (#206) verification helper: shove a string into the live
 * STT-partial caption above the chat input pill.  Empty text hides. */
extern void ui_chat_show_partial(const char *partial);
static void url_pct_decode_inplace(char *s);   /* defined further below */
static esp_err_t chat_partial_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char q[256] = {0}, text[160] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "text", text, sizeof(text));
    url_pct_decode_inplace(text);
    ui_chat_show_partial(text[0] ? text : NULL);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "text", text);
    return send_json_resp(req, root);
}

/* ── POST /tool_log/push?name=&detail=&ms= ──────────────────────
 * U7+U8 (#206) verification helper: forge a tool_log event so the
 * agents/focus surfaces can be exercised without a live Dragon LLM
 * tool_call producer.  When `ms` is provided, push a "done" entry;
 * otherwise push a "running" entry.  Also accepts mode=both to push
 * a paired call+result. */
extern void tool_log_push_call(const char *name, const char *detail);
extern void tool_log_push_result(const char *name, uint32_t exec_ms);

static esp_err_t tool_log_push_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char q[256] = {0}, name[32] = {0}, detail[96] = {0}, ms_s[12] = {0}, mode[12] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "name",   name,   sizeof(name));
    httpd_query_key_value(q, "detail", detail, sizeof(detail));
    httpd_query_key_value(q, "ms",     ms_s,   sizeof(ms_s));
    httpd_query_key_value(q, "mode",   mode,   sizeof(mode));
    url_pct_decode_inplace(name);
    url_pct_decode_inplace(detail);

    cJSON *root = cJSON_CreateObject();
    if (!name[0]) {
        cJSON_AddStringToObject(root, "error", "need ?name=<tool>");
        return send_json_resp(req, root);
    }
    uint32_t ms = (uint32_t)atoi(ms_s);
    bool both = (strcmp(mode, "both") == 0);

    if (both) {
        tool_log_push_call(name, detail);
        tool_log_push_result(name, ms);
    } else if (ms_s[0]) {
        tool_log_push_result(name, ms);
    } else {
        tool_log_push_call(name, detail);
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "mode", both ? "both" : (ms_s[0] ? "result" : "call"));
    cJSON_AddNumberToObject(root, "ms", ms);
    return send_json_resp(req, root);
}

/* ── POST /chat/audio_clip?url=<>&label=<> ─────────────────────────
 * U5 (#206) verification helper: pushes an audio_clip into the chat
 * store so the tap-to-play path can be exercised without a live Dragon
 * audio_clip producer.  url= is required and is passed verbatim to
 * ui_chat_push_audio_clip (relative paths are resolved against the
 * configured Dragon host on tap).  Note: the chat overlay must be open
 * for the row to render — POST /navigate?screen=chat first. */
static void url_pct_decode_inplace(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hi = r[1], lo = r[2];
            int hv = (hi >= '0' && hi <= '9') ? hi - '0'
                   : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                   : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
            int lv = (lo >= '0' && lo <= '9') ? lo - '0'
                   : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                   : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
            if (hv >= 0 && lv >= 0) {
                *w++ = (char)((hv << 4) | lv);
                r += 3;
                continue;
            }
        }
        if (*r == '+') { *w++ = ' '; r++; continue; }
        *w++ = *r++;
    }
    *w = 0;
}

static esp_err_t chat_audio_clip_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char q[512] = {0}, url[256] = {0}, label[96] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "url",   url,   sizeof(url));
    httpd_query_key_value(q, "label", label, sizeof(label));
    url_pct_decode_inplace(url);
    url_pct_decode_inplace(label);

    cJSON *root = cJSON_CreateObject();
    if (!url[0]) {
        cJSON_AddStringToObject(root, "error", "need ?url=<dragon-relative or absolute>");
        return send_json_resp(req, root);
    }
    ui_chat_push_audio_clip(url, 0.0f, label[0] ? label : "test clip");
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "url", url);
    cJSON_AddStringToObject(root, "label", label[0] ? label : "test clip");
    return send_json_resp(req, root);
}

/* ── GET /net/ping?host=<>&port=<> ──────────────────────────────── */
/* Re-implements the non-blocking probe locally so we don't leak
 * voice.c internals.  Matches the fix from #146. */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <fcntl.h>
#include <errno.h>
static esp_err_t ping_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char q[128] = {0}, host[64] = {0}, port_s[8] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "host", host, sizeof(host));
    httpd_query_key_value(q, "port", port_s, sizeof(port_s));
    int port = atoi(port_s);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "host", host);
    cJSON_AddNumberToObject(root, "port", port);
    if (!host[0] || port <= 0 || port > 65535) {
        cJSON_AddStringToObject(root, "error", "need host and port=1..65535");
        return send_json_resp(req, root);
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char ps[8]; snprintf(ps, sizeof(ps), "%d", port);
    int gai = getaddrinfo(host, ps, &hints, &res);
    if (gai != 0 || !res) {
        cJSON_AddStringToObject(root, "error", "dns failed");
        if (res) freeaddrinfo(res);
        return send_json_resp(req, root);
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res);
        cJSON_AddStringToObject(root, "error", "socket failed");
        return send_json_resp(req, root);
    }
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int64_t t0 = esp_timer_get_time();
    bool ok = false;
    int cr = connect(s, res->ai_addr, res->ai_addrlen);
    if (cr == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int n = select(s + 1, NULL, &wfds, NULL, &tv);
        if (n > 0 && FD_ISSET(s, &wfds)) {
            int soerr = 0; socklen_t le = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &le);
            ok = (soerr == 0);
        }
    }
    int64_t elapsed_us = esp_timer_get_time() - t0;
    close(s);
    freeaddrinfo(res);

    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddNumberToObject(root, "elapsed_ms", (double)(elapsed_us / 1000));
    return send_json_resp(req, root);
}

/* ── POST /nvs/erase family extracted to debug_server_settings.c
 *    (Wave 23b, #332) — handler + register live there now. ─────────── */

/* TT #328 Wave 12 — async banner trampoline for /debug/inject_error.
 * Fires on the LVGL thread; takes ownership of the heap-allocated
 * argument struct. */
typedef struct {
   char txt[96];
} debug_inject_banner_arg_t;
void debug_inject_banner_async(void *arg) {
   debug_inject_banner_arg_t *p = (debug_inject_banner_arg_t *)arg;
   if (!p) return;
   extern void ui_home_show_error_banner(const char *, void (*)(void));
   ui_home_show_error_banner(p->txt, NULL /* non-dismissable */);
   free(p);
}

/* TT #328 Wave 13 — /debug/inject_audio.  Bypass mic + pump caller-
 * supplied PCM through the same WS binary path the mic task uses.
 * Lets the harness validate STT round-trip without canned-mic setup.
 * Wire: raw POST body = 16 kHz mono int16 LE PCM, 1..64 KB. */
static esp_err_t debug_inject_audio_handler_impl(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   if (req->content_len <= 0 || req->content_len > 65536) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "POST body must be 16 kHz mono int16 PCM, 1..64 KB");
      return send_json_resp(req, r);
   }
   uint8_t *pcm = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
   if (!pcm) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "PSRAM alloc failed");
      return send_json_resp(req, r);
   }
   int total = 0;
   while (total < req->content_len) {
      int n = httpd_req_recv(req, (char *)pcm + total, req->content_len - total);
      if (n <= 0) break;
      total += n;
   }
   if (total != req->content_len) {
      heap_caps_free(pcm);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "short read");
      return send_json_resp(req, r);
   }
   extern esp_err_t voice_start_listening(void);
   extern esp_err_t voice_stop_listening(void);
   extern esp_err_t voice_ws_send_binary_public(const void *data, size_t len);
   esp_err_t e = voice_start_listening();
   if (e != ESP_OK) {
      heap_caps_free(pcm);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "voice_start_listening failed");
      cJSON_AddStringToObject(r, "esp_err", esp_err_to_name(e));
      return send_json_resp(req, r);
   }
   vTaskDelay(pdMS_TO_TICKS(60));
   /* 16 kHz × 20 ms = 320 samples × 2 = 640 B/frame, 20 ms cadence. */
   const size_t frame_bytes = 640;
   int frames_sent = 0;
   int bytes_sent = 0;
   for (int off = 0; off < total; off += frame_bytes) {
      size_t chunk = total - off;
      if (chunk > frame_bytes) chunk = frame_bytes;
      if (voice_ws_send_binary_public(pcm + off, chunk) == ESP_OK) {
         frames_sent++;
         bytes_sent += chunk;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
   }
   vTaskDelay(pdMS_TO_TICKS(80));
   voice_stop_listening();
   heap_caps_free(pcm);
   cJSON *r = cJSON_CreateObject();
   cJSON_AddBoolToObject(r, "ok", true);
   cJSON_AddNumberToObject(r, "frames_sent", frames_sent);
   cJSON_AddNumberToObject(r, "bytes_sent", bytes_sent);
   tab5_debug_obs_event("debug.inject_audio", "done");
   return send_json_resp(req, r);
}

esp_err_t debug_inject_audio_handler(httpd_req_t *req) { return debug_inject_audio_handler_impl(req); }

/* ── Codec endpoint family extracted to debug_server_codec.c (Wave 23b, #332) ─ */

/* TT #328 Wave 12 — synthesize Wave 3 error.* events + their banner/
 * toast UX without needing real fault conditions.  POST body or query
 * params:
 *
 *   {"class": "dragon" | "auth" | "ota" | "sd" | "k144",
 *    "detail": "<obs detail>",
 *    "banner": true|false,
 *    "banner_text": "<optional override>"}
 *
 * Always fires the obs event "error.<class> <detail>".  When
 * banner=true (default), also shows the persistent error banner with
 * banner_text.  Returns {"ok":true,"fired":"error.<class>"}. */
static esp_err_t debug_inject_error_handler_impl(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   char body[256] = {0};
   int total = 0;
   if (req->content_len > 0 && req->content_len < (int)sizeof(body)) {
      int n = httpd_req_recv(req, body, req->content_len);
      total = n > 0 ? n : 0;
   }
   body[total] = '\0';

   char err_class[16] = {0};
   char err_detail[40] = {0};
   char banner_text[96] = {0};
   bool show_banner = true;

   /* Body JSON parse if present, else fall through to query params. */
   if (body[0] == '{') {
      cJSON *root = cJSON_Parse(body);
      if (root) {
         cJSON *c = cJSON_GetObjectItem(root, "class");
         cJSON *d = cJSON_GetObjectItem(root, "detail");
         cJSON *b = cJSON_GetObjectItem(root, "banner");
         cJSON *t = cJSON_GetObjectItem(root, "banner_text");
         if (cJSON_IsString(c)) strncpy(err_class, c->valuestring, sizeof(err_class) - 1);
         if (cJSON_IsString(d)) strncpy(err_detail, d->valuestring, sizeof(err_detail) - 1);
         if (cJSON_IsBool(b)) show_banner = cJSON_IsTrue(b);
         if (cJSON_IsString(t)) strncpy(banner_text, t->valuestring, sizeof(banner_text) - 1);
         cJSON_Delete(root);
      }
   }
   if (!err_class[0]) {
      char q[128] = {0};
      httpd_req_get_url_query_str(req, q, sizeof(q));
      httpd_query_key_value(q, "class", err_class, sizeof(err_class));
      httpd_query_key_value(q, "detail", err_detail, sizeof(err_detail));
   }
   if (!err_class[0]) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "missing class (dragon|auth|ota|sd|k144)");
      return send_json_resp(req, r);
   }

   char kind[32];
   snprintf(kind, sizeof(kind), "error.%s", err_class);
   tab5_debug_obs_event(kind, err_detail[0] ? err_detail : "synthetic");

   if (show_banner) {
      const char *txt = banner_text[0] ? banner_text : "Synthetic error injection (debug)";
      debug_inject_banner_arg_t *arg = malloc(sizeof(*arg));
      if (arg) {
         strncpy(arg->txt, txt, sizeof(arg->txt) - 1);
         arg->txt[sizeof(arg->txt) - 1] = '\0';
         /* Marshalled to LVGL thread.  Static cb avoids GCC nested-
          * function trampoline (project avoids those per LEARNINGS). */
         extern lv_result_t tab5_lv_async_call(lv_async_cb_t cb, void *user_data);
         tab5_lv_async_call(debug_inject_banner_async, arg);
      }
   }

   cJSON *r = cJSON_CreateObject();
   cJSON_AddBoolToObject(r, "ok", true);
   cJSON_AddStringToObject(r, "fired", kind);
   cJSON_AddStringToObject(r, "detail", err_detail);
   cJSON_AddBoolToObject(r, "banner", show_banner);
   return send_json_resp(req, r);
}

esp_err_t debug_inject_error_handler(httpd_req_t *req) { return debug_inject_error_handler_impl(req); }

/* TT #328 Wave 2 — POST /debug/inject_ws.  Body is a JSON string that
 * gets routed through voice.c's WS text dispatcher as if Dragon had
 * sent it.  Lets the e2e harness exercise toast/banner UX paths
 * without needing Dragon to genuinely fail (e.g. fire a fake
 * config_update.error → assert banner → fire a fake llm_done → assert
 * banner cleared).  Body size capped at 2 KB; bigger frames don't
 * fit through the WS path either. */
static esp_err_t debug_inject_ws_handler_impl(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   if (req->content_len <= 0 || req->content_len > 2048) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "POST body must be JSON, 1..2048 bytes");
      return send_json_resp(req, r);
   }
   char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!buf) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "PSRAM alloc failed");
      return send_json_resp(req, r);
   }
   int total = 0;
   while (total < req->content_len) {
      int n = httpd_req_recv(req, buf + total, req->content_len - total);
      if (n <= 0) break;
      total += n;
   }
   buf[total] = '\0';
   if (total != req->content_len) {
      heap_caps_free(buf);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "short read");
      return send_json_resp(req, r);
   }
   extern void voice_debug_inject_text(const char *data, int len);
   voice_debug_inject_text(buf, total);
   heap_caps_free(buf);
   cJSON *r = cJSON_CreateObject();
   cJSON_AddBoolToObject(r, "ok", true);
   cJSON_AddNumberToObject(r, "bytes", total);
   tab5_debug_obs_event("debug.inject_ws", "done");
   return send_json_resp(req, r);
}

esp_err_t debug_inject_ws_handler(httpd_req_t *req) { return debug_inject_ws_handler_impl(req); }

/* ======================================================================== */
/*  Server init                                                              */
/* ======================================================================== */

esp_err_t tab5_debug_server_init(void)
{
    if (!tab5_wifi_connected()) {
        ESP_LOGW(TAG, "WiFi not connected — debug server not started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Initialize bearer token auth (generate on first boot, load from NVS thereafter) */
    init_auth_token();

    /* Eagerly create the hardware JPEG encoder once during server init.
     * Lazy per-request init races when multiple sockets arrive before
     * s_jpeg_enc is populated — both tasks try to init, the second one
     * crashes in jpeg_release_codec_handle(NULL). */
    (void)_ensure_jpeg_encoder();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DEBUG_PORT;
    config.stack_size  = 12288;
    /* TT #328 Wave 12: bumped 56 → 72.  The 56 cap was set in #149
     * for the PR-β endpoints; subsequent Waves added /m5 (#317),
     * /events, /chat/messages, /chat/audio_clip, /chat/partial,
     * /chat/llm_done, /tool_log/push, /keyboard/layout, /net/ping,
     * /nvs/erase, /debug/inject_error, etc.  Without the bump, late
     * registrations silently drop with httpd 404. */
    config.max_uri_handlers = 72;
    config.lru_purge_enable = true;
    config.max_open_sockets = 16;         /* Needs headroom for rapid API calls (nav+info pairs) */
    config.recv_wait_timeout = 5;         /* 5s recv timeout (default 5) */
    /* #74: was 90 s — a single stalled /screenshot held the dispatch
     * worker for the full timeout, blocking /info / /touch / /navigate
     * for minutes when WiFi RTT spiked.  Now that /screenshot is JPEG
     * (~30 KB, not 1.8 MB BMP) the original "needs 30-60 s of blocked
     * send()" reasoning no longer applies — 15 s is a comfortable
     * upper bound that drops a wedged client fast and frees the
     * worker for everything else. */
    config.send_wait_timeout = 15;
    /* #74: TCP_NODELAY on accepted sockets — disables Nagle so small
     * JSON responses (/info / /touch / /heap) don't sit in the kernel
     * waiting on the 200 ms tail when paired with a chunked response
     * stream from another in-flight handler. */
    extern esp_err_t debug_open_fn(httpd_handle_t hd, int sockfd);
    config.open_fn  = debug_open_fn;
    config.close_fn = NULL;               /* Use default close */
    /* Run httpd on Core 1 so it doesn't starve when LVGL is busy on Core 0.
     * Settings screen creates 55 objects (~500ms) which blocks Core 0 entirely.
     * Without this, /info requests time out during heavy LVGL rendering. */
    config.core_id = 1;
    config.task_priority = tskIDLE_PRIORITY + 6;  /* Above LVGL (prio 5) */

    if (s_httpd != NULL) {
        /* Already running — treat as idempotent.  Caller should stop
         * first if they want a restart.  Returning ESP_OK preserves
         * the old pre-W15-C02 behaviour where a second call was a
         * benign no-op (would have leaked a second server). */
        ESP_LOGW(TAG, "debug server already running, skipping re-init");
        return ESP_OK;
    }

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start debug server: %s", esp_err_to_name(ret));
        return ret;
    }
    s_httpd = server;

    /* Register URI handlers */
    const httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler
    };
    const httpd_uri_t uri_screenshot = {
        .uri = "/screenshot", .method = HTTP_GET, .handler = screenshot_handler
    };
    /* #148: /screenshot.bmp alias removed — the handler always returns
     * JPEG (hardware JPEG encoder), so the .bmp URL was lying to callers.
     * Use /screenshot or /screenshot.jpg. */
    const httpd_uri_t uri_screenshot_jpg = {
        .uri = "/screenshot.jpg", .method = HTTP_GET, .handler = screenshot_handler
    };
    const httpd_uri_t uri_info = {
        .uri = "/info", .method = HTTP_GET, .handler = info_handler
    };
    const httpd_uri_t uri_touch = {
        .uri = "/touch", .method = HTTP_POST, .handler = touch_handler
    };
    const httpd_uri_t uri_reboot = {
        .uri = "/reboot", .method = HTTP_POST, .handler = reboot_handler
    };
    const httpd_uri_t uri_log = {
        .uri = "/log", .method = HTTP_GET, .handler = log_handler
    };

    /* #148: /open merged into /navigate (single source of truth for
     * screen list).  /navtouch dropped — same underlying dispatcher. */
    const httpd_uri_t uri_coredump = {
        .uri = "/coredump", .method = HTTP_GET, .handler = coredump_handler
    };
    const httpd_uri_t uri_heap_trace_start = {
        .uri = "/heap_trace_start", .method = HTTP_POST,
        .handler = heap_trace_start_handler
    };
    const httpd_uri_t uri_heap_trace_dump = {
        .uri = "/heap_trace_dump", .method = HTTP_GET,
        .handler = heap_trace_dump_handler
    };
    const httpd_uri_t uri_crashlog = {
        .uri = "/crashlog", .method = HTTP_GET, .handler = crashlog_handler
    };
    const httpd_uri_t uri_sdcard = {
        .uri = "/sdcard", .method = HTTP_GET, .handler = sdcard_handler
    };
    /* #148: /wake removed (feature parked) — /info still reports state. */
    /* Wave 23b (#332): /settings GET+POST + /nvs/erase + /mode all
     * registered en-bloc via the per-family extracted modules
     * (debug_server_settings_register, debug_server_mode_register)
     * below. */
    const httpd_uri_t uri_navigate = {
        .uri = "/navigate", .method = HTTP_POST, .handler = navigate_handler
    };
    const httpd_uri_t uri_widget = {
        .uri = "/widget", .method = HTTP_POST, .handler = widget_handler
    };
    const httpd_uri_t uri_camera = {
        .uri = "/camera", .method = HTTP_GET, .handler = camera_handler
    };
    /* Wave 23b (#332): OTA endpoint family — registered en-bloc via
     * debug_server_ota_register() below. */
    const httpd_uri_t uri_chat = {
        .uri = "/chat", .method = HTTP_POST, .handler = chat_handler
    };
    const httpd_uri_t uri_input_text = {
        .uri = "/input/text", .method = HTTP_POST, .handler = input_text_handler
    };
    const httpd_uri_t uri_screen = {
        .uri = "/screen", .method = HTTP_GET, .handler = screen_state_handler
    };
    /* Wave 23b (#332): voice endpoint family (/voice, /voice/reconnect,
     * /voice/cancel, /voice/clear) registered en-bloc via
     * debug_server_voice_register() below. */
    /* Wave 23b (#332): K144 endpoint family — the 4 URIs (/m5, /m5/reset,
     * /m5/refresh, /m5/models) are registered via debug_server_m5_register()
     * below so the handlers + their cache layers live in their own .c file. */
    /* /video and /call families (12 endpoints) registered en-bloc via
     * debug_server_call_register() below. */
    /* /dictation (POST + GET) registered en-bloc via debug_server_dictation_register() below. */
    /* /wifi/kick registered en-bloc via debug_server_wifi_register() below. */
    const httpd_uri_t uri_selftest = {
        .uri = "/selftest", .method = HTTP_GET, .handler = selftest_handler
    };
    extern esp_err_t heap_handler(httpd_req_t *req);
    const httpd_uri_t uri_heap = {
        .uri = "/heap", .method = HTTP_GET, .handler = heap_handler
    };

    /* #149 PR β — new capability endpoints. */
    const httpd_uri_t uri_tasks          = { .uri = "/tasks",          .method = HTTP_GET,  .handler = tasks_handler };
    const httpd_uri_t uri_logs_tail      = { .uri = "/logs/tail",      .method = HTTP_GET,  .handler = logs_tail_handler };
    const httpd_uri_t uri_voice_text     = { .uri = "/voice/text",     .method = HTTP_POST, .handler = voice_text_handler };
    /* Wave 23b (#332): /voice/cancel + /voice/clear moved to debug_server_voice.c. */
    /* /wifi/status registered en-bloc via debug_server_wifi_register() below. */
    const httpd_uri_t uri_battery        = { .uri = "/battery",        .method = HTTP_GET,  .handler = battery_handler };
    const httpd_uri_t uri_disp_bright    = { .uri = "/display/brightness", .method = HTTP_POST, .handler = display_brightness_handler };
    const httpd_uri_t uri_audio_get      = { .uri = "/audio",          .method = HTTP_GET,  .handler = audio_handler };
    const httpd_uri_t uri_audio_post     = { .uri = "/audio",          .method = HTTP_POST, .handler = audio_handler };
    const httpd_uri_t uri_metrics        = { .uri = "/metrics",        .method = HTTP_GET,  .handler = metrics_handler };
    const httpd_uri_t uri_events         = { .uri = "/events",         .method = HTTP_GET,  .handler = events_handler };
    const httpd_uri_t uri_heap_history   = { .uri = "/heap/history",   .method = HTTP_GET,  .handler = heap_history_handler };
    const httpd_uri_t uri_heap_probe     = { .uri = "/heap/probe-csv", .method = HTTP_GET,  .handler = tab5_pool_probe_http_handler };
    const httpd_uri_t uri_chat_msgs      = { .uri = "/chat/messages",  .method = HTTP_GET,  .handler = chat_messages_handler };
    const httpd_uri_t uri_chat_audio     = { .uri = "/chat/audio_clip",.method = HTTP_POST, .handler = chat_audio_clip_handler };
    const httpd_uri_t uri_tool_push      = { .uri = "/tool_log/push",  .method = HTTP_POST, .handler = tool_log_push_handler };
    const httpd_uri_t uri_chat_partial   = { .uri = "/chat/partial",   .method = HTTP_POST, .handler = chat_partial_handler };
    const httpd_uri_t uri_chat_llm_done  = { .uri = "/chat/llm_done",  .method = HTTP_POST, .handler = chat_llm_done_handler };
    const httpd_uri_t uri_kb_layout      = { .uri = "/keyboard/layout",.method = HTTP_GET,  .handler = keyboard_layout_handler };
    const httpd_uri_t uri_net_ping       = { .uri = "/net/ping",       .method = HTTP_GET,  .handler = ping_handler };
    /* /nvs/erase registered en-bloc via debug_server_settings_register() below. */
    /* TT #328 Wave 12 — synthesize Wave 3 error.* obs events + their
     * banner/toast UX without needing real fault conditions.  Lets the
     * harness validate the persistent banner + tone-aware toast paths
     * for error.dragon / error.auth / error.ota / error.sd / error.k144
     * end-to-end. */
    extern esp_err_t debug_inject_error_handler(httpd_req_t * req);
    const httpd_uri_t uri_inject_error = {
        .uri = "/debug/inject_error", .method = HTTP_POST, .handler = debug_inject_error_handler};
    extern esp_err_t debug_inject_audio_handler(httpd_req_t * req);
    const httpd_uri_t uri_inject_audio = {
        .uri = "/debug/inject_audio", .method = HTTP_POST, .handler = debug_inject_audio_handler};
    /* Wave 23b (#332): codec endpoint family — registered en-bloc via
     * debug_server_codec_register() below. */

    /* TT #328 Wave 2 — synthetic WS-frame injector for error-surfacing
     * user-story tests (config_update.error, dictation_postprocessing_*,
     * generic `error`-type frames). */
    extern esp_err_t debug_inject_ws_handler(httpd_req_t * req);
    const httpd_uri_t uri_inject_ws = {
        .uri = "/debug/inject_ws", .method = HTTP_POST, .handler = debug_inject_ws_handler};

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_screenshot);
    httpd_register_uri_handler(server, &uri_screenshot_jpg);
    httpd_register_uri_handler(server, &uri_info);
    httpd_register_uri_handler(server, &uri_touch);
    httpd_register_uri_handler(server, &uri_reboot);
    httpd_register_uri_handler(server, &uri_log);
    httpd_register_uri_handler(server, &uri_crashlog);
    httpd_register_uri_handler(server, &uri_coredump);
    httpd_register_uri_handler(server, &uri_heap_trace_start);
    httpd_register_uri_handler(server, &uri_heap_trace_dump);
    httpd_register_uri_handler(server, &uri_sdcard);
    /* Wave 23b (#332): /settings (GET+POST) + /nvs/erase registered en-bloc. */
    debug_server_settings_register(server);
    /* Wave 23b (#332): /mode endpoint family registered en-bloc. */
    debug_server_mode_register(server);
    httpd_register_uri_handler(server, &uri_navigate);
    httpd_register_uri_handler(server, &uri_widget);
    httpd_register_uri_handler(server, &uri_camera);
    /* Wave 23b (#332): OTA endpoint family registered en-bloc. */
    debug_server_ota_register(server);
    httpd_register_uri_handler(server, &uri_chat);
    httpd_register_uri_handler(server, &uri_input_text);
    httpd_register_uri_handler(server, &uri_screen);
    /* Wave 23b (#332): voice endpoint family registered en-bloc. */
    debug_server_voice_register(server);
    /* Wave 23b (#332): K144 endpoint family registered en-bloc. */
    debug_server_m5_register(server);
    debug_server_call_register(server);
    /* Wave 23b (#332): /dictation family registered en-bloc. */
    debug_server_dictation_register(server);
    /* Wave 23b (#332): WiFi diagnostic + recovery family registered en-bloc. */
    debug_server_wifi_register(server);
    httpd_register_uri_handler(server, &uri_selftest);
    httpd_register_uri_handler(server, &uri_heap);
    /* #149 PR β registrations. */
    httpd_register_uri_handler(server, &uri_tasks);
    httpd_register_uri_handler(server, &uri_logs_tail);
    httpd_register_uri_handler(server, &uri_voice_text);
    /* Wave 23b (#332): /voice/cancel + /voice/clear registered via
     * debug_server_voice_register() above; /wifi/status registered via
     * debug_server_wifi_register() above. */
    httpd_register_uri_handler(server, &uri_battery);
    httpd_register_uri_handler(server, &uri_disp_bright);
    httpd_register_uri_handler(server, &uri_audio_get);
    httpd_register_uri_handler(server, &uri_audio_post);
    httpd_register_uri_handler(server, &uri_metrics);
    httpd_register_uri_handler(server, &uri_events);
    httpd_register_uri_handler(server, &uri_heap_history);
    httpd_register_uri_handler(server, &uri_heap_probe);
    httpd_register_uri_handler(server, &uri_chat_msgs);
    httpd_register_uri_handler(server, &uri_chat_audio);
    httpd_register_uri_handler(server, &uri_tool_push);
    httpd_register_uri_handler(server, &uri_chat_partial);
    httpd_register_uri_handler(server, &uri_chat_llm_done);
    httpd_register_uri_handler(server, &uri_kb_layout);
    httpd_register_uri_handler(server, &uri_net_ping);
    /* /nvs/erase registered via debug_server_settings_register() above. */
    httpd_register_uri_handler(server, &uri_inject_error);
    httpd_register_uri_handler(server, &uri_inject_audio);
    /* Wave 23b (#332): codec endpoint family registered en-bloc. */
    debug_server_codec_register(server);
    httpd_register_uri_handler(server, &uri_inject_ws);

    /* Log the URL */
    char ip[20];
    get_wifi_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "Debug server: http://%s:%d/", ip, DEBUG_PORT);

    return ESP_OK;
}

esp_err_t tab5_debug_server_stop(void)
{
    if (s_httpd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = httpd_stop(s_httpd);
    s_httpd = NULL;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Debug server stopped");
    } else {
        ESP_LOGW(TAG, "httpd_stop returned %s", esp_err_to_name(err));
    }
    return err;
}
