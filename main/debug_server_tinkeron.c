/*
 * debug_server_tinkeron.c — TinkerON (K144) health + control HTTP family.
 *
 * TT #578 (2026-05-18): Tab5 debug server family that gives the user
 * runtime visibility + control over the always-on wakeword listener
 * AND the underlying K144 module.  Builds on the wakeword work in
 * PR #576 (`feat/wakeword`).  The existing m5 endpoint family stays
 * as the low-level K144 surface; tinkeron is the product-level layer
 * that combines wakeword state + K144 hwinfo into a single dashboard.
 *
 * Endpoints:
 *   GET  /tinkeron/status        — wakeword + K144 hwinfo combined
 *   POST /tinkeron/arm?on=1|0    — start / stop the listener at runtime
 *   POST /tinkeron/wake_phrase   — change wake phrase at runtime
 *   POST /tinkeron/reboot        — full K144 Linux reboot via sys.reboot
 *   GET  /tinkeron/transcripts   — recent ASR partials (debug tail)
 */

#include "debug_server_tinkeron.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug_obs.h"             /* tab5_debug_obs_event */
#include "debug_server_internal.h" /* tab5_debug_check_auth */
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"       /* TT #617 — wake_src */
#include "task_worker.h"    /* tab5_worker_enqueue */
#include "voice_m5_llm.h"   /* sys_reboot, hwinfo accessor */
#include "voice_onboard.h"  /* failover state names */
#include "voice_wakeword.h" /* status + reconfigure_phrase + transcripts */

static const char *TAG = "debug_tinkeron";

/* httpd_query_key_value does NOT URL-decode the value.  Decode in-place:
 * %XX → byte, + → space.  Length doesn't grow, so this is safe. */
static void url_decode_inplace(char *s) {
   if (s == NULL) return;
   char *w = s;
   for (char *r = s; *r; r++) {
      if (*r == '+') {
         *w++ = ' ';
      } else if (*r == '%' && r[1] && r[2]) {
         int hi = r[1], lo = r[2];
         int hv = (hi >= '0' && hi <= '9') ? hi - '0'
                  : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                  : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
         int lv = (lo >= '0' && lo <= '9') ? lo - '0'
                  : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                  : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
         if (hv >= 0 && lv >= 0) {
            *w++ = (char)((hv << 4) | lv);
            r += 2;
         } else {
            *w++ = *r;
         }
      } else {
         *w++ = *r;
      }
   }
   *w = '\0';
}

/* Helper: read a URL-decoded query value into out (returns true on hit). */
static bool query_get(httpd_req_t *req, const char *key, char *out, size_t cap) {
   size_t qlen = httpd_req_get_url_query_len(req);
   if (qlen == 0 || qlen + 1 > 512) return false;
   char qbuf[512];
   if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) return false;
   if (httpd_query_key_value(qbuf, key, out, cap) != ESP_OK) return false;
   url_decode_inplace(out);
   return true;
}

static esp_err_t respond_error(httpd_req_t *req, const char *msg, int status_code) {
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "error", msg ? msg : "");
   char *body = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (body == NULL) {
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_sendstr(req, "{\"error\":\"json_print_failed\"}");
   }
   char status[32];
   snprintf(status, sizeof(status), "%d Error", status_code);
   httpd_resp_set_status(req, status);
   httpd_resp_set_type(req, "application/json");
   esp_err_t r = httpd_resp_sendstr(req, body);
   cJSON_free(body);
   return r;
}

static esp_err_t respond_json(httpd_req_t *req, cJSON *obj, int status_code) {
   char *body = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (body == NULL) {
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_sendstr(req, "{\"error\":\"json_print_failed\"}");
   }
   if (status_code != 200) {
      char status[32];
      snprintf(status, sizeof(status), "%d Error", status_code);
      httpd_resp_set_status(req, status);
   }
   httpd_resp_set_type(req, "application/json");
   esp_err_t r = httpd_resp_sendstr(req, body);
   cJSON_free(body);
   return r;
}

