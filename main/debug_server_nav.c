/*
 * debug_server_nav.c — screen navigation debug HTTP family.
 *
 * Wave 23b follow-up (#332): fourteenth per-family extract.  Owns:
 *
 *   POST /navigate?screen=...  — LVGL-thread screen swap with 500 ms debounce
 *   GET  /screen               — current screen + overlay visibility (chat /
 *                                voice / settings) for the e2e harness
 *
 * `tab5_debug_set_nav_target()` (declared in debug_server.h) is the public
 * setter ui_chrome / ui_chat / ui_camera / ui_files / ui_notes / ui_settings /
 * ui_wifi call when their swipe-back / back-button / home-button paths
 * change the loaded screen without going through /navigate.  Keeps the
 * harness's /screen view honest.
 *
 * Same convention as the prior 13 per-family extracts:
 *   check_auth(req)            → tab5_debug_check_auth(req)
 *   send_json_resp(req, root)  → tab5_debug_send_json_resp(req, root)
 */
#include "debug_server_nav.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug_obs.h"
#include "debug_server_internal.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ui_core.h" /* tab5_lv_async_call */
#include "ui_home.h"
#include "ui_keyboard.h"

/* No TAG here — only ESP_LOG calls below use the literal "nav" tag for
 * historical compat with the original debug_server.c log spelling. */

#define check_auth(req) tab5_debug_check_auth(req)
#define send_json_resp(req, root) tab5_debug_send_json_resp(req, root)

/* Last-requested screen via /navigate.  Defaults to "" until first navigate
 * fires (the /screen handler renders this as "home" — Tab5 boots into home). */
static char s_nav_target[16] = {0};
static volatile bool s_navigating = false;

/* TT #328 Wave 10 follow-up — public setter so non-/navigate code paths
 * (the persistent home button in ui_chrome, swipe-right-back gestures
 * in chat/camera/files) can keep s_nav_target in sync with the actually
 * loaded screen.  Pre-fix /screen would return the stale last-/navigate
 * target while the device was on a different screen, which broke the
 * harness's persistent-home-button assertion. */
void tab5_debug_set_nav_target(const char *name) {
   if (!name || !name[0]) return;
   size_t cap = sizeof(s_nav_target) - 1;
   strncpy(s_nav_target, name, cap);
   s_nav_target[cap] = '\0';
   tab5_debug_obs_event("screen.navigate", s_nav_target);
}

static void async_navigate(void *arg) {
   (void)arg;
   if (s_navigating) {
      ESP_LOGW("nav", "Navigation already in progress — skipping");
      return;
   }
   s_navigating = true;
   ESP_LOGI("nav", "async_navigate executing, target='%s'", s_nav_target);
   /* Navigate using the home tileview for pages 0-3.
    * For secondary screens (camera, files) create them separately.
    * This avoids the bug where ui_settings_create() replaces the
    * tileview screen entirely, breaking subsequent navigation. */

   /* Always dismiss ALL overlays before any navigation — HIDE not destroy.
    * Destroy+recreate corrupts LVGL timer linked list (lv_ll_remove crash). */
   extern void ui_chat_hide(void);
   extern void ui_settings_hide(void);
   extern void ui_notes_hide(void);
   extern void ui_agents_hide(void);
   extern void ui_memory_hide(void);
   extern void ui_focus_hide(void);
   extern void ui_sessions_hide(void);
   ui_chat_hide();
   ui_settings_hide();
   ui_notes_hide();
   ui_agents_hide();
   ui_memory_hide();
   ui_focus_hide();
   ui_sessions_hide();
   ui_keyboard_hide();
   /* Wave 4 UX fix: if the voice overlay is up but voice is idle, a
    * nav request means the user has moved on.  Dismiss so the user
    * sees the screen they just navigated to instead of a stale
    * "Tap to speak." card blocking the view.  Active states
    * (LISTENING / PROCESSING / SPEAKING) are left alone. */
   extern void ui_voice_dismiss_if_idle(void);
   ui_voice_dismiss_if_idle();

   /* #167: camera + files use destroy/create semantics (they own big
    * PSRAM canvas buffers + LVGL screen trees, not hide/show overlays).
    * If the user navigates away from them without hitting the back
    * chevron (e.g. /navigate or nav-sheet), we must tear them down here
    * or we leak ~1.8 MB + a zombie preview timer per camera cycle.
    * Both functions are NULL-guarded internally. */
   extern void ui_camera_destroy(void);
   extern void ui_files_destroy(void);
   ui_camera_destroy();
   ui_files_destroy();

   if (strcmp(s_nav_target, "home") == 0) {
      ui_home_go_home();
   } else if (strcmp(s_nav_target, "notes") == 0) {
      extern void *ui_notes_create(void);
      ui_notes_create();
   } else if (strcmp(s_nav_target, "chat") == 0) {
      extern void *ui_chat_create(void);
      ui_chat_create();
   } else if (strcmp(s_nav_target, "settings") == 0) {
      /* Settings is too heavy for lv_async_call — it blocks the LVGL render loop.
       * Use direct call since async_navigate already runs in LVGL context via lv_async_call. */
      extern void ui_home_nav_settings(void);
      ui_home_nav_settings();
   } else if (strcmp(s_nav_target, "camera") == 0) {
      extern void *ui_camera_create(void);
      ui_camera_create();
   } else if (strcmp(s_nav_target, "files") == 0) {
      extern void *ui_files_create(void);
      ui_files_create();
   } else if (strcmp(s_nav_target, "sessions") == 0) {
      extern void ui_sessions_show(void);
      ui_sessions_show();
   } else if (strcmp(s_nav_target, "agents") == 0) {
      extern void ui_agents_show(void);
      ui_agents_show();
   } else if (strcmp(s_nav_target, "skills") == 0) {
      /* TT #328 Wave 10 — dedicated tools-catalog viewer. */
      extern void ui_skills_show(void);
      ui_skills_show();
   } else if (strcmp(s_nav_target, "memory") == 0) {
      extern void ui_memory_show(void);
      ui_memory_show();
   } else if (strcmp(s_nav_target, "focus") == 0) {
      extern void ui_focus_show(void);
      ui_focus_show();
   } else if (strcmp(s_nav_target, "wifi") == 0) {
      /* #148: folded in from the removed /open endpoint so /navigate
       * is the single source of truth for all screen lists. */
      extern void *ui_wifi_create(void);
      ui_wifi_create();
   }
   s_navigating = false;
}

