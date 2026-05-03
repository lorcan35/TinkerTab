/*
 * debug_server_call.c — video-call + uplink-streaming debug HTTP family.
 *
 * Wave 23b follow-up (#332): tenth per-family extract.  All 12 video/
 * call control handlers moved verbatim from debug_server.c with
 * `check_auth(req)` → `tab5_debug_check_auth(req)` and
 * `send_json_resp(req, root)` → `tab5_debug_send_json_resp(req, root)`.
 * Identical behaviour.
 *
 * Endpoints (all bearer-auth):
 *   POST /video/start        — uplink JPEG streaming start
 *   POST /video/stop         — uplink JPEG streaming stop
 *   GET  /video              — uplink + downlink stats
 *   POST /video/show         — show downlink video pane
 *   POST /video/hide         — hide downlink video pane
 *   POST /video/call/start   — atomic call begin (mode flip + camera + pane)
 *   POST /video/call/end     — atomic call end
 *   GET  /call/status        — consolidated call state across surfaces
 *   POST /call/mute          — in-call mute
 *   POST /call/unmute        — in-call unmute
 *   POST /call/minimize      — minimise pane (call continues)
 *   POST /call/restore       — restore minimised pane
 */
#include "debug_server_call.h"

#include "cJSON.h"
#include "debug_server_internal.h" /* tab5_debug_check_auth / send_json_resp */
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_core.h"        /* tab5_lv_async_call */
#include "ui_video_pane.h"  /* ui_video_pane_show/hide/minimize/restore + state queries */
#include "voice.h"          /* voice_get_mode + voice_call_audio_is_muted + voice_mode_t enum */
#include "voice_video.h"    /* voice_video_* streaming + call control */

#include <stddef.h>
#include <stdlib.h>

/* Convenience: the tab5_debug_* prefixed helpers are the public-ish
 * surface in debug_server_internal.h.  Map them locally to the names
 * the moved code expects so the verbatim move stays clean. */
#define check_auth(req)            tab5_debug_check_auth(req)
#define send_json_resp(req, root)  tab5_debug_send_json_resp(req, root)

/* The pane is an LVGL widget — must be opened on the LVGL thread. */
static void show_pane_async(void *arg) { (void)arg; ui_video_pane_show(); }
static void hide_pane_async(void *arg) { (void)arg; ui_video_pane_hide(); }

static esp_err_t video_show_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    tab5_lv_async_call(show_pane_async, NULL);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json_resp(req, root);
}

static esp_err_t video_hide_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    tab5_lv_async_call(hide_pane_async, NULL);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json_resp(req, root);
}

/* #274: consolidated call status — one-shot view of every surface
 * involved in a video call so the user / dashboards / E2E tests can
 * see what the device is actually doing without grepping /info,
 * /video, /voice, /heap individually. */