/* ── GET /tinkeron/status ─────────────────────────────────────────── */

static esp_err_t handle_status(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_FAIL;

   voice_wakeword_status_t ww = {0};
   voice_wakeword_status(&ww);

   cJSON *root = cJSON_CreateObject();
   /* Wakeword listener */
   cJSON *w = cJSON_AddObjectToObject(root, "wakeword");
   cJSON_AddBoolToObject(w, "armed", ww.armed);
   cJSON_AddStringToObject(w, "wake_phrase", ww.wake_phrase);
   cJSON_AddStringToObject(w, "wake_phrase_alt", ww.wake_phrase_alt);
   cJSON_AddStringToObject(w, "end_phrase", ww.end_phrase);
   cJSON_AddNumberToObject(w, "fire_count", ww.fire_count);
   cJSON_AddNumberToObject(w, "vad_skip_count", ww.vad_skip_count);
   cJSON_AddNumberToObject(w, "last_fire_ms", (double)ww.last_fire_ms);
   cJSON_AddStringToObject(w, "last_match", ww.last_match);

   /* K144 hardware snapshot (best-effort; we use the live hwinfo path
    * which is itself 30s-cached on the underlying voice_m5_llm side). */
   cJSON *k = cJSON_AddObjectToObject(root, "k144");
   voice_m5_hwinfo_t hw;
   if (voice_m5_llm_sys_hwinfo(&hw) == ESP_OK) {
      cJSON_AddBoolToObject(k, "hwinfo_valid", true);
      cJSON_AddNumberToObject(k, "temp_celsius", hw.temperature_milli_c / 1000.0);
      cJSON_AddNumberToObject(k, "temp_milli_c", hw.temperature_milli_c);
      cJSON_AddNumberToObject(k, "cpu_loadavg", hw.cpu_loadavg);
      cJSON_AddNumberToObject(k, "mem", hw.mem);
   } else {
      cJSON_AddBoolToObject(k, "hwinfo_valid", false);
   }
   char ver[16] = {0};
   if (voice_m5_llm_sys_version(ver, sizeof(ver)) == ESP_OK) {
      cJSON_AddStringToObject(k, "version", ver);
   }

   cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));

   /* TT #617 — surface current wake source */
   {
      char src[16] = {0};
      tab5_settings_get_wake_src(src, sizeof(src));
      cJSON_AddStringToObject(root, "wake_src", src);
   }

   return respond_json(req, root, 200);
}

/* ── POST /tinkeron/wake_src?src=k144|dragon|ext_pcm|off ─────────── */

static esp_err_t handle_wake_src(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_FAIL;
   char qry[64] = {0};
   if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
      return respond_error(req, "missing src query param", 400);
   }
   char src[16] = {0};
   if (httpd_query_key_value(qry, "src", src, sizeof(src)) != ESP_OK) {
      return respond_error(req, "missing src= param", 400);
   }
   if (strcmp(src, "k144") != 0 && strcmp(src, "dragon") != 0 &&
       strcmp(src, "ext_pcm") != 0 && strcmp(src, "off") != 0) {
      return respond_error(req, "src must be k144|dragon|ext_pcm|off", 400);
   }
   esp_err_t e = tab5_settings_set_wake_src(src);
   if (e != ESP_OK) {
      return respond_error(req, esp_err_to_name(e), 500);
   }
   ESP_LOGI(TAG, "/tinkeron/wake_src: set to %s", src);
   tab5_debug_obs_event("wake_src", src);

   /* Apply immediately: disarm whatever is wrong, arm whatever is right. */
   extern void voice_wake_stream_disarm(void);
   extern void voice_wake_stream_arm(void);
   extern void voice_ext_pcm_stream_disarm(void);
   extern void voice_ext_pcm_stream_arm(void);
   extern esp_err_t voice_onboard_arm_wakeword(void);

   extern esp_err_t voice_onboard_arm_wakeword_async(void);
   if (strcmp(src, "k144") == 0) {
      voice_wake_stream_disarm();
      voice_ext_pcm_stream_disarm();
      voice_onboard_arm_wakeword_async();
   } else if (strcmp(src, "dragon") == 0) {
      voice_wakeword_stop();
      voice_ext_pcm_stream_disarm();
      voice_wake_stream_arm();
   } else if (strcmp(src, "ext_pcm") == 0) {
      voice_wake_stream_disarm();
      /* Arm voice_wakeword asynchronously so its asr.setup runs on the
       * worker.  The ext_pcm pump is gated on voice_wakeword_is_active()
       * + K144 READY so it won't fight the arm for UART. */
      voice_onboard_arm_wakeword_async();
      voice_ext_pcm_stream_arm();
   } else { /* off */
      voice_wakeword_stop();
      voice_wake_stream_disarm();
      voice_ext_pcm_stream_disarm();
   }

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "status", "applied");
   cJSON_AddStringToObject(root, "wake_src", src);
   return respond_json(req, root, 200);
}

