/*
 * ui_skills.c — TT #328 Wave 10 dedicated catalog viewer.
 *
 * See ui_skills.h for the contract.  Streamlined cousin of
 * ui_agents.c — pure tools catalog, no activity feed mixed in.
 * Larger, more readable cards.  Accessed from the nav-sheet
 * "Skills" tile (replacing the underused "Focus" tile in the
 * 3×3 grid).
 *
 * HTTP fetch + JSON parse path is byte-similar to ui_agents.c's
 * Wave 6+8 catalog fetch — kept inline rather than extracted to
 * a shared helper because (a) only two callers, (b) the two
 * surfaces want slightly different render styles, (c) extraction
 * would force both files to track helper-API churn.  If a third
 * surface needs the catalog, the helper extraction becomes worth
 * the maintenance cost; until then, keep it copy-paste-clear.
 */
#include "ui_skills.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings.h"
#include "task_worker.h"
#include "ui_core.h"
#include "ui_home.h"
#include "ui_theme.h"

static const char *TAG = "ui_skills";

#define SW 720
#define SH 1280
#define SIDE_PAD 52

/* Wave 10 — same registry cap as Wave 6's ui_agents catalog so the
 * two surfaces never disagree on what fits.  Memory bound: 16 *
 * (32 + 192) ≈ 3.5 KB in PSRAM per show. */
#define TOOLS_MAX 16
#define TOOL_NAME_LEN 32
#define TOOL_DESC_LEN 192

typedef struct {
   char name[TOOL_NAME_LEN];
   char description[TOOL_DESC_LEN];
} skill_card_t;

typedef struct {
   int n;
   skill_card_t tools[TOOLS_MAX];
   bool fetch_ok;
   char err_msg[80];
} skills_payload_t;

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_back_btn = NULL;
static lv_obj_t *s_count_lbl = NULL;
static lv_obj_t *s_list_root = NULL;
static bool s_visible = false;
static volatile bool s_fetch_pending = false;

static void render_payload(const skills_payload_t *p);
static void fetch_skills_job(void *arg);
static void async_render_cb(void *arg);

/* ── Helpers ─────────────────────────────────────────────────── */

static void back_click_cb(lv_event_t *e) {
   (void)e;
   ui_skills_hide();
}

static void overlay_gesture_cb(lv_event_t *e) {
   lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
   if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
      ui_skills_hide();
   }
}

/* ── HTTP fetch ──────────────────────────────────────────────── */

static void fetch_skills_job(void *arg) {
   (void)arg;
   skills_payload_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!p) {
      ESP_LOGE(TAG, "skills_payload alloc failed");
      s_fetch_pending = false;
      return;
   }

   char dragon_host[64] = {0};
   tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
   if (!dragon_host[0]) {
      snprintf(p->err_msg, sizeof(p->err_msg), "Dragon host not configured");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_cb, p);
      return;
   }

   char url[160];
   snprintf(url, sizeof(url), "http://%s:%d/api/v1/tools", dragon_host, TAB5_VOICE_PORT);

   const size_t resp_cap = 16 * 1024;
   char *resp_buf = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!resp_buf) {
      snprintf(p->err_msg, sizeof(p->err_msg), "PSRAM alloc failed");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_cb, p);
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
      tab5_lv_async_call(async_render_cb, p);
      return;
   }

   /* Wave 8 — bearer auth.  Empty token sends no header → Dragon
    * 401 → fallback UI surfaces gracefully. */
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
      tab5_lv_async_call(async_render_cb, p);
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
      tab5_lv_async_call(async_render_cb, p);
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

   cJSON *root = cJSON_Parse(resp_buf);
   heap_caps_free(resp_buf);
   if (!root) {
      snprintf(p->err_msg, sizeof(p->err_msg), "JSON parse failed");
      p->fetch_ok = false;
      tab5_lv_async_call(async_render_cb, p);
      return;
   }
   cJSON *arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "tools");
   if (!cJSON_IsArray(arr)) {
      snprintf(p->err_msg, sizeof(p->err_msg), "missing 'tools' array");
      p->fetch_ok = false;
      cJSON_Delete(root);
      tab5_lv_async_call(async_render_cb, p);
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
   ESP_LOGI(TAG, "Fetched %d skills from %s", p->n, url);

   tab5_lv_async_call(async_render_cb, p);
}

static void async_render_cb(void *arg) {
   skills_payload_t *p = (skills_payload_t *)arg;
   s_fetch_pending = false;
   if (p && s_overlay && s_visible) {
      render_payload(p);
   }
   if (p) heap_caps_free(p);
}

static void kick_off_fetch(void) {
   if (s_fetch_pending) return;
   s_fetch_pending = true;
   if (tab5_worker_enqueue(fetch_skills_job, NULL, "skills_fetch") != ESP_OK) {
      ESP_LOGW(TAG, "skills_fetch worker enqueue failed");
      s_fetch_pending = false;
   }
}

