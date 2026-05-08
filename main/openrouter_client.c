/* openrouter_client — Phase-1 skeleton.
 *
 * Auth helper + 4 stubbed verbs.  The real implementations land
 * across the next 4 commits (LLM streaming, STT, TTS, embeddings).
 *
 * TT #370 — see openrouter_client.h.
 */

#include "openrouter_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "openrouter_sse.h"
#include "settings.h"

static const char *TAG = "or_client";

#define OR_BASE_URL "https://openrouter.ai/api/v1"
#define OR_REFERER "https://tab5.tinkerclaw.local"

/* Single-flight: voice_solo never overlaps two calls. */
static esp_http_client_handle_t s_inflight = NULL;

/* Apply Authorization + the OpenRouter-recommended HTTP-Referer +
 * X-Title headers to a fresh esp_http_client handle.  Returns
 * ESP_ERR_INVALID_STATE if or_key empty so callers short-circuit
 * without opening the connection. */
esp_err_t openrouter_apply_auth(esp_http_client_handle_t c) {
   char key[96] = {0};
   esp_err_t err = tab5_settings_get_or_key(key, sizeof key);
   if (err != ESP_OK || key[0] == '\0') return ESP_ERR_INVALID_STATE;
   char hdr[128];
   snprintf(hdr, sizeof hdr, "Bearer %s", key);
   esp_http_client_set_header(c, "Authorization", hdr);
   esp_http_client_set_header(c, "HTTP-Referer", OR_REFERER);
   esp_http_client_set_header(c, "X-Title", "TinkerTab Solo");
   return ESP_OK;
}

/* Open a POST handle to OR_BASE_URL/<path> with auth applied.  Caller
 * adds Content-Type, calls esp_http_client_open(handle, body_len),
 * writes body, fetches response, then closes + cleans up.  s_inflight
 * is set to the new handle so openrouter_cancel_inflight can abort. */
esp_http_client_handle_t openrouter_open_post(const char *path) {
   char url[160];
   snprintf(url, sizeof url, "%s%s", OR_BASE_URL, path);
   esp_http_client_config_t cfg = {
       .url = url,
       .method = HTTP_METHOD_POST,
       .crt_bundle_attach = esp_crt_bundle_attach,
       .timeout_ms = 60000,
       .buffer_size = 4096,
       .buffer_size_tx = 2048,
   };
   esp_http_client_handle_t c = esp_http_client_init(&cfg);
   if (!c) return NULL;
   if (openrouter_apply_auth(c) != ESP_OK) {
      esp_http_client_cleanup(c);
      return NULL;
   }
   s_inflight = c;
   return c;
}

void openrouter_cancel_inflight(void) {
   if (s_inflight) {
      ESP_LOGW(TAG, "cancel_inflight — closing in-flight HTTP");
      esp_http_client_close(s_inflight);
   }
}

/* ── Stubs — filled in over the next 4 commits ──────────────────── */

esp_err_t openrouter_stt(const int16_t *pcm, size_t samples, char *out_text, size_t out_cap) {
   (void)pcm;
   (void)samples;
   (void)out_text;
   (void)out_cap;
   ESP_LOGW(TAG, "openrouter_stt stub");
   return ESP_ERR_NOT_SUPPORTED;
}

typedef struct {
   openrouter_chat_delta_cb_t cb;
   void *cb_ctx;
} chat_stream_ctx_t;

static void chat_sse_cb(const char *json, size_t len, void *vctx) {
   chat_stream_ctx_t *c = vctx;
   char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!copy) return;
   memcpy(copy, json, len);
   copy[len] = '\0';
   cJSON *root = cJSON_Parse(copy);
   if (root) {
      cJSON *choices = cJSON_GetObjectItem(root, "choices");
      if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
         cJSON *delta = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta");
         cJSON *content = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
         if (cJSON_IsString(content) && content->valuestring) {
            c->cb(content->valuestring, strlen(content->valuestring), c->cb_ctx);
         }
      }
      cJSON_Delete(root);
   }
   heap_caps_free(copy);
}

esp_err_t openrouter_chat_stream(const char *messages_json, openrouter_chat_delta_cb_t cb, void *ctx) {
   if (!messages_json || !cb) return ESP_ERR_INVALID_ARG;

   char model[96] = {0};
   tab5_settings_get_or_mdl_llm(model, sizeof model);

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "model", model);
   cJSON *msgs = cJSON_Parse(messages_json);
   if (!msgs) {
      cJSON_Delete(body);
      return ESP_ERR_INVALID_ARG;
   }
   cJSON_AddItemToObject(body, "messages", msgs);
   cJSON_AddBoolToObject(body, "stream", true);
   char *body_str = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!body_str) return ESP_ERR_NO_MEM;

   esp_http_client_handle_t c = openrouter_open_post("/chat/completions");
   if (!c) {
      free(body_str);
      return ESP_ERR_INVALID_STATE;
   }
   esp_http_client_set_header(c, "Content-Type", "application/json");
   esp_http_client_set_header(c, "Accept", "text/event-stream");

   chat_stream_ctx_t cctx = {.cb = cb, .cb_ctx = ctx};
   openrouter_sse_state_t *sse = NULL;
   if (openrouter_sse_init(&sse, chat_sse_cb, &cctx) != ESP_OK) {
      free(body_str);
      esp_http_client_cleanup(c);
      s_inflight = NULL;
      return ESP_ERR_NO_MEM;
   }

   esp_err_t err = esp_http_client_open(c, strlen(body_str));
   if (err != ESP_OK) goto cleanup;
   if (esp_http_client_write(c, body_str, strlen(body_str)) < 0) {
      err = ESP_FAIL;
      goto cleanup;
   }
   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "chat status=%d", status);
      err = ESP_FAIL;
      goto cleanup;
   }
   char rbuf[1024];
   while (1) {
      int n = esp_http_client_read(c, rbuf, sizeof rbuf);
      if (n <= 0) break;
      if (openrouter_sse_feed(sse, rbuf, (size_t)n)) break;
   }

cleanup:
   openrouter_sse_free(sse);
   esp_http_client_close(c);
   esp_http_client_cleanup(c);
   s_inflight = NULL;
   free(body_str);
   return err;
}

esp_err_t openrouter_tts(const char *text, openrouter_tts_chunk_cb_t cb, void *ctx) {
   (void)text;
   (void)cb;
   (void)ctx;
   ESP_LOGW(TAG, "openrouter_tts stub");
   return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t openrouter_embed(const char *text, float **out_vec, size_t *out_dim) {
   (void)text;
   if (out_vec) *out_vec = NULL;
   if (out_dim) *out_dim = 0;
   ESP_LOGW(TAG, "openrouter_embed stub");
   return ESP_ERR_NOT_SUPPORTED;
}
