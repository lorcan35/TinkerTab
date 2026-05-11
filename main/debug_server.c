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
#include "debug_server_admin.h"
#include "debug_server_call.h"
#include "debug_server_camera.h"
#include "debug_server_chat.h"
#include "debug_server_codec.h"
#include "debug_server_dictation.h"
#include "debug_server_inject.h"
#include "debug_server_input.h"
#include "debug_server_internal.h"
#include "debug_server_m5.h"
#include "debug_server_metrics.h"
#include "debug_server_mode.h"
#include "debug_server_nav.h"
#include "debug_server_obs.h"
#include "debug_server_ota.h"
#include "debug_server_settings.h"
#include "debug_server_solo.h"
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
/* debug_inject_state_t + s_inject_state + s_inject_seq + inject_publish
 * + tab5_debug_touch_override moved to debug_server_input.c
 * (Wave 23b #332). */

/* FB_W/H/BPP + s_jpeg_enc/_mux + ensure_jpeg_encoder all moved to
 * debug_server_camera.c (Wave 23b #332). */

/* /screenshot, /screenshot.jpg, /camera + the shared hardware JPEG
 * encoder + s_screenshot_busy machinery moved to debug_server_camera.c
 * — Wave 23b (#332) eleventh per-family extract.
 * Registered via debug_server_camera_register() below. */

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
    snprintf(display_str, sizeof(display_str), "%dx%d", TAB5_DISPLAY_WIDTH, TAB5_DISPLAY_HEIGHT);
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

/* touch_handler moved to debug_server_input.c (Wave 23b #332). */

/* /reboot moved to debug_server_admin.c (Wave 23b #332). */

/* ======================================================================== */
/*  GET /log                                                                 */
/* ======================================================================== */

/* log_handler moved to debug_server_obs.c */

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

/* crashlog_handler moved to debug_server_obs.c */

/* ── Coredump binary download ─────────────────────────────────────────── */

/* Wave 15 W15-C05: stream the raw coredump partition bytes so we can
 * decode them offline with `espcoredump.py info_corefile -t elf`.
 * Partition reads are chunked into 4 KiB so the httpd task stack stays
 * tiny regardless of the dump size (~3 MB on this board).  Erase is
 * NOT performed here — the operator explicitly calls /crashlog?erase=1
 * or runs the esptool after confirming the dump was captured.  This
 * avoids the pathological loop where a corrupted dump keeps crashing
 * the decoder and gets wiped on every fetch attempt. */
/* coredump_handler moved to debug_server_obs.c */

/* heap-trace machinery (start + dump handlers + s_heap_trace_*
 * + TAB5_HEAP_TRACE_NUM_RECORDS) moved to debug_server_obs.c */

/* _list_dir_to_json + sdcard_handler moved to debug_server_admin.c
 * (Wave 23b #332). */

/* ── OTA endpoint family extracted to debug_server_ota.c (Wave 23b, #332) ─ */

/* ── ADB-style debug APIs ─────────────────────────────────────────────── */

/* ── /settings (GET + POST) family extracted to debug_server_settings.c
 *    (Wave 23b, #332) — handlers + register live there now. ───────────── */

/* ── /mode endpoint family extracted to debug_server_mode.c (Wave 23b, #332) ─ */

/* s_nav_target + s_navigating + tab5_debug_set_nav_target +
 * async_navigate moved to debug_server_nav.c (Wave 23b #332). */

/* widget_handler + async_widget_refresh moved to debug_server_admin.c
 * (Wave 23b #332). */

/* navigate_handler moved to debug_server_nav.c (Wave 23b #332). */

/* #148: /navtouch removed.  It was a thin wrapper that computed a bogus
 * tap_x coordinate and then called the same /navigate path anyway.
 * Callers should POST /navigate?screen=<name> directly. */

/* ── Camera debug endpoint ────────────────────────────────────────────── */

/* camera_handler moved to debug_server_camera.c */

/* chat_handler moved to debug_server_chat.c (Wave 23b #332). */

/* Forward decl — definition is further down the file. */
static esp_err_t send_json_resp(httpd_req_t *req, cJSON *root);

/* input_text_args_t + resolve_input_target + input_text_apply_lvgl
 * + input_text_handler moved to debug_server_input.c
 * (Wave 23b #332). */

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
/* screen_state_handler moved to debug_server_nav.c (Wave 23b #332). */

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

/* heap_handler + the Wave 12 observability comment block moved
 * to debug_server_obs.c (Wave 23b #332). */

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

/* tasks_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* logs_tail_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* voice_text_handler moved to debug_server_chat.c (Wave 23b #332) — /voice/text
 * is just an alias for /chat, lives with the chat family now. */

/* /wifi/status extracted to debug_server_wifi.c (Wave 23b, #332). */

/* battery_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* display_brightness_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* audio_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* metrics_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* events_handler + the long-poll caveat note moved to debug_server_metrics.c (Wave 23b #332). */
/* heap_history_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* chat_messages_handler moved to debug_server_chat.c (Wave 23b #332). */

/* keyboard_layout_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* chat_llm_done_handler moved to debug_server_chat.c (Wave 23b #332). */

/* chat_partial_handler moved to debug_server_chat.c (Wave 23b #332). */

/* url_pct_decode_inplace + tool_log_push_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* chat_audio_clip_handler moved to debug_server_chat.c (Wave 23b #332). */

/* ping_handler moved to debug_server_metrics.c (Wave 23b #332). */

