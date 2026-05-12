/*
 * ui_agents.c — v5 Agents overlay (minimal)
 *
 * TT #328 Wave 6 — was a dead surface (static placeholder cards),
 * Wave 6 revives it by adding a live "TOOLS CATALOG" section below
 * the existing AGENT ACTIVITY entry.  On show the overlay fires an
 * HTTP fetch to Dragon's `GET /api/v1/tools`, parses the JSON list,
 * and renders each tool as a typographic card with name + short
 * description.  The user can finally see WHAT the agent can do
 * (web_search, recall, datetime, …) and not just stale activity.
 *
 * Fetch happens on tab5_worker_enqueue so the LVGL thread doesn't
 * block on the HTTP round-trip; parse runs on the worker, results
 * are handed to the UI via tab5_lv_async_call which renders the
 * cards.  Offline / fetch-fail states render a hint instead.
 */

#include "ui_agents.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "config.h"
#include "debug_obs.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings.h"
#include "task_worker.h"
#include "tool_log.h"
#include "ui_core.h"
#include "ui_home.h"
#include "ui_theme.h"

static const char *TAG = "ui_agents";

#define SW        720
#define SH        1280
#define SIDE_PAD  52

/* TT #328 Wave 6 — tools catalog cap.  Dragon currently registers 10
 * built-in tools (web_search, remember, recall, datetime, calculator,
 * unit_converter, weather, system, stock_ticker, timesense, quick_poll,
 * note_tool — up to 12 incl. surface tools).  16 gives headroom and
 * keeps the cap memory bound to <= 16 * 224 bytes ≈ 3.5 KB in PSRAM. */
#define TOOLS_MAX 16
#define TOOL_NAME_LEN 32
#define TOOL_DESC_LEN 192

typedef struct {
   char name[TOOL_NAME_LEN];
   char description[TOOL_DESC_LEN];
} tool_card_t;

typedef struct {
   int n;
   tool_card_t tools[TOOLS_MAX];
   bool fetch_ok; /* false if HTTP error; true even when n==0 */
   char err_msg[80];
} tools_payload_t;

/* TT #328 Wave 12 — cross-session agent activity feed.
 *
 * Tab5's local `tool_log` ring only captures activity since the last
 * boot.  Dragon serves a wider, persistent ring (last 64 invocations)
 * via /api/v1/agent_log so the user can see "what has the agent been
 * doing" even after a Tab5 reboot.  Fetched on every overlay show;
 * rendered below the empty-state copy when local has nothing to show. */
/* AGENT_LOG_MAX caps the rendered list at 5 entries — each card is
 * roughly 70px tall (heading + preview line + divider), so 5 fits in
 * the ~400px gap between the activity entry container (y=420) and the
 * catalog section (y=820) without overlap. */
#define AGENT_LOG_MAX 5
#define AGENT_LOG_TOOL_LEN 32
#define AGENT_LOG_PREVIEW_LEN 96

typedef struct {
   uint32_t id;
   uint32_t ts;
   char tool[AGENT_LOG_TOOL_LEN];
   bool done;
   uint32_t execution_ms;
   char preview[AGENT_LOG_PREVIEW_LEN];
} agent_log_entry_t;

typedef struct {
   int n;
   agent_log_entry_t entries[AGENT_LOG_MAX];
   bool fetch_ok;
   char err_msg[80];
} agent_log_payload_t;

static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_back_btn      = NULL;
static lv_obj_t *s_count_lbl     = NULL;   /* U7 (#206): refreshed on each show */
static lv_obj_t *s_entry_root    = NULL;   /* container for the live activity entry */
static lv_obj_t *s_catalog_root = NULL;    /* TT #328 Wave 6: tools-catalog section */
static lv_obj_t *s_agent_log_root = NULL;  /* Wave 12: cross-session feed section */
static bool      s_visible       = false;
static volatile bool s_fetch_pending = false;
static volatile bool s_log_fetch_pending = false;

static void render_live_content(void);
static void render_catalog(const tools_payload_t *p);
static void fetch_tools_job(void *arg);
static void async_render_catalog_cb(void *arg);
static void kick_off_tools_fetch(void);
static void render_agent_log(const agent_log_payload_t *p);
static void fetch_agent_log_job(void *arg);
static void async_render_agent_log_cb(void *arg);
static void kick_off_agent_log_fetch(void);
/* W7-B Tab5-side (audit 2026-05-11): mode-3 agent-skill catalog
 * fetch.  Dragon serves `/api/v1/agent_skills` with the union of
 * known-static OpenClaw core tools + tools observed in agent_log.
 * First slice fires an obs event with the summary; rendering into
 * the overlay UI is a follow-up. */
static void fetch_agent_skills_job(void *arg);
static void kick_off_agent_skills_fetch(void);
static volatile bool s_skills_fetch_pending = false;

/* ── Helpers ─────────────────────────────────────────────────── */

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ui_agents_hide();
}

static void overlay_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
        ui_agents_hide();
    }
}

