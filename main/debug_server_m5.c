/*
 * debug_server_m5.c — K144 (M5Stack LLM Module) HTTP endpoint family.
 *
 * Wave 23b (#332): extracted verbatim from debug_server.c as the
 * proof-of-pattern for the per-family split called for in the
 * cross-stack architecture audit (2026-05-01).  The handlers + helpers
 * + statics moved over with zero behavior change; the only difference
 * is the `check_auth(req)` calls now go through the shared wrapper
 * `tab5_debug_check_auth(req)` declared in debug_server_internal.h.
 *
 * The two-tier caching strategy (hwinfo: 30 s success TTL + 5 s attempt
 * rate-limit; modelist: 5 min PSRAM-lazy) was added in TT #328 Wave 14
 * and Wave 15 respectively — see CLAUDE.md "K144 hwinfo cache freshness
 * lie" + the LEARNINGS entry on PSRAM-lazy patterns.  The historical
 * comments are preserved verbatim.
 */

#include "debug_server_m5.h"

#include "cJSON.h"
#include "debug_server_internal.h" /* tab5_debug_check_auth */
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"      /* TT #620 W3: tab5_settings_get/set_xport */
#include "voice_m5_llm.h"  /* TT #327 Wave 5: K144 baud accessor for /m5 */
#include "voice_onboard.h" /* TT #327 Wave 4b: chain_active + failover_state */
#include "voice_usb_cdc.h" /* TT #620 W2: USB transport connection state for /m5 */
#include "voice_usb_ffs.h" /* V0 — StackFlow relay endpoint */
#include "voice_xport.h"   /* TT #620 W3: active transport name */
#include "voice_yolo.h"    /* TT #621 W6: K144 yolo11n inference */

static const char *TAG = "debug_m5";

/* ── Cached hwinfo + version snapshot ────────────────────────────────
 *
 * Wave 14 — cached hwinfo + version snapshot.  GET /m5 may be polled
 * by the dashboard / harness every few seconds; refreshing sys.hwinfo
 * on every call would saturate the UART (~150 ms per round-trip ×
 * concurrent polls = head-of-line blocking on llm.infer calls).  Cache
 * for 30 s; clients that want fresh data poll less often or use
 * POST /m5/refresh to force an update.
 *
 * Also avoids the worst-case path where K144 is hung mid-NPU-load and
 * sys.hwinfo silently stalls — every /m5 GET would otherwise block
 * 1.5 s on the timeout.  The cache bounds this to once per 30 s. */
static voice_m5_hwinfo_t s_m5_hwinfo_cache = {0};
static char s_m5_version_cache[16] = {0};
static int64_t s_m5_hwinfo_refreshed_us = 0; /* time of last SUCCESS */
static int64_t s_m5_hwinfo_attempted_us = 0; /* time of last ATTEMPT (success OR skip) */
#define M5_HWINFO_CACHE_TTL_US (30LL * 1000LL * 1000LL)
#define M5_HWINFO_RETRY_GATE_US (5LL * 1000LL * 1000LL) /* 5 s — bounds rate when K144 not READY */

static void m5_refresh_hwinfo_if_stale(bool force) {
   int64_t now = esp_timer_get_time();
   /* Cache hit fast-path — last successful fetch within TTL window. */
   if (!force && s_m5_hwinfo_refreshed_us != 0 && (now - s_m5_hwinfo_refreshed_us) < M5_HWINFO_CACHE_TTL_US) {
      return;
   }
   /* Even if cache is stale, throttle attempts (avoids hammering the
    * UART when /m5 is polled rapidly during a recovery cycle).  5 s
    * is short enough that a UNAVAILABLE→READY transition picks up new
    * data within ~10 s; long enough that 50 polls/sec from a script
    * don't queue 50 sys.hwinfo round-trips. */
   if (!force && s_m5_hwinfo_attempted_us != 0 && (now - s_m5_hwinfo_attempted_us) < M5_HWINFO_RETRY_GATE_US) {
      return;
   }
   s_m5_hwinfo_attempted_us = now;

   /* Skip the actual sys.hwinfo if K144 isn't READY — it would just
    * time out and waste 1.5 s.  Cached values stay; client sees old
    * data with `cache_age_ms` shown so they can decide what to trust. */
   int fs = voice_onboard_failover_state();
   if (fs != 2 /* M5_FAIL_READY */) {
      return;
   }

   voice_m5_hwinfo_t fresh = {0};
   esp_err_t he = voice_m5_llm_sys_hwinfo(&fresh);
   if (he == ESP_OK && fresh.valid) {
      s_m5_hwinfo_cache = fresh;
      s_m5_hwinfo_refreshed_us = now;
   }
   /* Version is fixed per K144 firmware install — read once and cache.
    * Cache survives sys.reset (daemon restart, version unchanged) but
    * does NOT survive a Tab5 reboot. */
   if (s_m5_version_cache[0] == '\0') {
      (void)voice_m5_llm_sys_version(s_m5_version_cache, sizeof(s_m5_version_cache));
   }
}

