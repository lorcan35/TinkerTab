/* dictation_notes.c — dictation engine extracted from ui_notes.c (W5).
 *
 * First increment: the Dragon REST sync (POST /api/notes).  Shares the note
 * store with ui_notes.c via ui_notes_internal.h.  The transcription queue and
 * SD WAV I/O move here in later increments. */

#include "dictation_notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "ui_notes.h"          /* ui_notes_sync_pending decl */
#include "ui_notes_internal.h" /* shared store + helpers */
#include "wifi.h"

static const char *TAG = "dictation_notes";

typedef struct {
   char title[128];
   char text[MAX_NOTE_LEN];
   int note_idx; /* S6: index in s_notes for clearing needs_sync */
} sync_note_args_t;

static void sync_note_to_dragon_task(void *arg) {
   sync_note_args_t *a = (sync_note_args_t *)arg;

   /* Build Dragon URL from settings */
   char dhost[64];
   tab5_settings_get_dragon_host(dhost, sizeof(dhost));
   char url[160];
   snprintf(url, sizeof(url), "http://%s:%d/api/notes", dhost, 3502);

   /* Build JSON body */
   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "title", a->title);
   cJSON_AddStringToObject(body, "text", a->text);
   char *json = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);

   if (!json) {
      free(a);
      vTaskSuspend(NULL);
      return;
   }

   esp_http_client_config_t cfg = {
       .url = url,
       .method = HTTP_METHOD_POST,
       .timeout_ms = 10000,
   };
   esp_http_client_handle_t client = esp_http_client_init(&cfg);
   esp_http_client_set_header(client, "Content-Type", "application/json");
   /* Dragon W13 C2: /api/notes is bearer-gated.  Read dragon_tok from NVS
    * and add the header; without it every sync silently 401s. */
   char dtok[80];
   if (tab5_settings_get_dragon_api_token(dtok, sizeof(dtok)) == ESP_OK && dtok[0]) {
      char auth_hdr[96];
      snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", dtok);
      esp_http_client_set_header(client, "Authorization", auth_hdr);
   }
   esp_http_client_set_post_field(client, json, strlen(json));

   esp_err_t err = esp_http_client_perform(client);
   int status = esp_http_client_get_status_code(client);

   if (err == ESP_OK && (status == 200 || status == 201)) {
      ESP_LOGI(TAG, "Note synced to Dragon (status=%d, idx=%d)", status, a->note_idx);
      /* S6: Clear needs_sync flag on success */
      if (a->note_idx >= 0 && a->note_idx < MAX_NOTES) {
         s_notes[a->note_idx].needs_sync = false;
      }
   } else {
      ESP_LOGW(TAG, "Note sync failed: err=%s status=%d", esp_err_to_name(err), status);
   }

   esp_http_client_cleanup(client);
   free(json);
   free(a);
   vTaskSuspend(NULL);
}

void sync_note_to_dragon(const char *title, const char *text) {
   if (!text || !text[0]) return;
   int idx = find_note_idx_by_text(text);

   if (!tab5_wifi_connected()) {
      ESP_LOGW(TAG, "Note sync skipped — WiFi not connected");
      /* S6: Mark for later sync */
      if (idx >= 0) s_notes[idx].needs_sync = true;
      return;
   }

   sync_note_args_t *args = calloc(1, sizeof(sync_note_args_t));
   if (!args) return;
   strncpy(args->title, title ? title : "", sizeof(args->title) - 1);
   strncpy(args->text, text, sizeof(args->text) - 1);
   args->note_idx = idx;

   ESP_LOGI(TAG, "Syncing note to Dragon (%zu chars, idx=%d)", strlen(text), idx);
   xTaskCreatePinnedToCore(sync_note_to_dragon_task, "note_sync", 4096, args, 3, NULL, 0);
}

/* S6: Sync all pending notes to Dragon (called on reconnect) */
void ui_notes_sync_pending(void) {
   notes_load();
   int synced = 0;
   for (int i = 0; i < MAX_NOTES; i++) {
      if (s_notes[i].used && s_notes[i].needs_sync && s_notes[i].text[0]) {
         ESP_LOGI(TAG, "Catch-up sync: note %d", i);
         sync_note_to_dragon("", s_notes[i].text);
         synced++;
         vTaskDelay(pdMS_TO_TICKS(500)); /* stagger to avoid flooding */
      }
   }
   if (synced > 0) {
      ESP_LOGI(TAG, "Catch-up sync: %d notes queued", synced);
   }
}
