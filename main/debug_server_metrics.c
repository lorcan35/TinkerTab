/*
 * debug_server_metrics.c — diagnostics / metrics / device-control
 * debug HTTP family.
 *
 * Wave 23b follow-up (#332): eighteenth per-family extract.  The 11
 * small read-only / control endpoints that didn't fit any other family
 * bucket move here, plus the /heap/probe-csv pool-probe relay:
 *
 *   GET  /tasks            — FreeRTOS task snapshot (count + per-task
 *                            dump when configUSE_TRACE_FACILITY=y)
 *   GET  /logs/tail?n=     — last N lines from the obs log ring
 *   GET  /battery          — voltage / current / power / percent / charging
 *   POST /display/brightness?pct=
 *                          — set + persist display brightness
 *   GET  /audio            — current volume + mic_mute
 *   POST /audio?action=    — volume|mute control
 *   GET  /metrics          — Prometheus text format (uptime, heaps, wifi,
 *                            voice, battery, NVS write count)
 *   GET  /events?since=ms  — obs events ring as JSON
 *   GET  /heap/history?n=  — heap sampling history JSON
 *   GET  /heap/probe-csv   — pool-probe CSV relay (handler in pool_probe.c)
 *   GET  /keyboard/layout  — live LVGL keyboard key positions
 *   POST /tool_log/push?name=&detail=&ms=
 *                          — synthesise a tool_log event for harness UX tests
 *   GET  /net/ping?host=&port= — non-blocking TCP connect with 2s timeout
 *
 * Same convention as the prior 17 per-family extracts:
 *   check_auth(req)            → tab5_debug_check_auth(req)
 *   send_json_resp(req, root)  → tab5_debug_send_json_resp(req, root)
 *
 * Local url_pct_decode_inplace — sister copy in debug_server_chat.c.
 * Both are file-static.  This is the last consumer in debug_server.c
 * after this extract; debug_server.c will not need its own copy.
 */
#include "debug_server_metrics.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "audio.h"
#include "battery.h"
#include "cJSON.h"
#include "debug_obs.h"
#include "debug_server_internal.h"
#include "display.h" /* tab5_display_set_brightness */
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h" /* TT #511 diagnose-first — GET /imu live accel/gyro read */
#include "lvgl.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "settings.h"
#include "tool_log.h"
#include "ui_core.h"     /* ui_core_get_fps */
#include "ui_keyboard.h" /* ui_keyboard_is_visible / dump_layout */
#include "ui_orb.h"      /* TT #511 step 8 — POST /orb/presence dim test endpoint */
#include "voice.h"
#include "wifi.h"

#define check_auth(req) tab5_debug_check_auth(req)
#define send_json_resp(req, root) tab5_debug_send_json_resp(req, root)

/* External handler from pool_probe.c — registered here as part of
 * the metrics family so all heap-observability endpoints land in one
 * register block. */
extern esp_err_t tab5_pool_probe_http_handler(httpd_req_t *req);

/* Local URL-decode helper — used by tool_log_push_handler.  Sister
 * copy lives in debug_server_chat.c.  The duplication is cheaper than
 * a cross-TU dependency on a 5-line primitive; if a third consumer
 * appears, lift to debug_server_internal.h. */
static void url_pct_decode_inplace(char *s) {
   char *r = s, *w = s;
   while (*r) {
      if (*r == '%' && r[1] && r[2]) {
         char hi = r[1], lo = r[2];
         int hv = (hi >= '0' && hi <= '9')   ? hi - '0'
                  : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                  : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                             : -1;
         int lv = (lo >= '0' && lo <= '9')   ? lo - '0'
                  : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                  : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                             : -1;
         if (hv >= 0 && lv >= 0) {
            *w++ = (char)((hv << 4) | lv);
            r += 3;
            continue;
         }
      }
      if (*r == '+') {
         *w++ = ' ';
         r++;
         continue;
      }
      *w++ = *r++;
   }
   *w = 0;
}

/* ── GET /tasks — FreeRTOS task snapshot ────────────────────────────── */
/* Full per-task dump requires configUSE_TRACE_FACILITY + the
 * run-time-stats infra to be enabled in sdkconfig.  Most ESP-IDF
 * projects leave this off to save ~96 B per task; degrade gracefully. */