static esp_err_t m5_status_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   /* Wave 14 — refresh hwinfo cache (no-op if warm + fresh enough). */
   m5_refresh_hwinfo_if_stale(false);

   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "chain_active", voice_onboard_chain_active());
   /* Wave 7: chain_uptime_ms — 0 if not active.  Lets a remote operator
    * spot a stuck chain without a serial cable. */
   cJSON_AddNumberToObject(root, "chain_uptime_ms", (double)voice_onboard_chain_uptime_ms());
   int fs = voice_onboard_failover_state();
   cJSON_AddNumberToObject(root, "failover_state", fs);
   const char *fs_names[] = {"unknown", "probing", "ready", "unavailable"};
   cJSON_AddStringToObject(root, "failover_state_name", (fs >= 0 && fs <= 3) ? fs_names[fs] : "?");
   cJSON_AddNumberToObject(root, "uart_baud", (double)voice_m5_llm_get_baud());

   /* TT #620 W2 — USB CDC-ACM transport state.  Tab5's USB-A host port
    * polls for the K144 composite gadget (vid=0x32c9 pid=0x2003 intf=1);
    * `usb_cdc_connected` flips true the moment K144 enumerates. */
   cJSON *usb = cJSON_CreateObject();
   cJSON_AddBoolToObject(usb, "init", voice_usb_cdc_is_initialized());
   cJSON_AddBoolToObject(usb, "connected", voice_usb_cdc_is_connected());
   cJSON_AddItemToObject(root, "usb_cdc", usb);

   /* TT #620 W3 — active transport (uart vs usb_cdc), driven by NVS
    * `xport` key.  Surfaces so a remote operator can verify which wire
    * the StackFlow JSON is travelling on without ssh + serial logs. */
   cJSON_AddStringToObject(root, "xport", voice_xport_name());

   /* Wave 14 — hardware status.  `valid` is true only when the cache
    * holds a successfully-parsed sys.hwinfo response; `cache_age_ms`
    * tells the client how stale the snapshot is.  `temp_celsius`
    * comes from the milli-degree field divided by 1000 (preserving
    * one decimal — JSON consumer can format). */
   cJSON *hw = cJSON_CreateObject();
   bool hw_valid = (s_m5_hwinfo_cache.valid != 0);
   cJSON_AddBoolToObject(hw, "valid", hw_valid);
   if (hw_valid) {
      double temp_c = (double)s_m5_hwinfo_cache.temperature_milli_c / 1000.0;
      cJSON_AddNumberToObject(hw, "temp_celsius", temp_c);
      cJSON_AddNumberToObject(hw, "temp_milli_c", (double)s_m5_hwinfo_cache.temperature_milli_c);
      cJSON_AddNumberToObject(hw, "cpu_loadavg", (double)s_m5_hwinfo_cache.cpu_loadavg);
      cJSON_AddNumberToObject(hw, "mem", (double)s_m5_hwinfo_cache.mem);
   }
   if (s_m5_hwinfo_refreshed_us != 0) {
      int64_t age_ms = (esp_timer_get_time() - s_m5_hwinfo_refreshed_us) / 1000;
      cJSON_AddNumberToObject(hw, "cache_age_ms", (double)age_ms);
   }
   cJSON_AddItemToObject(root, "hwinfo", hw);

   /* Wave 14 — daemon version (read once on first hwinfo refresh,
    * empty until then).  Surfaces "is K144 firmware up-to-date" + lets
    * remote diagnostics correlate behavior with daemon version. */
   cJSON_AddStringToObject(root, "version", s_m5_version_cache);

   /* TT #629 Wave C.4 — cached work_id snapshot + ms since last reset.
    * Lets soak tests verify that R12/R12b invalidation actually clears
    * the cached llm/tts/yolo handles after sys.reset:recovered. */
   cJSON *work_ids = cJSON_CreateObject();
   char llm_wid[32], tts_wid[32];
   voice_m5_llm_get_work_ids(llm_wid, sizeof(llm_wid), tts_wid, sizeof(tts_wid));
   cJSON_AddStringToObject(work_ids, "llm", llm_wid);
   cJSON_AddStringToObject(work_ids, "tts", tts_wid);
   cJSON_AddBoolToObject(work_ids, "yolo_ready", voice_yolo_is_ready());
   cJSON_AddItemToObject(root, "work_ids", work_ids);
   cJSON_AddNumberToObject(root, "ms_since_last_reset", (double)voice_onboard_ms_since_last_reset());

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* TT #328 Wave 14 — POST /m5/refresh.  Forces a sys.hwinfo refresh
 * regardless of the 30 s cache TTL.  Useful when a remote operator or
 * the e2e harness wants a guaranteed-fresh reading.  Returns the same
 * shape as GET /m5 with the cache freshly populated. */
