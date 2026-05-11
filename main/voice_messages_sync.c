/*
 * voice_messages_sync.c — see voice_messages_sync.h.
 *
 * Wave 3-C-b (cross-stack cohesion audit 2026-05-11).
 */
#include "voice_messages_sync.h"

#include <string.h>

#include "cJSON.h"
#include "config.h" /* TAB5_VOICE_PORT */
#include "debug_obs.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings.h"
#include "task_worker.h"

static const char *TAG = "msg_sync";

/* Job state — fully self-contained.  Worker reads, no NVS touch. */
typedef struct {
   char url[160];  /* http://{host}:3502/api/v1/sessions/{session_id}/messages */
   char auth[160]; /* "Bearer {token}" or "" when no token */
   char *body;     /* PSRAM, free()d by worker */
   size_t body_len;
   /* For obs / log: keep role + a short content prefix.  Avoids
    * leaking secrets if a malformed caller passes a token as content. */
   char role[16];
   char preview[32];
} msg_sync_job_t;

static void free_job(msg_sync_job_t *job) {
   if (!job) return;
   if (job->body) heap_caps_free(job->body);
   heap_caps_free(job);
}

static void msg_sync_post_job(void *arg) {
   msg_sync_job_t *job = (msg_sync_job_t *)arg;
   if (!job) return;

   esp_http_client_config_t cfg = {
       .url = job->url,
       .method = HTTP_METHOD_POST,
       .timeout_ms = 5000,
       .buffer_size = 2048,
       .buffer_size_tx = 1024,
       .crt_bundle_attach = NULL,
   };
   esp_http_client_handle_t client = esp_http_client_init(&cfg);
   if (!client) {
      ESP_LOGW(TAG, "http_client_init failed (role=%s)", job->role);
      tab5_debug_obs_event("msg_sync.error", "init");
      free_job(job);
      return;
   }

   esp_http_client_set_header(client, "Content-Type", "application/json");
   if (job->auth[0]) {
      esp_http_client_set_header(client, "Authorization", job->auth);
   }

   esp_err_t err = esp_http_client_open(client, (int)job->body_len);
   if (err != ESP_OK) {
      ESP_LOGW(TAG, "open failed: %s (role=%s)", esp_err_to_name(err), job->role);
      tab5_debug_obs_event("msg_sync.error", "open");
      esp_http_client_cleanup(client);
      free_job(job);
      return;
   }
   int written = esp_http_client_write(client, job->body, (int)job->body_len);
   if (written < 0 || (size_t)written != job->body_len) {
      ESP_LOGW(TAG, "write short: %d/%u (role=%s)", written, (unsigned)job->body_len, job->role);
      tab5_debug_obs_event("msg_sync.error", "write");
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      free_job(job);
      return;
   }
   int content_len = esp_http_client_fetch_headers(client);
   (void)content_len;
   int status = esp_http_client_get_status_code(client);
   /* Drain body so the connection can be cleanly reused / closed. */
   char drain[256];
   while (esp_http_client_read(client, drain, sizeof drain) > 0) {
      /* discard */
   }
   esp_http_client_close(client);
   esp_http_client_cleanup(client);

   if (status >= 200 && status < 300) {
      ESP_LOGI(TAG, "POST ok %d role=%s preview=%s", status, job->role, job->preview);
      char detail[48];
      snprintf(detail, sizeof detail, "role=%s status=%d", job->role, status);
      tab5_debug_obs_event("msg_sync.ok", detail);
   } else {
      ESP_LOGW(TAG, "POST status=%d role=%s preview=%s", status, job->role, job->preview);
      char detail[48];
      snprintf(detail, sizeof detail, "role=%s status=%d", job->role, status);
      tab5_debug_obs_event("msg_sync.fail", detail);
   }
   free_job(job);
}

esp_err_t voice_messages_sync_post(const char *role, const char *content, const char *input_mode) {
   if (!role || !role[0] || !content || !content[0]) {
      return ESP_ERR_INVALID_ARG;
   }
   if (!input_mode || !input_mode[0]) input_mode = "text";

   /* NVS snapshot in caller's thread.  Worker will not touch NVS. */
   char session_id[64] = {0};
   char dragon_host[64] = {0};
   char dragon_tok[96] = {0};
   tab5_settings_get_session_id(session_id, sizeof session_id);
   tab5_settings_get_dragon_host(dragon_host, sizeof dragon_host);
   tab5_settings_get_dragon_api_token(dragon_tok, sizeof dragon_tok);

   if (!session_id[0] || !dragon_host[0]) {
      ESP_LOGD(TAG, "skip: session_id=%s dragon_host=%s", session_id[0] ? "set" : "empty",
               dragon_host[0] ? "set" : "empty");
      return ESP_ERR_INVALID_STATE;
   }

   /* Build JSON body via cJSON so escaping is correct for arbitrary
    * UTF-8 content.  Worker takes ownership of the resulting buffer. */
   cJSON *root = cJSON_CreateObject();
   if (!root) return ESP_ERR_NO_MEM;
   cJSON_AddStringToObject(root, "role", role);
   cJSON_AddStringToObject(root, "content", content);
   cJSON_AddStringToObject(root, "input_mode", input_mode);
   char *body = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!body) return ESP_ERR_NO_MEM;
   size_t body_len = strlen(body);

   /* Re-pack body into PSRAM so cJSON's internal-SRAM allocation can
    * be freed immediately. */
   char *psram_body = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!psram_body) {
      cJSON_free(body);
      return ESP_ERR_NO_MEM;
   }
   memcpy(psram_body, body, body_len + 1);
   cJSON_free(body);

   msg_sync_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!job) {
      heap_caps_free(psram_body);
      return ESP_ERR_NO_MEM;
   }
   snprintf(job->url, sizeof job->url, "http://%s:%d/api/v1/sessions/%s/messages", dragon_host, TAB5_VOICE_PORT,
            session_id);
   if (dragon_tok[0]) {
      snprintf(job->auth, sizeof job->auth, "Bearer %s", dragon_tok);
   }
   job->body = psram_body;
   job->body_len = body_len;
   strncpy(job->role, role, sizeof(job->role) - 1);
   /* Short preview for the obs/log — first ~30 chars, ASCII-only safe. */
   size_t prev_n = strlen(content);
   if (prev_n > sizeof(job->preview) - 1) prev_n = sizeof(job->preview) - 1;
   memcpy(job->preview, content, prev_n);

   esp_err_t r = tab5_worker_enqueue(msg_sync_post_job, job, "msg_sync");
   if (r != ESP_OK) {
      ESP_LOGW(TAG, "worker queue full — dropping (role=%s)", role);
      tab5_debug_obs_event("msg_sync.drop", "queue_full");
      free_job(job);
   }
   return r;
}