/* ── POST /nvs/erase family extracted to debug_server_settings.c
 *    (Wave 23b, #332) — handler + register live there now. ─────────── */

/* debug_inject_banner_arg_t + banner_async + /debug/inject_audio +
 * /debug/inject_error + /debug/inject_ws moved to
 * debug_server_inject.c (Wave 23b #332). */

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
    (void)debug_server_camera_init_jpeg();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DEBUG_PORT;
    config.stack_size  = 12288;
    /* TT #328 Wave 12: bumped 56 → 72.  The 56 cap was set in #149
     * for the PR-β endpoints; subsequent Waves added /m5 (#317),
     * /events, /chat/messages, /chat/audio_clip, /chat/partial,
     * /chat/llm_done, /tool_log/push, /keyboard/layout, /net/ping,
     * /nvs/erase, /debug/inject_error, etc.  Without the bump, late
     * registrations silently drop with httpd 404. */
    /* TT #370: solo-mode synthetic test endpoints add /solo/sse_test +
     * /solo/llm_test + /solo/rag_test in subsequent commits.  Bumped
     * 72 → 80 to absorb them and leave a 4-slot cushion. */
    config.max_uri_handlers = 80;
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
    /* /screenshot + /screenshot.jpg registered via debug_server_camera_register() below. */
    const httpd_uri_t uri_info = {
        .uri = "/info", .method = HTTP_GET, .handler = info_handler
    };
    /* /touch registered via debug_server_input_register() below. */
    /* /reboot registered via debug_server_admin_register() below. */
    /* /log registered via debug_server_obs_register() below. */

    /* #148: /open merged into /navigate (single source of truth for
     * screen list).  /navtouch dropped — same underlying dispatcher. */
    /* /coredump /heap_trace_start /heap_trace_dump /crashlog
     * registered via debug_server_obs_register() below. */
    /* /sdcard registered via debug_server_admin_register() below. */
    /* #148: /wake removed (feature parked) — /info still reports state. */
    /* Wave 23b (#332): /settings GET+POST + /nvs/erase + /mode all
     * registered en-bloc via the per-family extracted modules
     * (debug_server_settings_register, debug_server_mode_register)
     * below. */
    /* /navigate registered via debug_server_nav_register() below. */
    /* /widget registered via debug_server_admin_register() below. */
    /* /camera registered via debug_server_camera_register() below. */
    /* Wave 23b (#332): OTA endpoint family — registered en-bloc via
     * debug_server_ota_register() below. */
    /* /chat registered via debug_server_chat_register() below. */
    /* /input/text registered via debug_server_input_register() below. */
    /* /screen registered via debug_server_nav_register() below. */
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
    /* /heap registered via debug_server_obs_register() below. */

    /* #149 PR β — new capability endpoints. */
    /* /tasks /logs/tail registered via debug_server_metrics_register() below. */
    /* /voice/text registered via debug_server_chat_register() (alias of /chat). */
    /* Wave 23b (#332): /voice/cancel + /voice/clear moved to debug_server_voice.c. */
    /* /wifi/status registered en-bloc via debug_server_wifi_register() below. */
    /* /battery /display/brightness /audio (GET+POST) /metrics /events /heap/history
     * /heap/probe-csv /tool_log/push registered via debug_server_metrics_register() below. */
    /* /chat/messages /chat/audio_clip /chat/partial /chat/llm_done
     * registered via debug_server_chat_register() below. */
    /* /keyboard/layout /net/ping registered via debug_server_metrics_register() below. */
    /* /nvs/erase registered en-bloc via debug_server_settings_register() below. */
    /* TT #328 Wave 12 — synthesize Wave 3 error.* obs events + their
     * banner/toast UX without needing real fault conditions.  Lets the
     * harness validate the persistent banner + tone-aware toast paths
     * for error.dragon / error.auth / error.ota / error.sd / error.k144
     * end-to-end. */
    /* /debug/inject_error + /debug/inject_audio + /debug/inject_ws
     * registered via debug_server_inject_register() below. */
    /* Wave 23b (#332): codec endpoint family — registered en-bloc via
     * debug_server_codec_register() below. */

    /* TT #328 Wave 2 — synthetic WS-frame injector for error-surfacing
     * user-story tests (config_update.error, dictation_postprocessing_*,
     * generic `error`-type frames). */

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_info);
    debug_server_obs_register(server);
    /* Wave 23b (#332): /settings (GET+POST) + /nvs/erase registered en-bloc. */
    debug_server_settings_register(server);
    /* Wave 23b (#332): /mode endpoint family registered en-bloc. */
    debug_server_mode_register(server);
    debug_server_admin_register(server);
    debug_server_camera_register(server);
    /* Wave 23b (#332): OTA endpoint family registered en-bloc. */
    debug_server_ota_register(server);
    debug_server_chat_register(server);
    debug_server_input_register(server);
    debug_server_nav_register(server);
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
    /* #149 PR β registrations. */
    debug_server_metrics_register(server);
    /* Wave 23b (#332): /voice/cancel + /voice/clear registered via
     * debug_server_voice_register() above; /wifi/status registered via
     * debug_server_wifi_register() above. */
    /* /nvs/erase registered via debug_server_settings_register() above. */
    debug_server_inject_register(server);
    /* Wave 23b (#332): codec endpoint family registered en-bloc. */
    debug_server_codec_register(server);
    /* TT #370: solo-mode synthetic test endpoints (/solo/sse_test, ...). */
    debug_server_solo_register(server);

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