/* ── POST /tinkeron/arm?on=1|0 ────────────────────────────────────── */

static void arm_start_job(void *arg) {
   (void)arg;
   /* Need an event handler — re-use the canonical one from voice_onboard.
    * voice_wakeword_reconfigure_phrase() with the CURRENT phrase
    * effectively re-arms with cached cb/user.  Simpler: just call start
    * with NULL config (defaults) + no callback.  But that drops the UI
    * bridge.  Better path: read the cached phrase via status, then
    * reconfigure with it — that re-uses the cached cb/user. */
   voice_wakeword_status_t st;
   voice_wakeword_status(&st);
   const char *phrase = st.wake_phrase[0] ? st.wake_phrase : "hey tinker";
   esp_err_t e = voice_wakeword_reconfigure_phrase(phrase);
   if (e != ESP_OK) {
      ESP_LOGW(TAG, "arm_start: reconfigure failed (%s)", esp_err_to_name(e));
   }
}

static esp_err_t handle_arm(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_FAIL;
   char on[8] = {0};
   if (!query_get(req, "on", on, sizeof(on))) {
      cJSON *err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "error", "missing_query_on");
      cJSON_AddStringToObject(err, "detail", "expected ?on=1 or ?on=0");
      return respond_json(req, err, 400);
   }

   bool want_on = (on[0] == '1' || on[0] == 't' || on[0] == 'T');
   bool currently = voice_wakeword_is_active();
   const char *action;

   if (want_on && !currently) {
      tab5_worker_enqueue(arm_start_job, NULL, "tinkeron_arm");
      action = "starting";
   } else if (!want_on && currently) {
      voice_wakeword_stop();
      action = "stopped";
   } else {
      action = want_on ? "already_armed" : "already_stopped";
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", action);
   cJSON_AddBoolToObject(resp, "armed", voice_wakeword_is_active());
   return respond_json(req, resp, 200);
}

/* ── POST /tinkeron/wake_phrase?phrase=hey+tink ──────────────────── */

static esp_err_t handle_wake_phrase(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_FAIL;
   char phrase[64] = {0};
   if (!query_get(req, "phrase", phrase, sizeof(phrase)) || phrase[0] == '\0') {
      cJSON *err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "error", "missing_query_phrase");
      cJSON_AddStringToObject(err, "detail", "expected ?phrase=hey+tinker");
      return respond_json(req, err, 400);
   }
   esp_err_t e = voice_wakeword_reconfigure_phrase(phrase);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "phrase", phrase);
   cJSON_AddStringToObject(resp, "status", e == ESP_OK ? "armed" : esp_err_to_name(e));
   cJSON_AddBoolToObject(resp, "armed", voice_wakeword_is_active());
   return respond_json(req, resp, e == ESP_OK ? 200 : 500);
}

/* ── POST /tinkeron/reboot ──────────────────────────────────────────
 *
 * Async — kicks the reboot on tab5_worker, returns 202 immediately.
 * Caller polls GET /tinkeron/status to see when K144 comes back. */

