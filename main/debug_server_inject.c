/*
 * debug_server_inject.c — synthetic-injection debug HTTP family.
 *
 * Wave 23b follow-up (#332): seventeenth per-family extract.  Owns:
 *
 *   POST /debug/inject_audio  — pump caller-supplied PCM through the
 *                               same WS binary path the mic task uses.
 *                               Wire: raw POST body = 16 kHz mono int16
 *                               LE PCM, 1..64 KB.  Bypasses the mic
 *                               capture loop so the harness can validate
 *                               STT round-trip without canned audio.
 *   POST /debug/inject_error  — synthesise a Wave 3 error.* obs event +
 *                               persistent banner.  Body or query:
 *                                 class=dragon|auth|ota|sd|k144
 *                                 detail=<short string>
 *                                 banner=true|false (default true)
 *                                 banner_text=<override>
 *   POST /debug/inject_ws     — route a JSON string through voice.c's
 *                               WS text dispatcher as if Dragon had sent
 *                               it.  Lets the harness exercise toast/
 *                               banner UX paths without making Dragon
 *                               genuinely fail.
 *
 * Same convention as the prior 16 per-family extracts:
 *   check_auth(req)            → tab5_debug_check_auth(req)
 *   send_json_resp(req, root)  → tab5_debug_send_json_resp(req, root)
 */
#include "debug_server_inject.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug_obs.h"
#include "debug_server_internal.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h" /* lv_async_cb_t / lv_result_t signature for tab5_lv_async_call */

/* TAG omitted — handlers below use fixed log strings rather than ESP_LOG. */

#define check_auth(req) tab5_debug_check_auth(req)
#define send_json_resp(req, root) tab5_debug_send_json_resp(req, root)

/* ── Banner async trampoline (LVGL thread) ──────────────────────────── */
/* TT #328 Wave 12 — async banner trampoline for /debug/inject_error.
 * Fires on the LVGL thread; takes ownership of the heap-allocated
 * argument struct. */
typedef struct {
   char txt[96];
} debug_inject_banner_arg_t;

static void debug_inject_banner_async(void *arg) {
   debug_inject_banner_arg_t *p = (debug_inject_banner_arg_t *)arg;
   if (!p) return;
   extern void ui_home_show_error_banner(const char *, void (*)(void));
   ui_home_show_error_banner(p->txt, NULL /* non-dismissable */);
   free(p);
}

/* ── POST /debug/inject_audio ────────────────────────────────────────── */

static esp_err_t debug_inject_audio_handler(httpd_req_t *req) {
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

/* ── POST /debug/inject_error ────────────────────────────────────────── */

static esp_err_t debug_inject_error_handler(httpd_req_t *req) {
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

/* ── POST /debug/inject_ws ──────────────────────────────────────────── */

static esp_err_t debug_inject_ws_handler(httpd_req_t *req) {
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

void debug_server_inject_register(httpd_handle_t server) {
   if (!server) return;

   static const httpd_uri_t uri_inject_audio = {
       .uri = "/debug/inject_audio", .method = HTTP_POST, .handler = debug_inject_audio_handler};
   static const httpd_uri_t uri_inject_error = {
       .uri = "/debug/inject_error", .method = HTTP_POST, .handler = debug_inject_error_handler};
   static const httpd_uri_t uri_inject_ws = {
       .uri = "/debug/inject_ws", .method = HTTP_POST, .handler = debug_inject_ws_handler};

   httpd_register_uri_handler(server, &uri_inject_audio);
   httpd_register_uri_handler(server, &uri_inject_error);
   httpd_register_uri_handler(server, &uri_inject_ws);
}