static esp_err_t navigate_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   /* Debounce: reject rapid navigation requests (same as UI 300ms debounce).
    * Without this, rapid /navigate calls queue multiple lv_async_call entries
    * that create/destroy LVGL overlays simultaneously → Load access fault. */
   static int64_t s_last_nav_us = 0;
   int64_t now = esp_timer_get_time();
   if (now - s_last_nav_us < 500000) { /* 500ms debounce */
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"navigation too fast, wait 500ms\"}");
      return ESP_OK;
   }
   s_last_nav_us = now;

   char query[64] = {0};
   if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"use ?screen=settings|notes|chat|camera|files|home\"}");
      return ESP_OK;
   }

   httpd_query_key_value(query, "screen", s_nav_target, sizeof(s_nav_target));

   /* #293: emit obs event for e2e harness BEFORE the async dispatch so
    * the harness sees the navigation intent even if the dispatch is
    * queued.  s_current_screen also gets updated on the LVGL side via
    * async_navigate completion. */
   tab5_debug_obs_event("screen.navigate", s_nav_target);

   /* Schedule on LVGL thread (#258 helper takes the recursive LVGL
    * mutex internally — lv_async_call itself is NOT thread-safe). */
   tab5_lv_async_call(async_navigate, NULL);

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "navigated", s_nav_target);
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* #293: GET /screen — current screen + overlay visibility for the e2e
 * harness.  s_nav_target is the last-requested screen via /navigate;
 * overlay state comes from the per-module ui_*_is_visible() / is_active()
 * helpers. */
static esp_err_t screen_state_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   extern bool ui_chat_is_active(void);
   extern bool ui_voice_is_visible(void);
   extern bool ui_settings_is_visible(void);

   cJSON *root = cJSON_CreateObject();
   /* Last-requested screen via /navigate.  Defaults to "home" until
    * the first navigate fires (Tab5 boots into home). */
   cJSON_AddStringToObject(root, "current", s_nav_target[0] ? s_nav_target : "home");
   cJSON *overlays = cJSON_CreateObject();
   cJSON_AddBoolToObject(overlays, "chat", ui_chat_is_active());
   cJSON_AddBoolToObject(overlays, "voice", ui_voice_is_visible());
   cJSON_AddBoolToObject(overlays, "settings", ui_settings_is_visible());
   cJSON_AddItemToObject(root, "overlays", overlays);

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

void debug_server_nav_register(httpd_handle_t server) {
   if (!server) return;

   static const httpd_uri_t uri_navigate = {.uri = "/navigate", .method = HTTP_POST, .handler = navigate_handler};
   static const httpd_uri_t uri_screen = {.uri = "/screen", .method = HTTP_GET, .handler = screen_state_handler};

   httpd_register_uri_handler(server, &uri_navigate);
   httpd_register_uri_handler(server, &uri_screen);
}
