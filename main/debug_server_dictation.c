/*
 * debug_server_dictation.c — POST /dictation + GET /dictation handlers.
 *
 * Wave 23b follow-up (#332): ninth per-family extract.  Single
 * handler moved verbatim from debug_server.c.  Only call-site changes
 * are the standard `check_auth` → `tab5_debug_check_auth` substitution
 * (no send_json_resp use — the handler builds the response inline).
 */

#include "debug_server_dictation.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug_server_internal.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "voice.h" /* voice_start_dictation, voice_stop_listening, voice_get_dictation_text, voice_get_state */

static const char *TAG = "debug_dictation";

/* ── Dictation endpoint ───────────────────────────────────────────────── */
/* POST /dictation?action=start|stop — remote dictation driver for the
 * long-duration pipeline tests (5 / 10 / 30 min).  GET also returns the
 * current accumulated transcript so the test harness can snapshot it
 * mid-run.  Long-press mic is the user-facing trigger; this endpoint
 * is the automation equivalent. */
static esp_err_t dictation_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   char query[128] = {0};
   char action[16] = {0};
   if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      httpd_query_key_value(query, "action", action, sizeof(action));
   }

   cJSON *root = cJSON_CreateObject();
   if (req->method == HTTP_POST && strcmp(action, "start") == 0) {
      esp_err_t err = voice_start_dictation();
      cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
      cJSON_AddStringToObject(root, "action", "start");
      if (err != ESP_OK) cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
   } else if (req->method == HTTP_POST && strcmp(action, "stop") == 0) {
      esp_err_t err = voice_stop_listening();
      cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
      cJSON_AddStringToObject(root, "action", "stop");
      if (err != ESP_OK) cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
   } else {
      /* GET — snapshot of current transcript + state */
      cJSON_AddBoolToObject(root, "ok", true);
      cJSON_AddStringToObject(root, "action", "status");
   }
   const char *txt = voice_get_dictation_text();
   cJSON_AddStringToObject(root, "transcript", txt ? txt : "");
   cJSON_AddNumberToObject(root, "transcript_len", txt ? (int)strlen(txt) : 0);
   cJSON_AddNumberToObject(root, "state", (int)voice_get_state());
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_sendstr(req, json);
   free(json);
   return ESP_OK;
}

void debug_server_dictation_register(httpd_handle_t server) {
   const httpd_uri_t uri_dictation_post = {.uri = "/dictation", .method = HTTP_POST, .handler = dictation_handler};
   const httpd_uri_t uri_dictation_get = {.uri = "/dictation", .method = HTTP_GET, .handler = dictation_handler};
   httpd_register_uri_handler(server, &uri_dictation_post);
   httpd_register_uri_handler(server, &uri_dictation_get);
   ESP_LOGI(TAG, "Dictation endpoint family registered (2 URIs, 1 handler)");
}