/* One agent "entry" — tight row block: label line, narrative, task list. */
static void build_agent_entry(lv_obj_t *parent, int y,
                              const char *label, const char *ts,
                              uint32_t dot_color, const char *narrative,
                              const char *tasks[], const int n_tasks)
{
    /* Container */
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_pos(c, SIDE_PAD, y);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1A1A24), 0);  /* hairline */
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    /* Head row: colored dot + label + timestamp */
    lv_obj_t *head = lv_obj_create(c);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(head);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t *name = lv_label_create(head);
    lv_label_set_text(name, label);
    lv_obj_set_style_text_font(name, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(name, 3, 0);

    lv_obj_t *sp = lv_obj_create(head);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_height(sp, 1);

    lv_obj_t *time = lv_label_create(head);
    lv_label_set_text(time, ts);
    lv_obj_set_style_text_font(time, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(time, lv_color_hex(0x55555D), 0);
    lv_obj_set_style_text_letter_space(time, 2, 0);

    /* Narrative — the headline line */
    lv_obj_t *line = lv_label_create(c);
    lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
    lv_label_set_text(line, narrative);
    lv_obj_set_width(line, SW - 2 * SIDE_PAD);
    lv_obj_set_style_text_font(line, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(line, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_line_space(line, 4, 0);

    /* Task stream — amber bullets for done, hairline for queued */
    for (int i = 0; i < n_tasks; i++) {
        lv_obj_t *row = lv_obj_create(c);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_set_style_pad_left(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *tdot = lv_obj_create(row);
        lv_obj_remove_style_all(tdot);
        lv_obj_set_size(tdot, 6, 6);
        lv_obj_set_style_radius(tdot, 3, 0);
        lv_obj_set_style_bg_color(tdot, lv_color_hex(i == 0 ? TH_STATUS_GREEN
                                                   : i == 1 ? TH_AMBER
                                                   : 0x2D2D35), 0);
        lv_obj_set_style_bg_opa(tdot, LV_OPA_COVER, 0);

        lv_obj_t *tlbl = lv_label_create(row);
        lv_label_set_text(tlbl, tasks[i]);
        lv_obj_set_style_text_font(tlbl, FONT_SMALL, 0);
        lv_obj_set_style_text_color(tlbl, lv_color_hex(TH_TEXT_BODY), 0);
    }
}

/* ── Wave 6: tools-catalog HTTP fetch + render ──────────────────── */

/* Worker job: GET /api/v1/tools, parse JSON, hand off to LVGL render
 * via tab5_lv_async_call.  Runs on the shared task_worker thread so
 * the LVGL UI thread doesn't block on the round-trip.  Output buffer
 * lives in PSRAM (cheap to allocate / free, doesn't fragment internal
 * SRAM). */
static void fetch_tools_job(void *arg) {
   (void)arg;
   tools_payload_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!p) {
      ESP_LOGE(TAG, "tools_payload alloc failed");
      s_fetch_pending = false;
      return;
   }

   char dragon_host[64] = {0};
   tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
   if (!dragon_host[0]) {
      snprintf(p->err_msg, sizeof(p->err_msg), "Dragon host not configured");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_catalog_cb, p);
      return;
   }

   char url[160];
   snprintf(url, sizeof(url), "http://%s:%d/api/v1/tools", dragon_host, TAB5_VOICE_PORT);

   /* PSRAM-backed response buffer.  Dragon's tools endpoint returns
    * ~2-5 KB JSON; 16 KB is plenty headroom for the registry growing
    * past current 10-12 tools. */
   const size_t resp_cap = 16 * 1024;
   char *resp_buf = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!resp_buf) {
      snprintf(p->err_msg, sizeof(p->err_msg), "PSRAM alloc failed");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_catalog_cb, p);
      return;
   }

   esp_http_client_config_t cfg = {
       .url = url,
       .method = HTTP_METHOD_GET,
       .timeout_ms = 5000,
       .buffer_size = 2048,
       .crt_bundle_attach = NULL,
   };
   esp_http_client_handle_t client = esp_http_client_init(&cfg);
   if (!client) {
      snprintf(p->err_msg, sizeof(p->err_msg), "http_client_init failed");
      p->fetch_ok = false;
      heap_caps_free(resp_buf);
      tab5_lv_async_call(async_render_catalog_cb, p);
      return;
   }

   /* TT #328 Wave 8 — Dragon REST endpoints (incl. /api/v1/tools)
    * are bearer-auth gated.  Send the token if the user has
    * provisioned one via Settings (`POST /settings` with
    * dragon_api_token); otherwise the request goes out without the
    * header and Dragon returns 401, which the fallback UI surfaces
    * gracefully. */
   char auth_header[128] = {0};
   {
      /* 96 bytes — 64-char token + null + headroom (matches the
       * /settings handler and the writer's 64-char cap with margin). */
      char tok[96] = {0};
      tab5_settings_get_dragon_api_token(tok, sizeof(tok));
      if (tok[0]) {
         snprintf(auth_header, sizeof(auth_header), "Bearer %s", tok);
         esp_http_client_set_header(client, "Authorization", auth_header);
      }
   }

   esp_err_t err = esp_http_client_open(client, 0);
   if (err != ESP_OK) {
      snprintf(p->err_msg, sizeof(p->err_msg), "open: %s", esp_err_to_name(err));
      p->fetch_ok = false;
      esp_http_client_cleanup(client);
      heap_caps_free(resp_buf);
      tab5_lv_async_call(async_render_catalog_cb, p);
      return;
   }
   int content_len = esp_http_client_fetch_headers(client);
   int status = esp_http_client_get_status_code(client);
   if (status != 200) {
      snprintf(p->err_msg, sizeof(p->err_msg), "HTTP %d", status);
      p->fetch_ok = false;
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      heap_caps_free(resp_buf);
      tab5_lv_async_call(async_render_catalog_cb, p);
      return;
   }

   int total = 0;
   while (total < (int)resp_cap - 1) {
      int n = esp_http_client_read(client, resp_buf + total, (int)resp_cap - 1 - total);
      if (n <= 0) break;
      total += n;
   }
   resp_buf[total] = '\0';
   (void)content_len;
   esp_http_client_close(client);
   esp_http_client_cleanup(client);

   /* Parse JSON shape:
    *   { "tools": [ {"name":"web_search","description":"..."} , ... ] }
    * The tools/ endpoint sometimes returns either {tools: [...]} or
    * a bare array — accept both. */
   cJSON *root = cJSON_Parse(resp_buf);
   heap_caps_free(resp_buf);
   if (!root) {
      snprintf(p->err_msg, sizeof(p->err_msg), "JSON parse failed");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_catalog_cb, p);
      return;
   }
   cJSON *arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "tools");
   if (!cJSON_IsArray(arr)) {
      snprintf(p->err_msg, sizeof(p->err_msg), "missing 'tools' array");
      p->fetch_ok = false;
      cJSON_Delete(root);
      tab5_lv_async_call(async_render_catalog_cb, p);
      return;
   }

   int count = cJSON_GetArraySize(arr);
   if (count > TOOLS_MAX) count = TOOLS_MAX;
   for (int i = 0; i < count; i++) {
      cJSON *t = cJSON_GetArrayItem(arr, i);
      cJSON *nm = cJSON_GetObjectItem(t, "name");
      cJSON *dsc = cJSON_GetObjectItem(t, "description");
      if (cJSON_IsString(nm)) {
         strncpy(p->tools[p->n].name, nm->valuestring, TOOL_NAME_LEN - 1);
      }
      if (cJSON_IsString(dsc)) {
         strncpy(p->tools[p->n].description, dsc->valuestring, TOOL_DESC_LEN - 1);
      }
      p->n++;
   }
   cJSON_Delete(root);
   p->fetch_ok = true;
   ESP_LOGI(TAG, "Fetched %d tools from %s", p->n, url);

   tab5_lv_async_call(async_render_catalog_cb, p);
}

