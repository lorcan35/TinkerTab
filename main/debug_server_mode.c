/*
 * debug_server_mode.c — POST /mode endpoint family.
 *
 * Wave 23b follow-up (#332): fifth per-family extract.  Single
 * handler + its async LVGL-thread refresh helper moved verbatim from
 * debug_server.c.  Only call-site changes are the standard
 * `check_auth` → `tab5_debug_check_auth` and `send_json_resp` →
 * `tab5_debug_send_json_resp` substitutions.
 */

#include "debug_server_mode.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug_server_internal.h" /* tab5_debug_check_auth + send_json_resp */
#include "esp_http_server.h"
#include "esp_log.h"
#include "settings.h" /* tab5_settings_set_voice_mode etc. */
#include "ui_core.h"  /* tab5_lv_async_call (#258) */
#include "voice.h"    /* voice_is_connected, voice_send_config_update */

/* Async wrapper for tab5_lv_async_call — refreshes home mode badge on LVGL thread. */
static void async_refresh_mode_badge(void *arg) {
   (void)arg;
   extern void ui_home_refresh_mode_badge(void);
   ui_home_refresh_mode_badge();
}

/* POST /mode?m=0|1|2|3|4&model=... — switch voice mode.
 * mode 3=TinkerClaw; mode 4=VMODE_LOCAL_ONBOARD (K144, Tab5-side only). */
static esp_err_t mode_set_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   /* Parse query string */
   char query[128] = {0};
   if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
      cJSON *err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "error",
                              "use ?m=0|1|2|3|4|5&model=... (3=TinkerClaw, 4=K144 onboard, 5=Solo direct)");
      return tab5_debug_send_json_resp(req, err);
   }

   char val[8] = {0};
   char model[64] = {0};
   httpd_query_key_value(query, "m", val, sizeof(val));
   httpd_query_key_value(query, "model", model, sizeof(model));

   int mode = atoi(val);
   /* TT #317 Phase 5: vmode=4 added for VMODE_LOCAL_ONBOARD (K144).
    * TT #370 Phase 1: vmode=5 added for VMODE_SOLO_DIRECT (OpenRouter
    * direct, no Dragon, no K144). */
   if (mode < 0 || mode > 5) mode = 0;

   /* Save to NVS */
   tab5_settings_set_voice_mode((uint8_t)mode);
   if (model[0]) {
      tab5_settings_set_llm_model(model);
   }

   ESP_LOGI("debug_mode", "Mode switch: voice_mode=%d, llm_model=%s", mode, model[0] ? model : "(unchanged)");

   /* Send config_update to Dragon via voice WS — except for vmode=4
    * (VMODE_LOCAL_ONBOARD) and vmode=5 (VMODE_SOLO_DIRECT) which are
    * both Tab5-side-only tiers.  Dragon doesn't understand them and
    * would ACK with an error that reverts our NVS write back to 0.
    * When user picks one of those, tell Dragon we're still in
    * "local" (mode=0) so its STT/TTS stay on local backends; Tab5
    * then bypasses Dragon for the turn via voice_send_text. */
   if (voice_is_connected()) {
      int dragon_mode = (mode >= 4) ? 0 : mode;
      voice_send_config_update(dragon_mode, model[0] ? model : NULL);
   }

   /* Refresh home screen mode badge (runs on LVGL thread) */
   tab5_lv_async_call(async_refresh_mode_badge, NULL);

   /* Return current state */
   cJSON *root = cJSON_CreateObject();
   cJSON_AddNumberToObject(root, "voice_mode", mode);
   const char *mode_names[] = {"local", "hybrid", "cloud", "tinkerclaw", "local_onboard", "solo_direct"};
   cJSON_AddStringToObject(root, "mode_name", mode <= 5 ? mode_names[mode] : "unknown");
   char cur_model[64];
   tab5_settings_get_llm_model(cur_model, sizeof(cur_model));
   cJSON_AddStringToObject(root, "llm_model", cur_model);
   cJSON_AddBoolToObject(root, "voice_connected", voice_is_connected());
   cJSON_AddStringToObject(root, "status", "applied");
   return tab5_debug_send_json_resp(req, root);
}

void debug_server_mode_register(httpd_handle_t server) {
   const httpd_uri_t uri_mode_set = {.uri = "/mode", .method = HTTP_POST, .handler = mode_set_handler};
   httpd_register_uri_handler(server, &uri_mode_set);
   ESP_LOGI("debug_mode", "Mode endpoint family registered (1 URI)");
}