static esp_err_t tasks_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   cJSON *root = cJSON_CreateObject();
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
      const char *state_names[] = {"running", "ready", "blocked", "suspended", "deleted", "invalid"};
      for (UBaseType_t i = 0; i < got; i++) {
         const TaskStatus_t *t = &status[i];
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "name", t->pcTaskName ? t->pcTaskName : "");
         cJSON_AddNumberToObject(o, "prio", t->uxCurrentPriority);
         cJSON_AddStringToObject(o, "state", t->eCurrentState <= eInvalid ? state_names[t->eCurrentState] : "?");
         cJSON_AddNumberToObject(o, "stack_free", (double)(t->usStackHighWaterMark * sizeof(StackType_t)));
         cJSON_AddNumberToObject(o, "runtime", (double)t->ulRunTimeCounter);
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

/* ── GET /logs/tail?n=100 ───────────────────────────────────────────── */

static esp_err_t logs_tail_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   int n = 100;
   char q[64] = {0}, v[16] = {0};
   if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
       httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
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

/* ── GET /battery ────────────────────────────────────────────────────── */

static esp_err_t battery_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   tab5_battery_info_t bat = {0};
   esp_err_t r = tab5_battery_read(&bat);
   cJSON *root = cJSON_CreateObject();
   if (r == ESP_OK) {
      cJSON_AddNumberToObject(root, "voltage", (double)bat.voltage);
      cJSON_AddNumberToObject(root, "current", (double)bat.current);
      cJSON_AddNumberToObject(root, "power", (double)bat.power);
      cJSON_AddNumberToObject(root, "percent", (double)bat.percent);
      cJSON_AddBoolToObject(root, "charging", bat.charging);
   } else {
      cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
   }
   return send_json_resp(req, root);
}

/* ── GET /imu ─────────────────────────────────────────────────────────
 *
 * Live BMI270 read.  Diagnose-first companion for the orb-redesign tilt
 * specular (TT #511 attempt didn't visibly move the highlight; we need
 * to verify the IMU is actually producing non-zero values before we
 * wire it to LVGL again).  Returns:
 *
 *   { "ok": true,
 *     "accel": { "x": -0.02, "y": -0.01, "z": 0.99 },
 *     "gyro":  { "x": 0.4,   "y": -0.1,  "z": 0.0  },
 *     "age_ms": 12 }
 *
 *   { "ok": false, "error": "ESP_ERR_NOT_FOUND" }   on read failure
 *
 * Bearer-auth, same as siblings.  Synchronous I2C; budget ~2 ms.
 */
static esp_err_t imu_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   int64_t t0 = esp_timer_get_time();
   tab5_imu_data_t d = {0};
   esp_err_t r = tab5_imu_read(&d);
   int64_t age_us = esp_timer_get_time() - t0;
   cJSON *root = cJSON_CreateObject();
   if (r == ESP_OK) {
      cJSON_AddBoolToObject(root, "ok", true);
      cJSON *acc = cJSON_AddObjectToObject(root, "accel");
      cJSON_AddNumberToObject(acc, "x", (double)d.accel.x);
      cJSON_AddNumberToObject(acc, "y", (double)d.accel.y);
      cJSON_AddNumberToObject(acc, "z", (double)d.accel.z);
      cJSON *gy = cJSON_AddObjectToObject(root, "gyro");
      cJSON_AddNumberToObject(gy, "x", (double)d.gyro.x);
      cJSON_AddNumberToObject(gy, "y", (double)d.gyro.y);
      cJSON_AddNumberToObject(gy, "z", (double)d.gyro.z);
      cJSON_AddNumberToObject(root, "age_ms", (double)(age_us / 1000));
   } else {
      cJSON_AddBoolToObject(root, "ok", false);
      cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
   }
   return send_json_resp(req, root);
}

/* ── POST /orb/presence?p=0|1 ────────────────────────────────────────── */

/* TT #511 wave-1 step 8: debug-server hook for the orb's presence-wake
 * dim machinery.  The auto-trigger from camera face-detection is wave-2
 * follow-up; this endpoint lets us test the dim VISUAL behavior in
 * isolation.  p=1 → orb at full brightness; p=0 → orb dims to ~33%. */
static esp_err_t orb_presence_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   char q[32] = {0}, v[8] = {0};
   httpd_req_get_url_query_str(req, q, sizeof(q));
   httpd_query_key_value(q, "p", v, sizeof(v));
   int p = atoi(v);
   bool near = (p != 0);
   ui_orb_set_presence(near);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "near", near);
   return send_json_resp(req, root);
}