/* LVGL-thread render: takes ownership of the heap-allocated payload,
 * builds the catalog cards, then frees it.  Idempotent — called on
 * every show via kick_off_tools_fetch. */
static void async_render_catalog_cb(void *arg) {
   tools_payload_t *p = (tools_payload_t *)arg;
   s_fetch_pending = false;
   if (p && s_overlay && s_visible) {
      render_catalog(p);
   }
   if (p) heap_caps_free(p);
}

static void kick_off_tools_fetch(void) {
   if (s_fetch_pending) return;
   s_fetch_pending = true;
   if (tab5_worker_enqueue(fetch_tools_job, NULL, "agents_tools_fetch") != ESP_OK) {
      ESP_LOGW(TAG, "tools_fetch worker enqueue failed");
      s_fetch_pending = false;
   }
}

/* ── Wave 12: cross-session agent_log fetch + render ─────────────── */

/* Worker job: GET /api/v1/agent_log, parse JSON, hand the most-recent
 * AGENT_LOG_MAX entries to LVGL via tab5_lv_async_call.  Same shape
 * as fetch_tools_job — bearer auth, PSRAM-backed response, cJSON parse. */
static void fetch_agent_log_job(void *arg) {
   (void)arg;
   agent_log_payload_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!p) {
      ESP_LOGE(TAG, "agent_log_payload alloc failed");
      s_log_fetch_pending = false;
      return;
   }

   char dragon_host[64] = {0};
   tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
   if (!dragon_host[0]) {
      snprintf(p->err_msg, sizeof(p->err_msg), "Dragon host not configured");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_agent_log_cb, p);
      return;
   }

   char url[160];
   snprintf(url, sizeof(url), "http://%s:%d/api/v1/agent_log?limit=%d", dragon_host, TAB5_VOICE_PORT, AGENT_LOG_MAX);

   const size_t resp_cap = 16 * 1024;
   char *resp_buf = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!resp_buf) {
      snprintf(p->err_msg, sizeof(p->err_msg), "PSRAM alloc failed");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_agent_log_cb, p);
      return;
   }

   esp_http_client_config_t cfg = {
       .url = url,
       .method = HTTP_METHOD_GET,
       .timeout_ms = 5000,
       .buffer_size = 2048,
       .crt_bundle_attach = NULL,
   };
   esp_http_client_handle_t client = esp_http_client_init(&cfg);
   if (!client) {
      snprintf(p->err_msg, sizeof(p->err_msg), "http_client_init failed");
      p->fetch_ok = false;
      heap_caps_free(resp_buf);
      tab5_lv_async_call(async_render_agent_log_cb, p);
      return;
   }

   /* Same bearer-auth path as Wave 6 — empty token = unauth request,
    * Dragon returns 401, fallback UI surfaces the message. */
   char auth_header[128] = {0};
   {
      char tok[96] = {0};
      tab5_settings_get_dragon_api_token(tok, sizeof(tok));
      if (tok[0]) {
         snprintf(auth_header, sizeof(auth_header), "Bearer %s", tok);
         esp_http_client_set_header(client, "Authorization", auth_header);
      }
   }

   esp_err_t err = esp_http_client_open(client, 0);
   if (err != ESP_OK) {
      snprintf(p->err_msg, sizeof(p->err_msg), "open: %s", esp_err_to_name(err));
      p->fetch_ok = false;
      esp_http_client_cleanup(client);
      heap_caps_free(resp_buf);
      tab5_lv_async_call(async_render_agent_log_cb, p);
      return;
   }
   esp_http_client_fetch_headers(client);
   int status = esp_http_client_get_status_code(client);
   if (status != 200) {
      snprintf(p->err_msg, sizeof(p->err_msg), "HTTP %d", status);
      p->fetch_ok = false;
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      heap_caps_free(resp_buf);
      tab5_lv_async_call(async_render_agent_log_cb, p);
      return;
   }

   int total = 0;
   while (total < (int)resp_cap - 1) {
      int n = esp_http_client_read(client, resp_buf + total, (int)resp_cap - 1 - total);
      if (n <= 0) break;
      total += n;
   }
   resp_buf[total] = '\0';
   esp_http_client_close(client);
   esp_http_client_cleanup(client);

   /* { "items": [{id, ts, tool, status, result, execution_ms, args}, ...],
    *   "count":N, "head_id":H, "tail_id":T, "ring_size":S }                */
   cJSON *root = cJSON_Parse(resp_buf);
   heap_caps_free(resp_buf);
   if (!root) {
      snprintf(p->err_msg, sizeof(p->err_msg), "JSON parse failed");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_agent_log_cb, p);
      return;
   }
   cJSON *arr = cJSON_GetObjectItem(root, "items");
   if (!cJSON_IsArray(arr)) {
      snprintf(p->err_msg, sizeof(p->err_msg), "missing 'items' array");
      p->fetch_ok = false;
      cJSON_Delete(root);
      tab5_lv_async_call(async_render_agent_log_cb, p);
      return;
   }

   int count = cJSON_GetArraySize(arr);
   if (count > AGENT_LOG_MAX) count = AGENT_LOG_MAX;
   for (int i = 0; i < count; i++) {
      cJSON *e = cJSON_GetArrayItem(arr, i);
      cJSON *id = cJSON_GetObjectItem(e, "id");
      cJSON *ts = cJSON_GetObjectItem(e, "ts");
      cJSON *tool = cJSON_GetObjectItem(e, "tool");
      cJSON *status_j = cJSON_GetObjectItem(e, "status");
      cJSON *result = cJSON_GetObjectItem(e, "result");
      cJSON *exec_ms = cJSON_GetObjectItem(e, "execution_ms");
      agent_log_entry_t *out = &p->entries[p->n];
      if (cJSON_IsNumber(id)) out->id = (uint32_t)id->valuedouble;
      if (cJSON_IsNumber(ts)) out->ts = (uint32_t)ts->valuedouble;
      if (cJSON_IsString(tool)) {
         strncpy(out->tool, tool->valuestring, AGENT_LOG_TOOL_LEN - 1);
      }
      out->done = cJSON_IsString(status_j) && strcmp(status_j->valuestring, "done") == 0;
      if (cJSON_IsNumber(exec_ms)) out->execution_ms = (uint32_t)exec_ms->valuedouble;
      if (cJSON_IsString(result)) {
         strncpy(out->preview, result->valuestring, AGENT_LOG_PREVIEW_LEN - 1);
      }
      p->n++;
   }
   cJSON_Delete(root);
   p->fetch_ok = true;
   ESP_LOGI(TAG, "Fetched %d agent_log entries from %s", p->n, url);

   tab5_lv_async_call(async_render_agent_log_cb, p);
}

