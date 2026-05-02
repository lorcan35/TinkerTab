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
#include "voice_m5_llm.h"  /* TT #327 Wave 5: K144 baud accessor for /m5 */
#include "voice_onboard.h" /* TT #327 Wave 4b: chain_active + failover_state */

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

/* ── Public registration entry point ─────────────────────────────────
 * Called once from tab5_debug_server_start() during boot.  All four
 * URI structs are local to this function (matching the inline pattern
 * the rest of debug_server.c still uses for the other families). */
void debug_server_m5_register(httpd_handle_t server) {
   const httpd_uri_t uri_m5_status = {.uri = "/m5", .method = HTTP_GET, .handler = m5_status_handler};
   const httpd_uri_t uri_m5_reset = {.uri = "/m5/reset", .method = HTTP_POST, .handler = m5_reset_handler};
   const httpd_uri_t uri_m5_refresh = {.uri = "/m5/refresh", .method = HTTP_POST, .handler = m5_refresh_handler};
   const httpd_uri_t uri_m5_models = {.uri = "/m5/models", .method = HTTP_GET, .handler = m5_models_handler};

   httpd_register_uri_handler(server, &uri_m5_status);
   httpd_register_uri_handler(server, &uri_m5_reset);
   httpd_register_uri_handler(server, &uri_m5_refresh);
   httpd_register_uri_handler(server, &uri_m5_models);

   ESP_LOGI(TAG, "K144 endpoint family registered (4 URIs)");
}
