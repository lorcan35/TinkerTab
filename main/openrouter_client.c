/* openrouter_client — Phase-1 skeleton.
 *
 * Auth helper + 4 stubbed verbs.  The real implementations land
 * across the next 4 commits (LLM streaming, STT, TTS, embeddings).
 *
 * TT #370 — see openrouter_client.h.
 */

#include "openrouter_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "or_client";

#define OR_BASE_URL "https://openrouter.ai/api/v1"
#define OR_REFERER  "https://tab5.tinkerclaw.local"

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

esp_err_t openrouter_stt(const int16_t *pcm, size_t samples,
                         char *out_text, size_t out_cap) {
   (void)pcm;
   (void)samples;
   (void)out_text;
   (void)out_cap;
   ESP_LOGW(TAG, "openrouter_stt stub");
   return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t openrouter_chat_stream(const char *messages_json,
                                 openrouter_chat_delta_cb_t cb, void *ctx) {
   (void)messages_json;
   (void)cb;
   (void)ctx;
   ESP_LOGW(TAG, "openrouter_chat_stream stub");
   return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t openrouter_tts(const char *text,
                         openrouter_tts_chunk_cb_t cb, void *ctx) {
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