static esp_err_t m5_refresh_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   m5_refresh_hwinfo_if_stale(true);
   return m5_status_handler(req);
}

/* TT #328 Wave 15 — GET /m5/models.  Surfaces the K144 model registry
 * (sys.lsmode response) so the dashboard + e2e harness can see what's
 * installed.  Cached for 5 min — sys.lsmode is ~50 ms warm but the
 * registry doesn't change between K144 reboots, so re-fetching often
 * is wasteful.  ?force=1 bypasses the cache for a fresh fetch.
 *
 * Cache lives in PSRAM (heap_caps_calloc, lazy on first request) —
 * NOT BSS-static.  An earlier draft used a BSS-static
 * voice_m5_modelist_t (~1.8 KB) and tripped the same boot-loop
 * `vApplicationGetTimerTaskMemory` assert as Wave 11 / Wave 13's
 * first attempt.  See LEARNINGS "BSS-static caches >3 KB push Tab5
 * over a boot SRAM threshold" — same lesson, different size band:
 * we hit the threshold around ~1.8 KB on top of Wave 14's BSS
 * additions.  Lazy PSRAM is the universal fix. */
static voice_m5_modelist_t *s_m5_modelist_cache = NULL;
static int64_t s_m5_modelist_refreshed_us = 0;
#define M5_MODELLIST_CACHE_TTL_US (5LL * 60LL * 1000LL * 1000LL) /* 5 min */