/* ── POST /display/brightness?pct=0..100 ─────────────────────────────── */

static esp_err_t display_brightness_handler(httpd_req_t *req) {
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

/* ── GET/POST /audio ─────────────────────────────────────────────────── */

static esp_err_t audio_handler(httpd_req_t *req) {
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
   cJSON_AddNumberToObject(root, "volume", tab5_settings_get_volume());
   cJSON_AddNumberToObject(root, "mic_mute", tab5_settings_get_mic_mute());
   return send_json_resp(req, root);
}

/* ── GET /metrics — Prometheus text format ─────────────────────────── */

static esp_err_t metrics_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
   size_t int_lrg = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
   size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
   size_t ps_lrg = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
   lv_mem_monitor_t lv_m;
   lv_mem_monitor(&lv_m);
   tab5_battery_info_t bat = {0};
   tab5_battery_read(&bat);

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
                    (unsigned long long)(esp_timer_get_time() / 1000), (unsigned)int_free, (unsigned)ps_free,
                    (unsigned)lv_m.free_size, (unsigned)int_lrg, (unsigned)ps_lrg, tab5_wifi_connected() ? 1 : 0,
                    voice_is_connected() ? 1 : 0, tab5_settings_get_voice_mode(), (unsigned)voice_get_state(),
                    (unsigned long)ui_core_get_fps(), bat.percent, bat.voltage, bat.current,
                    (unsigned long)tab5_settings_get_nvs_write_count());
   (void)n;
   httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_sendstr(req, out);
   return ESP_OK;
}

/* ── GET /events?since=<ms> ─────────────────────────────────────────── */

static esp_err_t events_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   uint64_t since = 0;
   char q[48] = {0}, v[24] = {0};
   if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
       httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
      since = (uint64_t)strtoull(v, NULL, 10);
   }
   cJSON *root = cJSON_CreateObject();
   cJSON *arr = tab5_debug_obs_events_json(since);
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

/* ── GET /heap/history?n=60 ─────────────────────────────────────────── */

static esp_err_t heap_history_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   int n = 60;
   char q[32] = {0}, v[8] = {0};
   if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
       httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
      int x = atoi(v);
      if (x > 0) n = x;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON_AddItemToObject(root, "samples", tab5_debug_obs_heap_json(n));
   return send_json_resp(req, root);
}

/* ── GET /keyboard/layout ───────────────────────────────────────────── */
/* Issue #161: returns the live keyboard key positions so tests can
 * derive tap coords instead of hardcoding them in a memory file that
 * rots on layout drift.  Empty array when keyboard isn't built yet. */

static esp_err_t keyboard_layout_handler(httpd_req_t *req) {
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
      cJSON_AddNumberToObject(k, "x", buf[i].x);
      cJSON_AddNumberToObject(k, "y", buf[i].y);
      cJSON_AddNumberToObject(k, "w", buf[i].w);
      cJSON_AddNumberToObject(k, "h", buf[i].h);
      /* center-of-key tap targets — most useful for scripts */
      cJSON_AddNumberToObject(k, "cx", buf[i].x + buf[i].w / 2);
      cJSON_AddNumberToObject(k, "cy", buf[i].y + buf[i].h / 2);
      const char *tn = (buf[i].type < (sizeof(kTypeName) / sizeof(*kTypeName))) ? kTypeName[buf[i].type] : "unknown";
      cJSON_AddStringToObject(k, "type", tn);
      cJSON_AddItemToArray(keys, k);
   }
   return send_json_resp(req, root);
}

/* ── POST /tool_log/push?name=&detail=&ms= ─────────────────────────── */
/* U7+U8 (#206) verification helper: forge a tool_log event so the
 * agents/focus surfaces can be exercised without a live Dragon LLM
 * tool_call producer.  When `ms` is provided, push a "done" entry;
 * otherwise push a "running" entry.  Also accepts mode=both to push
 * a paired call+result. */

static esp_err_t tool_log_push_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   char q[256] = {0}, name[32] = {0}, detail[96] = {0}, ms_s[12] = {0}, mode[12] = {0};
   httpd_req_get_url_query_str(req, q, sizeof(q));
   httpd_query_key_value(q, "name", name, sizeof(name));
   httpd_query_key_value(q, "detail", detail, sizeof(detail));
   httpd_query_key_value(q, "ms", ms_s, sizeof(ms_s));
   httpd_query_key_value(q, "mode", mode, sizeof(mode));
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