static void async_render_agent_log_cb(void *arg) {
   agent_log_payload_t *p = (agent_log_payload_t *)arg;
   s_log_fetch_pending = false;
   if (p && s_overlay && s_visible) {
      render_agent_log(p);
   }
   if (p) heap_caps_free(p);
}

static void kick_off_agent_log_fetch(void) {
   if (s_log_fetch_pending) return;
   s_log_fetch_pending = true;
   if (tab5_worker_enqueue(fetch_agent_log_job, NULL, "agents_log_fetch") != ESP_OK) {
      ESP_LOGW(TAG, "agent_log_fetch worker enqueue failed");
      s_log_fetch_pending = false;
   }
}

/* ── W7-B Tab5-side: GET /api/v1/agent_skills round-trip probe ────────
 *
 * First slice — fetch the catalog on every overlay show, emit an obs
 * event with `count=N observed=M`, log a one-line summary.  No UI
 * rendering yet; that's a follow-up once we've validated the data
 * shape on live hardware via the obs ring.  Catalog is at
 * `dragon_host:3502/api/v1/agent_skills`. */
static void fetch_agent_skills_job(void *arg) {
   (void)arg;

   char dragon_host[64] = {0};
   tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
   if (!dragon_host[0]) {
      tab5_debug_obs_event("agent_skills", "no_host");
      s_skills_fetch_pending = false;
      return;
   }

   char url[160];
   snprintf(url, sizeof(url), "http://%s:%d/api/v1/agent_skills", dragon_host, TAB5_VOICE_PORT);

   const size_t resp_cap = 8 * 1024;
   char *resp_buf = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!resp_buf) {
      tab5_debug_obs_event("agent_skills", "psram_alloc_fail");
      s_skills_fetch_pending = false;
      return;
   }

   esp_http_client_config_t cfg = {
       .url = url,
       .method = HTTP_METHOD_GET,
       .timeout_ms = 5000,
       .buffer_size = 2048,
       .crt_bundle_attach = NULL,
   };
   esp_http_client_handle_t client = esp_http_client_init(&cfg);
   if (!client) {
      tab5_debug_obs_event("agent_skills", "http_init_fail");
      heap_caps_free(resp_buf);
      s_skills_fetch_pending = false;
      return;
   }

   /* Same bearer-auth path as the tools + agent_log fetches. */
   char auth_header[128] = {0};
   {
      char tok[96] = {0};
      tab5_settings_get_dragon_api_token(tok, sizeof(tok));
      if (tok[0]) {
         snprintf(auth_header, sizeof(auth_header), "Bearer %s", tok);
         esp_http_client_set_header(client, "Authorization", auth_header);
      }
   }

   esp_err_t err = esp_http_client_open(client, 0);
   if (err != ESP_OK) {
      char detail[48];
      snprintf(detail, sizeof detail, "open_err=%s", esp_err_to_name(err));
      tab5_debug_obs_event("agent_skills", detail);
      esp_http_client_cleanup(client);
      heap_caps_free(resp_buf);
      s_skills_fetch_pending = false;
      return;
   }
   esp_http_client_fetch_headers(client);
   int status = esp_http_client_get_status_code(client);
   if (status != 200) {
      char detail[24];
      snprintf(detail, sizeof detail, "http_%d", status);
      tab5_debug_obs_event("agent_skills", detail);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      heap_caps_free(resp_buf);
      s_skills_fetch_pending = false;
      return;
   }

   int total = 0;
   while (total < (int)resp_cap - 1) {
      int n = esp_http_client_read(client, resp_buf + total, (int)resp_cap - 1 - total);
      if (n <= 0) break;
      total += n;
   }
   resp_buf[total] = '\0';
   esp_http_client_close(client);
   esp_http_client_cleanup(client);

   /* Response shape:
    *   {"items":[...], "count":N, "static_count":S, "observed_count":O}
    * We only care about the summary counts for this slice. */
   cJSON *root = cJSON_Parse(resp_buf);
   heap_caps_free(resp_buf);
   if (!root) {
      tab5_debug_obs_event("agent_skills", "json_parse_fail");
      s_skills_fetch_pending = false;
      return;
   }
   int count = 0, observed = 0;
   cJSON *c = cJSON_GetObjectItem(root, "count");
   cJSON *o = cJSON_GetObjectItem(root, "observed_count");
   if (cJSON_IsNumber(c)) count = c->valueint;
   if (cJSON_IsNumber(o)) observed = o->valueint;
   cJSON_Delete(root);

   char detail[48];
   snprintf(detail, sizeof detail, "count=%d observed=%d", count, observed);
   tab5_debug_obs_event("agent_skills", detail);
   ESP_LOGI(TAG, "agent_skills fetched: %s", detail);
   s_skills_fetch_pending = false;
}