static void m5_refresh_modelist_if_stale(bool force) {
   int64_t now = esp_timer_get_time();
   if (!force && s_m5_modelist_cache != NULL && s_m5_modelist_cache->valid &&
       (now - s_m5_modelist_refreshed_us) < M5_MODELLIST_CACHE_TTL_US) {
      return;
   }
   if (voice_onboard_failover_state() != 2 /* M5_FAIL_READY */) {
      return; /* Don't waste 3 s on UART timeout when K144 is offline */
   }
   if (s_m5_modelist_cache == NULL) {
      s_m5_modelist_cache = heap_caps_calloc(1, sizeof(*s_m5_modelist_cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (s_m5_modelist_cache == NULL) {
         ESP_LOGE(TAG, "modelist PSRAM alloc failed");
         return;
      }
   }
   voice_m5_modelist_t fresh = {0};
   if (voice_m5_llm_sys_lsmode(&fresh) == ESP_OK && fresh.valid) {
      memcpy(s_m5_modelist_cache, &fresh, sizeof(fresh));
      s_m5_modelist_refreshed_us = now;
   }
}

static esp_err_t m5_models_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   bool force = false;
   {
      char qbuf[64];
      if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
         char val[8];
         if (httpd_query_key_value(qbuf, "force", val, sizeof(val)) == ESP_OK && (val[0] == '1' || val[0] == 't')) {
            force = true;
         }
      }
   }
   m5_refresh_modelist_if_stale(force);

   cJSON *root = cJSON_CreateObject();
   bool valid = (s_m5_modelist_cache != NULL && s_m5_modelist_cache->valid);
   cJSON_AddBoolToObject(root, "valid", valid);
   cJSON_AddNumberToObject(root, "count", valid ? (double)s_m5_modelist_cache->n : 0.0);
   cJSON *arr = cJSON_AddArrayToObject(root, "models");
   if (valid) {
      for (int i = 0; i < s_m5_modelist_cache->n; i++) {
         cJSON *m = cJSON_CreateObject();
         cJSON_AddStringToObject(m, "mode", s_m5_modelist_cache->models[i].mode);
         cJSON_AddStringToObject(m, "primary_cap", s_m5_modelist_cache->models[i].primary_cap);
         cJSON_AddStringToObject(m, "language", s_m5_modelist_cache->models[i].language);
         cJSON_AddItemToArray(arr, m);
      }
   }
   if (s_m5_modelist_refreshed_us != 0) {
      int64_t age_s = (esp_timer_get_time() - s_m5_modelist_refreshed_us) / 1000000;
      cJSON_AddNumberToObject(root, "cache_age_s", (double)age_s);
   }
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* TT #328 Wave 13 — POST /m5/reset.  Triggers
 * voice_onboard_reset_failover(), which sends sys.reset to the K144
 * StackFlow daemon, waits for it to come back, and re-runs the warmup
 * probe.  The actual reset happens asynchronously on tab5_worker; the
 * caller can poll GET /m5 to observe state cycle PROBING → READY (or
 * UNAVAILABLE on continued failure).
 *
 * Useful for both the e2e harness (verifies recovery round-trip
 * without UI tap geometry) and dashboard / remote-operator debugging
 * (a Tab5 stuck in UNAVAILABLE can be unstuck without a reboot or a
 * physical tap on the Settings health chip). */
static esp_err_t m5_reset_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   esp_err_t qe = voice_onboard_reset_failover();
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "status", qe == ESP_OK ? "queued" : "rejected");
   cJSON_AddStringToObject(root, "detail",
                           qe == ESP_OK ? "K144 reset job enqueued — poll GET /m5"
                                        : "Probe already in flight — try again "
                                          "after current cycle finishes");
   cJSON_AddNumberToObject(root, "failover_state", voice_onboard_failover_state());
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* TT #620 W3 (+ TT #621 usb_ffs) — POST /m5/xport?x=uart|usb_cdc|usb_ffs.
 *
 * Flips the active Tab5↔K144 transport: writes the NVS `xport` key +
 * re-applies via voice_xport_init.  Falls back to uart if the requested
 * USB backend hasn't enumerated K144 yet. */
static esp_err_t m5_xport_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   char query[64] = {0};
   uint8_t want = UINT8_MAX;
   if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      char value[16] = {0};
      if (httpd_query_key_value(query, "x", value, sizeof(value)) == ESP_OK) {
         if (strcmp(value, "uart") == 0 || strcmp(value, "0") == 0)
            want = 0;
         else if (strcmp(value, "usb_cdc") == 0 || strcmp(value, "1") == 0)
            want = 1;
         else if (strcmp(value, "usb_ffs") == 0 || strcmp(value, "2") == 0)
            want = 2;
      }
   }

   if (want > 2) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_set_type(req, "application/json");
      return httpd_resp_sendstr(req, "{\"error\":\"x must be uart|usb_cdc|usb_ffs|0|1|2\"}");
   }

   tab5_settings_set_xport(want);
   voice_xport_init(3000);

   const char *names[] = {"uart", "usb_cdc", "usb_ffs"};
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "xport_requested", names[want]);
   cJSON_AddStringToObject(root, "xport_active", voice_xport_name());
   cJSON_AddBoolToObject(root, "ready", voice_xport_is_ready());
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* TT #621 W6 — POST /yolo/infer with a JPEG body.  Drives K144 yolo11n
 * via voice_yolo + voice_xport (USB ffs.control bridge), returns the
 * detection boxes as JSON.  Capped to 64 KB body for the first cut.
 *
 *   curl -H "Authorization: Bearer $TOK" --data-binary @frame_320.jpg \
 *        http://<tab5>:8080/yolo/infer
 */
