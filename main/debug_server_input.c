/*
 * debug_server_input.c — touch + text-input debug HTTP family.
 *
 * Wave 23b follow-up (#332): thirteenth per-family extract.  Owns:
 *
 *   POST /touch       — synthetic touch injection (tap / press / release /
 *                       long_press / swipe).  Drives the LVGL touch read
 *                       callback via the seqlock-published injection state.
 *   POST /input/text  — type into a named LVGL textarea (e.g. the chat
 *                       input bar) with optional submit-on-done.
 *
 * The seqlock-protected injection state (s_inject_state, s_inject_seq,
 * inject_publish, tab5_debug_touch_override) moves with this family —
 * tab5_debug_touch_override is the public reader the LVGL touch driver
 * polls inside its read callback (Story #294 + audit P0 #3 traced flaky
 * story_onboard step 9 to a write-tear race here, fixed via seqlock).
 *
 * Same convention as the prior 12 per-family extracts:
 *   check_auth(req)            → tab5_debug_check_auth(req)
 *   send_json_resp(req, root)  → tab5_debug_send_json_resp(req, root)
 */
#include "debug_server_input.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug_server_internal.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui_core.h" /* tab5_lv_async_call */

static const char *TAG = "debug_input";

#define check_auth(req) tab5_debug_check_auth(req)
#define send_json_resp(req, root) tab5_debug_send_json_resp(req, root)

/* ── Touch-injection seqlock (TT #328 Wave 2 / #294 audit P0 #3) ─────── */
/* TT #294 audit P0 #3: the four fields used to be set one-by-one.  The
 * LVGL touch read callback could observe a torn snapshot — e.g. mid-
 * swipe, the reader saw the new x,y while pressed=false from the prior
 * release, mis-firing as a tap at the swipe midpoint.
 *
 * Fix: seqlock pattern.  Writer increments seq twice around the four-
 * field publish (odd = mid-write, even = published).  Reader spins
 * until seq is even AND unchanged across the field read — guarantees
 * a self-consistent snapshot without taking a mutex on the LVGL hot
 * path. */
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

bool tab5_debug_touch_override(int32_t *x, int32_t *y, bool *pressed) {
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

/* ── POST /touch ─────────────────────────────────────────────────────── */

static esp_err_t touch_handler(httpd_req_t *req) {
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
      if (!cJSON_IsNumber(jx1) || !cJSON_IsNumber(jy1) || !cJSON_IsNumber(jx2) || !cJSON_IsNumber(jy2)) {
         cJSON_Delete(root);
         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "swipe needs x1,y1,x2,y2");
         return ESP_FAIL;
      }
      int x1 = jx1->valueint, y1 = jy1->valueint;
      int x2 = jx2->valueint, y2 = jy2->valueint;
      int dur = (jdur && cJSON_IsNumber(jdur)) ? jdur->valueint : 250;
      if (dur < 50) dur = 50;
      if (dur > 3000) dur = 3000;

      ESP_LOGI(TAG, "Touch inject: swipe (%d,%d)->(%d,%d) %dms", x1, y1, x2, y2, dur);

      /* 20 ms step cadence — enough for LVGL to register the gesture
         direction without flooding the indev read callback. */
      const int step_ms = 20;
      int steps = dur / step_ms;
      if (steps < 5) steps = 5;

      inject_publish(x1, y1, /*active=*/true, /*pressed=*/true);
      vTaskDelay(pdMS_TO_TICKS(40)); /* settle — let LVGL see the press */
      for (int i = 1; i <= steps; i++) {
         int32_t sx = x1 + (x2 - x1) * i / steps;
         int32_t sy = y1 + (y2 - y1) * i / steps;
         inject_publish(sx, sy, /*active=*/true, /*pressed=*/true);
         vTaskDelay(pdMS_TO_TICKS(step_ms));
      }
      inject_publish(s_inject_state.x, s_inject_state.y, /*active=*/true, /*pressed=*/false);
      vTaskDelay(pdMS_TO_TICKS(100)); /* release + LVGL gesture dispatch */
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
      if (dur < 500) dur = 500;
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

/* ── POST /input/text ────────────────────────────────────────────────── */
/* The new contract: the caller MUST name a target widget that the
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
   char target[24]; /* widget name; "" → defaults to "chat" */
   bool submit;
   /* Reply slot — written by the LVGL-thread executor. */
   bool ok;
   int accepted;
   bool submitted;
   char resolved_target[24];
   char err[40];
   SemaphoreHandle_t done;
} input_text_args_t;

/* Resolve a target name to its LVGL textarea, or NULL.  Each target
 * is responsible for its own visibility/state checks (e.g. "chat"
 * returns NULL when the chat overlay isn't active). */
static lv_obj_t *resolve_input_target(const char *name) {
   extern lv_obj_t *ui_chat_get_input_textarea(void);
   if (!name || !name[0] || strcmp(name, "chat") == 0) {
      return ui_chat_get_input_textarea();
   }
   return NULL; /* unknown_target */
}

static void input_text_apply_lvgl(void *arg) {
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
      snprintf(a->err, sizeof(a->err), strcmp(name, "chat") == 0 ? "chat_not_active" : "target_unavailable");
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

static esp_err_t input_text_handler(httpd_req_t *req) {
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
      httpd_query_key_value(query, "text", a.text, sizeof(a.text));
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
               cJSON *t = cJSON_GetObjectItem(root, "text");
               cJSON *s = cJSON_GetObjectItem(root, "submit");
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
                           a.resolved_target[0] ? a.resolved_target : (a.target[0] ? a.target : "chat"));
   if (a.ok) {
      cJSON_AddNumberToObject(root, "accepted", a.accepted);
      cJSON_AddBoolToObject(root, "submitted", a.submitted);
   } else {
      cJSON_AddStringToObject(root, "error", a.err[0] ? a.err : "unknown");
   }
   return send_json_resp(req, root);
}

void debug_server_input_register(httpd_handle_t server) {
   if (!server) return;

   static const httpd_uri_t uri_touch = {.uri = "/touch", .method = HTTP_POST, .handler = touch_handler};
   static const httpd_uri_t uri_input_text = {.uri = "/input/text", .method = HTTP_POST, .handler = input_text_handler};

   httpd_register_uri_handler(server, &uri_touch);
   httpd_register_uri_handler(server, &uri_input_text);
}