static void reboot_job(void *arg) {
   (void)arg;
   /* Tear down the wakeword task first so its UART drain doesn't fight
    * the rebooting K144. */
   voice_wakeword_stop();
   ESP_LOGI(TAG, "/tinkeron/reboot: sending sys.reboot...");
   esp_err_t e = voice_m5_llm_sys_reboot();
   if (e == ESP_OK) {
      ESP_LOGI(TAG, "sys.reboot acked — K144 will come back in ~30 s");
   } else {
      ESP_LOGW(TAG, "sys.reboot returned %s", esp_err_to_name(e));
   }
   /* Caller-side warmup re-trigger lives in voice_onboard_reset_failover;
    * we don't auto-invoke it here because the user may want to observe
    * the cold state via /tinkeron/status first. */
}

static esp_err_t handle_reboot(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_FAIL;
   tab5_worker_enqueue(reboot_job, NULL, "tinkeron_reboot");
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "queued");
   cJSON_AddStringToObject(resp, "detail",
                           "sys.reboot dispatched. K144 will Linux-reboot (~30 s); "
                           "poll GET /tinkeron/status. Call POST /m5/reset afterward "
                           "to re-warm the failover chain.");
   return respond_json(req, resp, 202);
}

/* ── GET /tinkeron/transcripts?n=20 ───────────────────────────────── */

#define TRANSCRIPT_MAX_RETURN 32

static esp_err_t handle_transcripts(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_FAIL;
   char nbuf[8] = {0};
   size_t want = 20;
   if (query_get(req, "n", nbuf, sizeof(nbuf))) {
      int n = atoi(nbuf);
      if (n > 0 && n <= TRANSCRIPT_MAX_RETURN) want = (size_t)n;
   }
   voice_wakeword_transcript_t buf[TRANSCRIPT_MAX_RETURN];
   size_t got = voice_wakeword_get_recent_transcripts(buf, want);

   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "transcripts");
   for (size_t i = 0; i < got; i++) {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddNumberToObject(e, "ms", (double)buf[i].ms);
      cJSON_AddBoolToObject(e, "finish", buf[i].finish);
      cJSON_AddStringToObject(e, "text", buf[i].text);
      cJSON_AddItemToArray(arr, e);
   }
   cJSON_AddNumberToObject(root, "count", got);
   return respond_json(req, root, 200);
}

/* ── Registration ─────────────────────────────────────────────────── */

void debug_server_tinkeron_register(httpd_handle_t server) {
   static const httpd_uri_t uri_status = {
       .uri = "/tinkeron/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = NULL,
   };
   static const httpd_uri_t uri_arm = {
       .uri = "/tinkeron/arm", .method = HTTP_POST, .handler = handle_arm, .user_ctx = NULL,
   };
   static const httpd_uri_t uri_phrase = {
       .uri = "/tinkeron/wake_phrase", .method = HTTP_POST, .handler = handle_wake_phrase, .user_ctx = NULL,
   };
   static const httpd_uri_t uri_reboot = {
       .uri = "/tinkeron/reboot", .method = HTTP_POST, .handler = handle_reboot, .user_ctx = NULL,
   };
   static const httpd_uri_t uri_transcripts = {
       .uri = "/tinkeron/transcripts", .method = HTTP_GET, .handler = handle_transcripts, .user_ctx = NULL,
   };
   static const httpd_uri_t uri_wake_src = {
       .uri = "/tinkeron/wake_src",
       .method = HTTP_POST,
       .handler = handle_wake_src,
       .user_ctx = NULL,
   };
   httpd_register_uri_handler(server, &uri_status);
   httpd_register_uri_handler(server, &uri_arm);
   httpd_register_uri_handler(server, &uri_phrase);
   httpd_register_uri_handler(server, &uri_reboot);
   httpd_register_uri_handler(server, &uri_transcripts);
   httpd_register_uri_handler(server, &uri_wake_src);
   ESP_LOGI(TAG, "TinkerON debug family registered (6 endpoints)");
}