static void kick_off_agent_skills_fetch(void) {
   if (s_skills_fetch_pending) return;
   s_skills_fetch_pending = true;
   if (tab5_worker_enqueue(fetch_agent_skills_job, NULL, "agents_skills_fetch") != ESP_OK) {
      ESP_LOGW(TAG, "agent_skills_fetch worker enqueue failed");
      s_skills_fetch_pending = false;
   }
}

static void render_catalog(const tools_payload_t *p) {
   /* Lazy-create the catalog container.  Sits below the existing
    * AGENT ACTIVITY entry at y ≈ 250 + (entry height ~250).  Use
    * y=540 as a stable starting point; the entry above is dynamic
    * but capped low. */
   if (!s_catalog_root) {
      s_catalog_root = lv_obj_create(s_overlay);
      lv_obj_remove_style_all(s_catalog_root);
      lv_obj_set_size(s_catalog_root, SW, LV_SIZE_CONTENT);
      /* Wave 12 — push catalog further down to leave room for the
       * cross-session agent_log section at y=420.  Overlay scrolls
       * vertically so going past 1280 is safe. */
      lv_obj_set_pos(s_catalog_root, 0, 820);
      lv_obj_set_flex_flow(s_catalog_root, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(s_catalog_root, 14, 0);
      lv_obj_clear_flag(s_catalog_root, LV_OBJ_FLAG_SCROLLABLE);
   } else {
      lv_obj_clean(s_catalog_root);
   }

   /* Section header */
   lv_obj_t *kicker = lv_label_create(s_catalog_root);
   lv_label_set_text(kicker, "\xe2\x80\xa2 TOOLS CATALOG");
   lv_obj_set_style_text_font(kicker, FONT_CAPTION, 0);
   lv_obj_set_style_text_color(kicker, lv_color_hex(TH_AMBER), 0);
   lv_obj_set_style_text_letter_space(kicker, 4, 0);
   lv_obj_set_style_pad_left(kicker, SIDE_PAD, 0);

   if (!p->fetch_ok) {
      lv_obj_t *e = lv_label_create(s_catalog_root);
      lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
      char buf[256];
      snprintf(buf, sizeof(buf),
               "Couldn't reach Dragon's tool catalog (%s).\n\n"
               "Once Dragon is online, reopen this screen to see "
               "what tools the agent can call.",
               p->err_msg[0] ? p->err_msg : "no detail");
      lv_label_set_text(e, buf);
      lv_obj_set_width(e, SW - 2 * SIDE_PAD);
      lv_obj_set_style_pad_left(e, SIDE_PAD, 0);
      lv_obj_set_style_text_font(e, FONT_BODY, 0);
      lv_obj_set_style_text_color(e, lv_color_hex(TH_TEXT_DIM), 0);
      return;
   }

   if (p->n == 0) {
      lv_obj_t *e = lv_label_create(s_catalog_root);
      lv_label_set_text(e, "No tools registered on this Dragon yet.");
      lv_obj_set_style_pad_left(e, SIDE_PAD, 0);
      lv_obj_set_style_text_font(e, FONT_BODY, 0);
      lv_obj_set_style_text_color(e, lv_color_hex(TH_TEXT_DIM), 0);
      return;
   }

   /* Count line */
   lv_obj_t *count = lv_label_create(s_catalog_root);
   char cbuf[40];
   snprintf(cbuf, sizeof(cbuf), "%d TOOLS AVAILABLE", p->n);
   lv_label_set_text(count, cbuf);
   lv_obj_set_style_text_font(count, FONT_CAPTION, 0);
   lv_obj_set_style_text_color(count, lv_color_hex(TH_TEXT_SECONDARY), 0);
   lv_obj_set_style_text_letter_space(count, 3, 0);
   lv_obj_set_style_pad_left(count, SIDE_PAD, 0);

   /* Each tool: a typographic block — name in heading, description
    * wrapped underneath, divider hairline below. */
   for (int i = 0; i < p->n; i++) {
      lv_obj_t *card = lv_obj_create(s_catalog_root);
      lv_obj_remove_style_all(card);
      lv_obj_set_size(card, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
      lv_obj_set_style_margin_left(card, SIDE_PAD, 0);
      lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(card, 4, 0);
      lv_obj_set_style_pad_bottom(card, 12, 0);
      lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_color(card, lv_color_hex(0x1A1A24), 0);
      lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t *nm = lv_label_create(card);
      lv_label_set_text(nm, p->tools[i].name);
      lv_obj_set_style_text_font(nm, FONT_HEADING, 0);
      lv_obj_set_style_text_color(nm, lv_color_hex(TH_TEXT_PRIMARY), 0);

      if (p->tools[i].description[0]) {
         lv_obj_t *ds = lv_label_create(card);
         lv_label_set_long_mode(ds, LV_LABEL_LONG_WRAP);
         lv_label_set_text(ds, p->tools[i].description);
         lv_obj_set_width(ds, SW - 2 * SIDE_PAD);
         lv_obj_set_style_text_font(ds, FONT_SMALL, 0);
         lv_obj_set_style_text_color(ds, lv_color_hex(TH_TEXT_BODY), 0);
         lv_obj_set_style_text_line_space(ds, 3, 0);
      }
   }
}

/* TT #328 Wave 12 — render the cross-session agent_log feed.
 *
 * Sits between the local-activity entry (y≈250) and the tools catalog
 * (y=540 → pushed to y=820 when this section renders).  Hidden when
 * the local tool_log already has entries — under that condition, the
 * live entry above is the freshest activity surface and a duplicate
 * historical feed would only add noise.  Shown in two cases:
 *   • local empty + Dragon ring populated → "Recent agent activity"
 *     section listing the last N invocations
 *   • local empty + Dragon unreachable    → fallback hint sourced from
 *     err_msg (typically "HTTP 401" if no token, "open: …" if offline) */
static void render_agent_log(const agent_log_payload_t *p) {
   /* Skip rendering if the local activity log already has live data —
    * the existing entry above carries the freshest signal. */
   bool local_empty = (tool_log_count() == 0);

   if (!s_agent_log_root) {
      s_agent_log_root = lv_obj_create(s_overlay);
      lv_obj_remove_style_all(s_agent_log_root);
      lv_obj_set_size(s_agent_log_root, SW, LV_SIZE_CONTENT);
      lv_obj_set_pos(s_agent_log_root, 0, 420);
      lv_obj_set_flex_flow(s_agent_log_root, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(s_agent_log_root, 8, 0);
      lv_obj_clear_flag(s_agent_log_root, LV_OBJ_FLAG_SCROLLABLE);
   } else {
      lv_obj_clean(s_agent_log_root);
   }

   /* When local has live entries, hide this section entirely.  We
    * still create + clean the container so future re-renders (after
    * a New Chat clears tool_log) don't end up with stale children. */
   if (!local_empty) {
      lv_obj_add_flag(s_agent_log_root, LV_OBJ_FLAG_HIDDEN);
      return;
   }
   lv_obj_remove_flag(s_agent_log_root, LV_OBJ_FLAG_HIDDEN);

   lv_obj_t *kicker = lv_label_create(s_agent_log_root);
   lv_label_set_text(kicker, "\xe2\x80\xa2 RECENT AGENT ACTIVITY (DRAGON)");
   lv_obj_set_style_text_font(kicker, FONT_CAPTION, 0);
   lv_obj_set_style_text_color(kicker, lv_color_hex(TH_AMBER), 0);
   lv_obj_set_style_text_letter_space(kicker, 4, 0);
   lv_obj_set_style_pad_left(kicker, SIDE_PAD, 0);

   if (!p->fetch_ok) {
      lv_obj_t *e = lv_label_create(s_agent_log_root);
      lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
      char buf[256];
      snprintf(buf, sizeof(buf),
               "Couldn't fetch Dragon's recent activity (%s).\n\n"
               "Configure the Dragon API token via Settings to see "
               "cross-session tool history.",
               p->err_msg[0] ? p->err_msg : "no detail");
      lv_label_set_text(e, buf);
      lv_obj_set_width(e, SW - 2 * SIDE_PAD);
      lv_obj_set_style_pad_left(e, SIDE_PAD, 0);
      lv_obj_set_style_text_font(e, FONT_BODY, 0);
      lv_obj_set_style_text_color(e, lv_color_hex(TH_TEXT_DIM), 0);
      return;
   }

   if (p->n == 0) {
      lv_obj_t *e = lv_label_create(s_agent_log_root);
      lv_label_set_text(e, "No tool activity recorded on Dragon yet.");
      lv_obj_set_style_pad_left(e, SIDE_PAD, 0);
      lv_obj_set_style_text_font(e, FONT_BODY, 0);
      lv_obj_set_style_text_color(e, lv_color_hex(TH_TEXT_DIM), 0);
      return;
   }

   /* Compact card per entry: status dot + tool name (heading) + result
    * preview (small body).  Same hairline-divider treatment as the
    * tools catalog so the two sections feel like a single design. */
   for (int i = 0; i < p->n; i++) {
      const agent_log_entry_t *en = &p->entries[i];
      lv_obj_t *card = lv_obj_create(s_agent_log_root);
      lv_obj_remove_style_all(card);
      lv_obj_set_size(card, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
      lv_obj_set_style_margin_left(card, SIDE_PAD, 0);
      lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(card, 4, 0);
      lv_obj_set_style_pad_bottom(card, 10, 0);
      lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_color(card, lv_color_hex(0x1A1A24), 0);
      lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

      /* Header row: status dot + tool name + execution timing. */
      lv_obj_t *head = lv_obj_create(card);
      lv_obj_remove_style_all(head);
      lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(head, 8, 0);
      lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t *dot = lv_obj_create(head);
      lv_obj_remove_style_all(dot);
      lv_obj_set_size(dot, 6, 6);
      lv_obj_set_style_radius(dot, 3, 0);
      lv_obj_set_style_bg_color(dot, lv_color_hex(en->done ? TH_STATUS_GREEN : TH_AMBER), 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

      lv_obj_t *nm = lv_label_create(head);
      lv_label_set_text(nm, en->tool[0] ? en->tool : "(unknown)");
      lv_obj_set_style_text_font(nm, FONT_HEADING, 0);
      lv_obj_set_style_text_color(nm, lv_color_hex(TH_TEXT_PRIMARY), 0);

      lv_obj_t *sp = lv_obj_create(head);
      lv_obj_remove_style_all(sp);
      lv_obj_set_flex_grow(sp, 1);
      lv_obj_set_height(sp, 1);
      lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE);

      char tbuf[24] = {0};
      if (en->done) {
         snprintf(tbuf, sizeof(tbuf), "%lums", (unsigned long)en->execution_ms);
      } else {
         snprintf(tbuf, sizeof(tbuf), "RUNNING");
      }
      lv_obj_t *tlbl = lv_label_create(head);
      lv_label_set_text(tlbl, tbuf);
      lv_obj_set_style_text_font(tlbl, FONT_CAPTION, 0);
      lv_obj_set_style_text_color(tlbl, lv_color_hex(TH_TEXT_DIM), 0);
      lv_obj_set_style_text_letter_space(tlbl, 2, 0);

      if (en->preview[0]) {
         lv_obj_t *prev = lv_label_create(card);
         lv_label_set_long_mode(prev, LV_LABEL_LONG_WRAP);
         lv_label_set_text(prev, en->preview);
         lv_obj_set_width(prev, SW - 2 * SIDE_PAD);
         lv_obj_set_style_text_font(prev, FONT_SMALL, 0);
         lv_obj_set_style_text_color(prev, lv_color_hex(TH_TEXT_BODY), 0);
         lv_obj_set_style_text_line_space(prev, 3, 0);
      }
   }
}

/* ── Build + show ────────────────────────────────────────────── */

void ui_agents_show(void)
{
    /* Match the go-home fix: if a secondary lv_screen (camera/files) is
     * currently active, load home first so our overlay renders. */
    lv_obj_t *home = ui_home_get_screen();
    if (home && lv_screen_active() != home) {
        lv_screen_load(home);
    }

    /* Re-use hidden overlay (hide/show pattern). */
    if (s_overlay) {
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_visible = true;
        /* U7 (#206): live content was built from a snapshot of
         * tool_log on first show — refresh on every re-show so new
         * tool activity appears. */
        render_live_content();
        /* TT #328 Wave 6 — also re-fetch the tool catalog so new
         * tool registrations on Dragon (skill installs, plugin loads)
         * surface without a Tab5 reboot. */
        kick_off_tools_fetch();
        /* TT #328 Wave 12 — refresh cross-session activity feed. */
        kick_off_agent_log_fetch();
        /* W7-B (audit 2026-05-11): probe Dragon's agent-skill catalog
         * for mode-3 visibility — obs ring records count + observed. */
        kick_off_agent_skills_fetch();
        return;
    }

    /* Fullscreen overlay — parent is home screen, same pattern as chat/settings */
    lv_obj_t *parent = home;
    if (!parent) parent = lv_screen_active();
    if (!parent) return;

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, SW, SH);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_overlay, LV_DIR_VER);
    lv_obj_add_event_cb(s_overlay, overlay_gesture_cb, LV_EVENT_GESTURE, NULL);
    /* Stop gesture bubble so home's screen_gesture_cb doesn't reopen an
     * overlay we just closed via swipe. */
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Back hit — top-left (taps) */
    s_back_btn = lv_button_create(s_overlay);
    lv_obj_set_size(s_back_btn, 120, 60);
    lv_obj_set_pos(s_back_btn, 24, 30);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    lv_obj_set_style_border_width(s_back_btn, 0, 0);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(s_back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  HOME");
    lv_obj_set_style_text_font(bl, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(bl, 3, 0);
    lv_obj_center(bl);

    /* Header — "Agents" + count */
    lv_obj_t *head = lv_label_create(s_overlay);
    lv_label_set_text(head, "Agents");
    lv_obj_set_style_text_font(head, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_pos(head, SIDE_PAD, 110);

    render_live_content();
    s_visible = true;
    /* TT #328 Wave 6 — kick off the tool-catalog fetch on first show.
     * The HTTP round-trip happens on the worker thread; the LVGL paint
     * lands a moment later via async_render_catalog_cb. */
    kick_off_tools_fetch();
    /* TT #328 Wave 12 — fetch cross-session agent_log in parallel.
     * Renders below the local activity entry when local is empty. */
    kick_off_agent_log_fetch();
    /* W7-B (audit 2026-05-11): probe Dragon's agent-skill catalog
     * for mode-3 visibility — first slice records `count` + `observed`
     * to the obs ring; visual rendering follows once data shape is
     * validated on live hardware. */
    kick_off_agent_skills_fetch();
    ESP_LOGI(TAG, "agents overlay shown");
}

/* U7+U8 (#206): rebuild the count label + entry container from the
 * current tool_log ring snapshot.  Idempotent — called on every show
 * (first or re-show).  Hide/show pattern is preserved (we don't
 * destroy s_overlay), so the LV pool isn't churned. */
static void render_live_content(void)
{
    if (!s_overlay) return;

    int total = tool_log_count();
    int running = 0, done = 0;
    for (int i = 0; i < total; i++) {
        tool_log_event_t e;
        if (tool_log_get(i, &e)) {
            if (e.status == TOOL_LOG_RUNNING) running++;
            else                              done++;
        }
    }

    /* Count label — create-once, update text in place on re-renders. */
    if (!s_count_lbl) {
        s_count_lbl = lv_label_create(s_overlay);
        lv_obj_set_style_text_font(s_count_lbl, FONT_CAPTION, 0);
        lv_obj_set_style_text_color(s_count_lbl,
            lv_color_hex(TH_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_letter_space(s_count_lbl, 3, 0);
        lv_obj_set_pos(s_count_lbl, SIDE_PAD, 190);
    }
    char count_buf[40];
    snprintf(count_buf, sizeof(count_buf), "%d LIVE  \xe2\x80\xa2  %d DONE",
             running, done);
    lv_label_set_text(s_count_lbl, count_buf);

    /* Entry container — wipe + rebuild children on each render.  The
     * container itself persists across re-shows so the LV pool isn't
     * thrashed; only the cheap label/dot widgets are recycled. */
    if (!s_entry_root) {
        s_entry_root = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(s_entry_root);
        lv_obj_set_size(s_entry_root, SW, LV_SIZE_CONTENT);
        lv_obj_set_pos(s_entry_root, 0, 250);
        lv_obj_clear_flag(s_entry_root, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clean(s_entry_root);
    }

    if (total == 0) {
        lv_obj_t *empty = lv_label_create(s_entry_root);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_label_set_text(empty,
            "No tool activity yet.\n\n"
            "Talk to TinkerClaw -- when it searches the web, recalls a "
            "memory, or runs any other tool, it'll show up here.");
        lv_obj_set_width(empty, SW - 2 * SIDE_PAD);
        lv_obj_set_style_text_font(empty, FONT_BODY, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(TH_TEXT_DIM), 0);
        lv_obj_set_style_text_line_space(empty, 4, 0);
        lv_obj_set_pos(empty, SIDE_PAD, 0);
        return;
    }

    int n_show = total > 6 ? 6 : total;
    const char *task_lines[6] = {0};
    static char  task_storage[6][96];

    tool_log_event_t newest;
    tool_log_get(0, &newest);

    char narrative[160];
    if (running > 0) {
        snprintf(narrative, sizeof(narrative),
            "Running %s now. %d completed in this session.",
            newest.detail[0] ? newest.detail : newest.name, done);
    } else {
        snprintf(narrative, sizeof(narrative),
            "Last action: %s. %d completed in this session.",
            newest.detail[0] ? newest.detail : newest.name, done);
    }

    for (int i = 0; i < n_show; i++) {
        tool_log_event_t e;
        if (!tool_log_get(i, &e)) continue;
        if (e.status == TOOL_LOG_DONE) {
            snprintf(task_storage[i], sizeof(task_storage[0]),
                     "%s  \xe2\x80\xa2  done  \xe2\x80\xa2  %u ms",
                     e.name, (unsigned)e.exec_ms);
        } else {
            snprintf(task_storage[i], sizeof(task_storage[0]),
                     "%s  \xe2\x80\xa2  running",
                     e.name);
        }
        task_lines[i] = task_storage[i];
    }

    char ts_label[24] = "JUST NOW";
    time_t now = time(NULL);
    long secs = (long)(now - newest.started_at);
    if      (secs < 60)    snprintf(ts_label, sizeof(ts_label), "JUST NOW");
    else if (secs < 3600)  snprintf(ts_label, sizeof(ts_label), "%ld MIN AGO", secs / 60);
    else if (secs < 86400) snprintf(ts_label, sizeof(ts_label), "%ld H AGO",   secs / 3600);
    else                   snprintf(ts_label, sizeof(ts_label), "%ld D AGO",   secs / 86400);

    build_agent_entry(s_entry_root, 0,
                      "AGENT ACTIVITY", ts_label,
                      running > 0 ? TH_MODE_CLAW : TH_STATUS_GREEN,
                      narrative, task_lines, n_show);
}

void ui_agents_hide(void)
{
    if (!s_visible) return;
    /* Hide instead of destroy (see ui_focus_hide). */
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ESP_LOGI(TAG, "agents overlay hidden");
}

bool ui_agents_is_visible(void)
{
    return s_visible;
}