/* ── GET /net/ping?host=<>&port=<> ──────────────────────────────────── */
/* Re-implements the non-blocking probe locally so we don't leak voice.c
 * internals.  Matches the fix from #146. */

static esp_err_t ping_handler(httpd_req_t *req) {
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

   struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
   struct addrinfo *res = NULL;
   char ps[8];
   snprintf(ps, sizeof(ps), "%d", port);
   int gai = getaddrinfo(host, ps, &hints, &res);
   if (gai != 0 || !res) {
      cJSON_AddStringToObject(root, "error", "dns failed");
      if (res) freeaddrinfo(res);
      return send_json_resp(req, root);
   }
   int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (s < 0) {
      freeaddrinfo(res);
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
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(s, &wfds);
      struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
      int n = select(s + 1, NULL, &wfds, NULL, &tv);
      if (n > 0 && FD_ISSET(s, &wfds)) {
         int soerr = 0;
         socklen_t le = sizeof(soerr);
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

void debug_server_metrics_register(httpd_handle_t server) {
   if (!server) return;

   static const httpd_uri_t uri_tasks = {.uri = "/tasks", .method = HTTP_GET, .handler = tasks_handler};
   static const httpd_uri_t uri_logs_tail = {.uri = "/logs/tail", .method = HTTP_GET, .handler = logs_tail_handler};
   static const httpd_uri_t uri_battery = {.uri = "/battery", .method = HTTP_GET, .handler = battery_handler};
   static const httpd_uri_t uri_imu = {.uri = "/imu", .method = HTTP_GET, .handler = imu_handler};
   static const httpd_uri_t uri_orb_presence = {
       .uri = "/orb/presence", .method = HTTP_POST, .handler = orb_presence_handler};
   static const httpd_uri_t uri_disp_bright = {
       .uri = "/display/brightness", .method = HTTP_POST, .handler = display_brightness_handler};
   static const httpd_uri_t uri_audio_get = {.uri = "/audio", .method = HTTP_GET, .handler = audio_handler};
   static const httpd_uri_t uri_audio_post = {.uri = "/audio", .method = HTTP_POST, .handler = audio_handler};
   static const httpd_uri_t uri_metrics = {.uri = "/metrics", .method = HTTP_GET, .handler = metrics_handler};
   static const httpd_uri_t uri_events = {.uri = "/events", .method = HTTP_GET, .handler = events_handler};
   static const httpd_uri_t uri_heap_history = {
       .uri = "/heap/history", .method = HTTP_GET, .handler = heap_history_handler};
   static const httpd_uri_t uri_heap_probe = {
       .uri = "/heap/probe-csv", .method = HTTP_GET, .handler = tab5_pool_probe_http_handler};
   static const httpd_uri_t uri_kb_layout = {
       .uri = "/keyboard/layout", .method = HTTP_GET, .handler = keyboard_layout_handler};
   static const httpd_uri_t uri_tool_push = {
       .uri = "/tool_log/push", .method = HTTP_POST, .handler = tool_log_push_handler};
   static const httpd_uri_t uri_net_ping = {.uri = "/net/ping", .method = HTTP_GET, .handler = ping_handler};

   httpd_register_uri_handler(server, &uri_tasks);
   httpd_register_uri_handler(server, &uri_logs_tail);
   httpd_register_uri_handler(server, &uri_battery);
   httpd_register_uri_handler(server, &uri_imu);
   httpd_register_uri_handler(server, &uri_orb_presence);
   httpd_register_uri_handler(server, &uri_disp_bright);
   httpd_register_uri_handler(server, &uri_audio_get);
   httpd_register_uri_handler(server, &uri_audio_post);
   httpd_register_uri_handler(server, &uri_metrics);
   httpd_register_uri_handler(server, &uri_events);
   httpd_register_uri_handler(server, &uri_heap_history);
   httpd_register_uri_handler(server, &uri_heap_probe);
   httpd_register_uri_handler(server, &uri_kb_layout);
   httpd_register_uri_handler(server, &uri_tool_push);
   httpd_register_uri_handler(server, &uri_net_ping);
}