static esp_err_t m5_yolo_infer_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   int total = req->content_len;
   if (total <= 0 || total > 64 * 1024) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_sendstr(req, "{\"error\":\"body must be 1..65536 bytes JPEG\"}");
   }

   /* PSRAM-back the body buffer.  Default malloc would put a 30-60 KB
    * frame into already-tight internal SRAM and starve Wi-Fi. */
   uint8_t *jpeg = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!jpeg) {
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_sendstr(req, "{\"error\":\"alloc fail\"}");
   }
   int got = 0;
   while (got < total) {
      int n = httpd_req_recv(req, (char *)(jpeg + got), total - got);
      if (n <= 0) {
         heap_caps_free(jpeg);
         return ESP_FAIL;
      }
      got += n;
   }

   if (!voice_yolo_is_ready()) {
      esp_err_t ie = voice_yolo_init();
      if (ie != ESP_OK) {
         free(jpeg);
         httpd_resp_set_status(req, "503 Service Unavailable");
         char body[96];
         snprintf(body, sizeof(body), "{\"error\":\"yolo_init: %s\"}", esp_err_to_name(ie));
         return httpd_resp_sendstr(req, body);
      }
   }

   voice_yolo_box_t boxes[16];
   size_t n_boxes = 0;
   esp_err_t err = voice_yolo_infer(jpeg, total, boxes, 16, &n_boxes, 5000);
   heap_caps_free(jpeg);

   if (err != ESP_OK) {
      httpd_resp_set_status(req, "504 Gateway Timeout");
      char body[96];
      snprintf(body, sizeof(body), "{\"error\":\"infer: %s\"}", esp_err_to_name(err));
      return httpd_resp_sendstr(req, body);
   }

   cJSON *root = cJSON_CreateObject();
   cJSON_AddNumberToObject(root, "count", n_boxes);
   cJSON *arr = cJSON_AddArrayToObject(root, "boxes");
   for (size_t i = 0; i < n_boxes; i++) {
      cJSON *b = cJSON_CreateObject();
      cJSON_AddStringToObject(b, "class", boxes[i].klass);
      cJSON_AddNumberToObject(b, "confidence", boxes[i].confidence);
      cJSON_AddNumberToObject(b, "x", boxes[i].x);
      cJSON_AddNumberToObject(b, "y", boxes[i].y);
      cJSON_AddNumberToObject(b, "w", boxes[i].w);
      cJSON_AddNumberToObject(b, "h", boxes[i].h);
      cJSON_AddItemToArray(arr, b);
   }
   char *out = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   esp_err_t ret = httpd_resp_sendstr(req, out);
   free(out);
   return ret;
}

/* ── V0 (TT #686): StackFlow relay ───────────────────────────────────
 *
 * Take a JSON body + optional ?ch=control|video, send it to the
 * matching K144 USB-FFS channel, drain the response for a bounded
 * timeout, return all raw response lines.
 *
 * Used to probe arbitrary StackFlow verbs (e.g. internvl.setup) from
 * the dev host without needing to physically swap the K144 USB-C
 * cable from Tab5 to dev box for ADB access.
 *
 * Request body: any JSON object — relayed verbatim with a newline
 *               appended (StackFlow's framing).
 * Query:        ?ch=control (default) or ?ch=video
 *               ?timeout=NNNN (default 3000 ms)
 *
 * Response: {"ok":true, "sent_bytes":N, "lines":["<json>", ...],
 *            "raw_bytes":M, "channel":"control|video"} */
#define RELAY_RESP_CAP (32 * 1024)