static esp_err_t call_status_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();

    /* Top-level call state */
    cJSON_AddBoolToObject(root, "in_call", voice_video_is_in_call());
    cJSON_AddBoolToObject(root, "pane_visible", ui_video_pane_is_visible());
    cJSON_AddBoolToObject(root, "pane_minimized", ui_video_pane_is_minimized());

    /* Video subsection (uplink + downlink stats) */
    voice_video_stats_t v;
    voice_video_get_stats(&v);
    cJSON *vid = cJSON_AddObjectToObject(root, "video");
    cJSON_AddBoolToObject(vid,   "uplink_active", v.active);
    cJSON_AddNumberToObject(vid, "uplink_fps",    v.fps);
    cJSON_AddNumberToObject(vid, "uplink_frames", (double)v.frames_sent);
    cJSON_AddNumberToObject(vid, "uplink_dropped",(double)v.frames_dropped);
    cJSON_AddNumberToObject(vid, "uplink_bytes",  (double)v.bytes_sent);
    cJSON_AddNumberToObject(vid, "uplink_last_jpeg",(double)v.last_jpeg_bytes);
    cJSON_AddNumberToObject(vid, "downlink_frames",(double)v.frames_recv);
    cJSON_AddNumberToObject(vid, "downlink_dropped",(double)v.frames_recv_dropped);
    cJSON_AddNumberToObject(vid, "downlink_bytes",(double)v.bytes_recv);
    cJSON_AddNumberToObject(vid, "downlink_last_jpeg",(double)v.last_recv_jpeg_bytes);

    /* Audio subsection — mode is the proxy for "call audio active". */
    cJSON *aud = cJSON_AddObjectToObject(root, "audio");
    voice_mode_t vmode = voice_get_mode();
    cJSON_AddNumberToObject(aud, "voice_mode_enum", (double)vmode);
    const char *mode_names[] = {"ASK","DICTATE","CALL"};
    cJSON_AddStringToObject(aud, "voice_mode_name",
        (vmode < (sizeof(mode_names)/sizeof(mode_names[0]))) ? mode_names[vmode] : "?");
    cJSON_AddBoolToObject(aud, "call_audio_active", vmode == VOICE_MODE_CALL);
    cJSON_AddBoolToObject(aud, "call_audio_muted",  voice_call_audio_is_muted());

    /* Heap snapshot — the heap_wd reset after start_call exhausted
     * SRAM, so always show internal-largest here for context. */
    cJSON *hp = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(hp, "internal_largest_kb",
        (double)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    cJSON_AddNumberToObject(hp, "internal_free_kb",
        (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    cJSON_AddNumberToObject(hp, "psram_largest_kb",
        (double)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    cJSON_AddNumberToObject(hp, "dma_largest_kb",
        (double)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024));

    cJSON_AddNumberToObject(root, "tasks", (double)uxTaskGetNumberOfTasks());
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));

    return send_json_resp(req, root);
}

/* #270 Phase 3D: one-shot call control. */
static esp_err_t video_call_start_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    int fps = VOICE_VIDEO_DEFAULT_FPS;
    char q[64] = {0}, v[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "fps", v, sizeof(v)) == ESP_OK) {
        int n = atoi(v);
        if (n > 0 && n <= VOICE_VIDEO_MAX_FPS) fps = n;
    }
    esp_err_t err = voice_video_start_call(fps);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(root, "fps", fps);
    if (err != ESP_OK) cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    return send_json_resp(req, root);
}

static esp_err_t video_call_end_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    voice_video_end_call();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json_resp(req, root);
}

/* #280: in-call mute toggle.  POST /call/mute or POST /call/unmute. */
static esp_err_t call_mute_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    bool muted = voice_call_audio_set_muted(true);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "muted", muted);
    return send_json_resp(req, root);
}

static esp_err_t call_unmute_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    bool muted = voice_call_audio_set_muted(false);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "muted", muted);
    return send_json_resp(req, root);
}

/* #282 minimize / restore: the call pane gets hidden but the call
 * continues + a small "On call" chip on lv_layer_top lets the user
 * re-show.  Both endpoints hop to the LVGL thread. */
static void minimize_pane_async(void *arg) { (void)arg; ui_video_pane_minimize(); }
static void restore_pane_async(void *arg)  { (void)arg; ui_video_pane_restore(); }

static esp_err_t call_minimize_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    tab5_lv_async_call(minimize_pane_async, NULL);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json_resp(req, root);
}

static esp_err_t call_restore_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    tab5_lv_async_call(restore_pane_async, NULL);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json_resp(req, root);
}

static esp_err_t video_start_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    int fps = VOICE_VIDEO_DEFAULT_FPS;
    char q[64] = {0}, v[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "fps", v, sizeof(v)) == ESP_OK) {
        int n = atoi(v);
        if (n > 0 && n <= VOICE_VIDEO_MAX_FPS) fps = n;
    }
    esp_err_t err = voice_video_start_streaming(fps);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(root, "fps", fps);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    return send_json_resp(req, root);
}