static void render_payload(const skills_payload_t *p) {
   /* Lazy-create the list container.  Persists across re-shows. */
   if (!s_list_root) {
      s_list_root = lv_obj_create(s_overlay);
      lv_obj_remove_style_all(s_list_root);
      lv_obj_set_size(s_list_root, SW, LV_SIZE_CONTENT);
      lv_obj_set_pos(s_list_root, 0, 280);
      lv_obj_set_flex_flow(s_list_root, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(s_list_root, 18, 0);
      lv_obj_clear_flag(s_list_root, LV_OBJ_FLAG_SCROLLABLE);
   } else {
      lv_obj_clean(s_list_root);
   }

   if (!p->fetch_ok) {
      lv_obj_t *e = lv_label_create(s_list_root);
      lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
      char buf[256];
      snprintf(buf, sizeof(buf),
               "Couldn't reach Dragon's tool registry (%s).\n\n"
               "Set the Dragon API token under POST /settings "
               "(dragon_api_token) and re-open this screen.",
               p->err_msg[0] ? p->err_msg : "no detail");
      lv_label_set_text(e, buf);
      lv_obj_set_width(e, SW - 2 * SIDE_PAD);
      lv_obj_set_style_pad_left(e, SIDE_PAD, 0);
      lv_obj_set_style_text_font(e, FONT_BODY, 0);
      lv_obj_set_style_text_color(e, lv_color_hex(TH_TEXT_DIM), 0);
      lv_obj_set_style_text_line_space(e, 4, 0);
      if (s_count_lbl) lv_label_set_text(s_count_lbl, "OFFLINE");
      return;
   }

   if (p->n == 0) {
      lv_obj_t *e = lv_label_create(s_list_root);
      lv_label_set_text(e, "No tools registered on this Dragon yet.");
      lv_obj_set_style_pad_left(e, SIDE_PAD, 0);
      lv_obj_set_style_text_font(e, FONT_BODY, 0);
      lv_obj_set_style_text_color(e, lv_color_hex(TH_TEXT_DIM), 0);
      if (s_count_lbl) lv_label_set_text(s_count_lbl, "0 SKILLS");
      return;
   }

   if (s_count_lbl) {
      char cbuf[40];
      snprintf(cbuf, sizeof(cbuf), "%d SKILL%s AVAILABLE", p->n, p->n == 1 ? "" : "S");
      lv_label_set_text(s_count_lbl, cbuf);
   }

   /* One typographic card per tool — name in title font, description
    * wrapped in body font, hairline divider below.  Bigger / more
    * readable than ui_agents's catalog cards (those share row-space
    * with the activity feed; here we have the whole screen). */
   for (int i = 0; i < p->n; i++) {
      lv_obj_t *card = lv_obj_create(s_list_root);
      lv_obj_remove_style_all(card);
      lv_obj_set_size(card, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
      lv_obj_set_style_margin_left(card, SIDE_PAD, 0);
      lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(card, 6, 0);
      lv_obj_set_style_pad_bottom(card, 18, 0);
      lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_color(card, lv_color_hex(0x1A1A24), 0);
      lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t *nm = lv_label_create(card);
      lv_label_set_text(nm, p->tools[i].name);
      lv_obj_set_style_text_font(nm, FONT_TITLE, 0);
      lv_obj_set_style_text_color(nm, lv_color_hex(TH_TEXT_PRIMARY), 0);

      if (p->tools[i].description[0]) {
         lv_obj_t *ds = lv_label_create(card);
         lv_label_set_long_mode(ds, LV_LABEL_LONG_WRAP);
         lv_label_set_text(ds, p->tools[i].description);
         lv_obj_set_width(ds, SW - 2 * SIDE_PAD);
         lv_obj_set_style_text_font(ds, FONT_BODY, 0);
         lv_obj_set_style_text_color(ds, lv_color_hex(TH_TEXT_BODY), 0);
         lv_obj_set_style_text_line_space(ds, 4, 0);
      }
   }
}

/* ── Public API ──────────────────────────────────────────────── */

void ui_skills_show(void) {
   /* Match ui_agents pattern — load home screen first if a separate
    * lv_screen (camera/files) is currently active, so the overlay
    * renders on top of the right parent. */
   lv_obj_t *home = ui_home_get_screen();
   if (home && lv_screen_active() != home) {
      lv_screen_load(home);
   }

   if (s_overlay) {
      lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(s_overlay);
      s_visible = true;
      kick_off_fetch();
      return;
   }

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
   lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

   /* Back button — top-left, mirrors ui_agents geometry. */
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

   /* Header — "Skills" + count subtitle */
   lv_obj_t *head = lv_label_create(s_overlay);
   lv_label_set_text(head, "Skills");
   lv_obj_set_style_text_font(head, &lv_font_montserrat_48, 0);
   lv_obj_set_style_text_color(head, lv_color_hex(TH_AMBER), 0);
   lv_obj_set_pos(head, SIDE_PAD, 110);

   s_count_lbl = lv_label_create(s_overlay);
   lv_label_set_text(s_count_lbl, "Loading…");
   lv_obj_set_style_text_font(s_count_lbl, FONT_CAPTION, 0);
   lv_obj_set_style_text_color(s_count_lbl, lv_color_hex(TH_TEXT_SECONDARY), 0);
   lv_obj_set_style_text_letter_space(s_count_lbl, 3, 0);
   lv_obj_set_pos(s_count_lbl, SIDE_PAD, 200);

   s_visible = true;
   kick_off_fetch();
   ESP_LOGI(TAG, "skills overlay shown");
}

void ui_skills_hide(void) {
   if (!s_visible) return;
   if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
   s_visible = false;
   ESP_LOGI(TAG, "skills overlay hidden");
}

bool ui_skills_is_visible(void) { return s_visible; }