static esp_err_t m5_relay_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   if (req->content_len == 0 || req->content_len > 16384) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be 1..16384 bytes");
      return ESP_FAIL;
   }
   char *body = (char *)heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!body) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "body alloc");
      return ESP_FAIL;
   }
   int got = httpd_req_recv(req, body, req->content_len);
   if (got != (int)req->content_len) {
      heap_caps_free(body);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete body");
      return ESP_FAIL;
   }
   body[got] = '\0';

   /* Parse query for channel + timeout. */
   char query[64] = {0};
   httpd_req_get_url_query_str(req, query, sizeof(query));
   bool use_video = false;
   char ch[16] = {0};
   if (httpd_query_key_value(query, "ch", ch, sizeof(ch)) == ESP_OK) {
      use_video = (strcmp(ch, "video") == 0);
   }
   uint32_t timeout_ms = 3000;
   char tval[16] = {0};
   if (httpd_query_key_value(query, "timeout", tval, sizeof(tval)) == ESP_OK) {
      int t = atoi(tval);
      if (t >= 100 && t <= 15000) timeout_ms = (uint32_t)t;
   }

   bool connected = use_video ? voice_usb_ffs_video_is_connected() : voice_usb_ffs_is_connected();
   if (!connected) {
      heap_caps_free(body);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "k144 ffs channel not connected");
      return ESP_FAIL;
   }

   esp_err_t lock = use_video ? voice_usb_ffs_video_lock(2000) : voice_usb_ffs_lock(2000);
   if (lock != ESP_OK) {
      heap_caps_free(body);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ffs lock timeout");
      return ESP_FAIL;
   }

   /* Flush stale input + send the request with newline framing. */
   if (use_video)
      voice_usb_ffs_video_flush();
   else
      voice_usb_ffs_flush();

   int sent = use_video ? voice_usb_ffs_video_send(body, (size_t)got) : voice_usb_ffs_send(body, (size_t)got);
   const char nl = '\n';
   if (use_video)
      voice_usb_ffs_video_send(&nl, 1);
   else
      voice_usb_ffs_send(&nl, 1);

   /* Drain response into a PSRAM buffer until timeout elapses. */
   char *resp = (char *)heap_caps_malloc(RELAY_RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   size_t resp_len = 0;
   if (resp) {
      int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
      for (;;) {
         int64_t now = esp_timer_get_time();
         if (now >= deadline) break;
         uint32_t poll = (uint32_t)((deadline - now) / 1000);
         if (poll > 100) poll = 100;
         int n = use_video ? voice_usb_ffs_video_recv(resp + resp_len, RELAY_RESP_CAP - resp_len - 1, poll)
                           : voice_usb_ffs_recv(resp + resp_len, RELAY_RESP_CAP - resp_len - 1, poll);
         if (n > 0) {
            resp_len += (size_t)n;
            if (resp_len >= RELAY_RESP_CAP - 1) break;
         }
      }
      resp[resp_len] = '\0';
   }

   if (use_video)
      voice_usb_ffs_video_unlock();
   else
      voice_usb_ffs_unlock();

   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "ok", true);
   cJSON_AddNumberToObject(root, "sent_bytes", sent);
   cJSON_AddNumberToObject(root, "raw_bytes", (double)resp_len);
   cJSON_AddStringToObject(root, "channel", use_video ? "video" : "control");
   cJSON *lines = cJSON_CreateArray();
   if (resp) {
      char *line = resp;
      while (line && *line) {
         char *next = strchr(line, '\n');
         if (next) *next++ = '\0';
         if (*line) cJSON_AddItemToArray(lines, cJSON_CreateString(line));
         line = next;
      }
   }
   cJSON_AddItemToObject(root, "lines", lines);
   if (resp) heap_caps_free(resp);
   heap_caps_free(body);
   return tab5_debug_send_json_resp(req, root);
}

/* ── Public registration entry point ─────────────────────────────────
 * Called once from tab5_debug_server_start() during boot.  All five
 * URI structs are local to this function (matching the inline pattern
 * the rest of debug_server.c still uses for the other families). */
void debug_server_m5_register(httpd_handle_t server) {
   const httpd_uri_t uri_m5_status = {.uri = "/m5", .method = HTTP_GET, .handler = m5_status_handler};
   const httpd_uri_t uri_m5_reset = {.uri = "/m5/reset", .method = HTTP_POST, .handler = m5_reset_handler};
   const httpd_uri_t uri_m5_refresh = {.uri = "/m5/refresh", .method = HTTP_POST, .handler = m5_refresh_handler};
   const httpd_uri_t uri_m5_models = {.uri = "/m5/models", .method = HTTP_GET, .handler = m5_models_handler};
   const httpd_uri_t uri_m5_xport = {.uri = "/m5/xport", .method = HTTP_POST, .handler = m5_xport_handler};
   const httpd_uri_t uri_yolo_infer = {.uri = "/yolo/infer", .method = HTTP_POST, .handler = m5_yolo_infer_handler};
   const httpd_uri_t uri_m5_relay = {.uri = "/m5/relay", .method = HTTP_POST, .handler = m5_relay_handler};

   httpd_register_uri_handler(server, &uri_m5_status);
   httpd_register_uri_handler(server, &uri_m5_reset);
   httpd_register_uri_handler(server, &uri_m5_xport);
   httpd_register_uri_handler(server, &uri_m5_refresh);
   httpd_register_uri_handler(server, &uri_m5_models);
   httpd_register_uri_handler(server, &uri_yolo_infer);
   httpd_register_uri_handler(server, &uri_m5_relay);

   ESP_LOGI(TAG, "K144 endpoint family registered (6 URIs)");
}