static esp_err_t video_stop_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    voice_video_stop_streaming();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json_resp(req, root);
}

static esp_err_t video_state_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    voice_video_stats_t s;
    voice_video_get_stats(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active",          s.active);
    cJSON_AddNumberToObject(root, "fps",           s.fps);
    cJSON_AddNumberToObject(root, "frames_sent",   (double)s.frames_sent);
    cJSON_AddNumberToObject(root, "frames_dropped",(double)s.frames_dropped);
    cJSON_AddNumberToObject(root, "bytes_sent",    (double)s.bytes_sent);
    cJSON_AddNumberToObject(root, "last_jpeg_bytes",(double)s.last_jpeg_bytes);
    /* #268 downlink stats. */
    cJSON_AddBoolToObject(root, "pane_visible",      ui_video_pane_is_visible());
    cJSON_AddNumberToObject(root, "frames_recv",     (double)s.frames_recv);
    cJSON_AddNumberToObject(root, "frames_recv_dropped", (double)s.frames_recv_dropped);
    cJSON_AddNumberToObject(root, "bytes_recv",      (double)s.bytes_recv);
    cJSON_AddNumberToObject(root, "last_recv_jpeg_bytes",(double)s.last_recv_jpeg_bytes);
    return send_json_resp(req, root);
}

void debug_server_call_register(httpd_handle_t server)
{
    if (!server) return;

    static const httpd_uri_t uri_video_start = {
        .uri = "/video/start", .method = HTTP_POST, .handler = video_start_handler
    };
    static const httpd_uri_t uri_video_stop = {
        .uri = "/video/stop", .method = HTTP_POST, .handler = video_stop_handler
    };
    static const httpd_uri_t uri_video_state = {
        .uri = "/video", .method = HTTP_GET, .handler = video_state_handler
    };
    static const httpd_uri_t uri_video_show = {
        .uri = "/video/show", .method = HTTP_POST, .handler = video_show_handler
    };
    static const httpd_uri_t uri_video_hide = {
        .uri = "/video/hide", .method = HTTP_POST, .handler = video_hide_handler
    };
    static const httpd_uri_t uri_video_call_start = {
        .uri = "/video/call/start", .method = HTTP_POST, .handler = video_call_start_handler
    };
    static const httpd_uri_t uri_video_call_end = {
        .uri = "/video/call/end", .method = HTTP_POST, .handler = video_call_end_handler
    };
    static const httpd_uri_t uri_call_status = {
        .uri = "/call/status", .method = HTTP_GET, .handler = call_status_handler
    };
    static const httpd_uri_t uri_call_mute = {
        .uri = "/call/mute", .method = HTTP_POST, .handler = call_mute_handler
    };
    static const httpd_uri_t uri_call_unmute = {
        .uri = "/call/unmute", .method = HTTP_POST, .handler = call_unmute_handler
    };
    static const httpd_uri_t uri_call_minimize = {
        .uri = "/call/minimize", .method = HTTP_POST, .handler = call_minimize_handler
    };
    static const httpd_uri_t uri_call_restore = {
        .uri = "/call/restore", .method = HTTP_POST, .handler = call_restore_handler
    };

    httpd_register_uri_handler(server, &uri_video_start);
    httpd_register_uri_handler(server, &uri_video_stop);
    httpd_register_uri_handler(server, &uri_video_state);
    httpd_register_uri_handler(server, &uri_video_show);
    httpd_register_uri_handler(server, &uri_video_hide);
    httpd_register_uri_handler(server, &uri_video_call_start);
    httpd_register_uri_handler(server, &uri_video_call_end);
    httpd_register_uri_handler(server, &uri_call_status);
    httpd_register_uri_handler(server, &uri_call_mute);
    httpd_register_uri_handler(server, &uri_call_unmute);
    httpd_register_uri_handler(server, &uri_call_minimize);
    httpd_register_uri_handler(server, &uri_call_restore);
}
