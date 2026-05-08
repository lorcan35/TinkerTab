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

/* Build a 44-byte WAV RIFF header for PCM-16LE @ 16 kHz mono.  Caller
 * writes the PCM right after.  Required by /audio/transcriptions
 * which expects an audio container, not raw PCM. */
static void wav16_header(uint8_t *hdr, size_t pcm_bytes) {
   const uint32_t sample_rate = 16000;
   const uint16_t num_ch = 1;
   const uint16_t bits = 16;
   const uint32_t byte_rate = sample_rate * num_ch * bits / 8;
   uint32_t riff_size = 36u + (uint32_t)pcm_bytes;
   uint32_t fmt_size = 16;
   uint16_t fmt_pcm = 1;
   uint16_t block_align = num_ch * bits / 8;
   uint32_t data_size = (uint32_t)pcm_bytes;
   memcpy(hdr, "RIFF", 4);
   memcpy(hdr + 4, &riff_size, 4);
   memcpy(hdr + 8, "WAVE", 4);
   memcpy(hdr + 12, "fmt ", 4);
   memcpy(hdr + 16, &fmt_size, 4);
   memcpy(hdr + 20, &fmt_pcm, 2);
   memcpy(hdr + 22, &num_ch, 2);
   memcpy(hdr + 24, &sample_rate, 4);
   memcpy(hdr + 28, &byte_rate, 4);
   memcpy(hdr + 32, &block_align, 2);
   memcpy(hdr + 34, &bits, 2);
   memcpy(hdr + 36, "data", 4);
   memcpy(hdr + 40, &data_size, 4);
}

esp_err_t openrouter_stt(const int16_t *pcm, size_t samples, char *out_text, size_t out_cap) {
   if (!pcm || !out_text || out_cap < 16 || samples == 0) return ESP_ERR_INVALID_ARG;

   char model[64] = {0};
   tab5_settings_get_or_mdl_stt(model, sizeof model);

   static const char kBoundary[] = "----tab5-solo-boundary";
   const size_t pcm_bytes = samples * sizeof(int16_t);
   const size_t wav_bytes = 44 + pcm_bytes;

   /* Build the part-A and part-B framing strings up front.  Their
    * sizes go into the Content-Length we hand to esp_http_client_open. */
   char part_a[256];
   int part_a_n = snprintf(part_a, sizeof part_a,
                           "--%s\r\nContent-Disposition: form-data; "
                           "name=\"file\"; filename=\"audio.wav\"\r\n"
                           "Content-Type: audio/wav\r\n\r\n",
                           kBoundary);

   char part_b[160];
   int part_b_n = snprintf(part_b, sizeof part_b,
                           "\r\n--%s\r\nContent-Disposition: form-data; "
                           "name=\"model\"\r\n\r\n%s\r\n--%s--\r\n",
                           kBoundary, model, kBoundary);

   if (part_a_n <= 0 || part_b_n <= 0 || (size_t)part_b_n >= sizeof part_b) {
      return ESP_ERR_INVALID_SIZE;
   }
   const size_t total = (size_t)part_a_n + wav_bytes + (size_t)part_b_n;

   esp_http_client_handle_t c = openrouter_open_post("/audio/transcriptions");
   if (!c) return ESP_ERR_INVALID_STATE;
   char ctype[96];
   snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", kBoundary);
   esp_http_client_set_header(c, "Content-Type", ctype);

   esp_err_t err = esp_http_client_open(c, total);
   if (err != ESP_OK) goto cleanup;
   if (esp_http_client_write(c, part_a, part_a_n) < 0) {
      err = ESP_FAIL;
      goto cleanup;
   }

   uint8_t hdr[44];
   wav16_header(hdr, pcm_bytes);
   if (esp_http_client_write(c, (const char *)hdr, sizeof hdr) < 0) {
      err = ESP_FAIL;
      goto cleanup;
   }

   /* PCM in 4 KB chunks to bound TX-buffer memcpy churn on PSRAM. */
   const char *pcm_b = (const char *)pcm;
   size_t off = 0;
   while (off < pcm_bytes) {
      size_t n = pcm_bytes - off;
      if (n > 4096) n = 4096;
      int wrote = esp_http_client_write(c, pcm_b + off, n);
      if (wrote < 0) {
         err = ESP_FAIL;
         goto cleanup;
      }
      off += (size_t)wrote;
   }
   if (esp_http_client_write(c, part_b, part_b_n) < 0) {
      err = ESP_FAIL;
      goto cleanup;
   }

   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "stt status=%d", status);
      err = ESP_FAIL;
      goto cleanup;
   }

   /* Response is JSON: {"text":"..."}.  Cap at 16 KB into PSRAM. */
   const size_t rcap = 16 * 1024;
   char *rbuf = heap_caps_calloc(1, rcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!rbuf) {
      err = ESP_ERR_NO_MEM;
      goto cleanup;
   }
   size_t total_read = 0;
   while (total_read + 1 < rcap) {
      int n = esp_http_client_read(c, rbuf + total_read, rcap - 1 - total_read);
      if (n <= 0) break;
      total_read += (size_t)n;
   }
   cJSON *root = cJSON_Parse(rbuf);
   heap_caps_free(rbuf);
   if (root) {
      cJSON *t = cJSON_GetObjectItem(root, "text");
      if (cJSON_IsString(t) && t->valuestring) {
         strncpy(out_text, t->valuestring, out_cap - 1);
         out_text[out_cap - 1] = '\0';
      } else {
         err = ESP_FAIL;
      }
      cJSON_Delete(root);
   } else {
      err = ESP_FAIL;
   }

cleanup:
   esp_http_client_close(c);
   esp_http_client_cleanup(c);
   s_inflight = NULL;
   return err;
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
   if (!text || !*text || !cb) return ESP_ERR_INVALID_ARG;

   char model[64] = {0};
   tab5_settings_get_or_mdl_tts(model, sizeof model);
   char voice[32] = {0};
   tab5_settings_get_or_voice(voice, sizeof voice);

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "model", model);
   cJSON_AddStringToObject(body, "voice", voice);
   cJSON_AddStringToObject(body, "input", text);
   cJSON_AddStringToObject(body, "response_format", "wav");
   char *body_str = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!body_str) return ESP_ERR_NO_MEM;

   esp_http_client_handle_t c = openrouter_open_post("/audio/speech");
   if (!c) {
      free(body_str);
      return ESP_ERR_INVALID_STATE;
   }
   esp_http_client_set_header(c, "Content-Type", "application/json");

   esp_err_t err = esp_http_client_open(c, strlen(body_str));
   if (err != ESP_OK) goto cleanup;
   if (esp_http_client_write(c, body_str, strlen(body_str)) < 0) {
      err = ESP_FAIL;
      goto cleanup;
   }
   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "tts status=%d", status);
      err = ESP_FAIL;
      goto cleanup;
   }

   /* Skip the 44-byte WAV RIFF header — caller wants raw PCM-16LE. */
   uint8_t header[44];
   int hr = 0;
   while (hr < (int)sizeof header) {
      int n = esp_http_client_read(c, (char *)header + hr, sizeof header - hr);
      if (n <= 0) {
         err = ESP_FAIL;
         goto cleanup;
      }
      hr += n;
   }

   /* Stream PCM payload to the callback in 4 KB chunks. */
   uint8_t buf[4096];
   while (1) {
      int n = esp_http_client_read(c, (char *)buf, sizeof buf);
      if (n <= 0) break;
      cb(buf, (size_t)n, ctx);
   }

cleanup:
   esp_http_client_close(c);
   esp_http_client_cleanup(c);
   s_inflight = NULL;
   free(body_str);
   return err;
}

esp_err_t openrouter_embed(const char *text, float **out_vec, size_t *out_dim) {
   if (!text || !out_vec || !out_dim) return ESP_ERR_INVALID_ARG;
   *out_vec = NULL;
   *out_dim = 0;

   char model[64] = {0};
   tab5_settings_get_or_mdl_emb(model, sizeof model);

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "model", model);
   cJSON_AddStringToObject(body, "input", text);
   char *body_str = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!body_str) return ESP_ERR_NO_MEM;

   esp_http_client_handle_t c = openrouter_open_post("/embeddings");
   if (!c) {
      free(body_str);
      return ESP_ERR_INVALID_STATE;
   }
   esp_http_client_set_header(c, "Content-Type", "application/json");

   esp_err_t err = esp_http_client_open(c, strlen(body_str));
   if (err != ESP_OK) goto cleanup;
   if (esp_http_client_write(c, body_str, strlen(body_str)) < 0) {
      err = ESP_FAIL;
      goto cleanup;
   }
   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "embed status=%d", status);
      err = ESP_FAIL;
      goto cleanup;
   }

   /* Response body in PSRAM.  text-embedding-3-small returns 1536
    * floats × ~12 chars each ≈ 18 KB; 32 KB is comfortable. */
   const size_t cap = 32 * 1024;
   char *rbuf = heap_caps_calloc(1, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!rbuf) {
      err = ESP_ERR_NO_MEM;
      goto cleanup;
   }
   size_t total_read = 0;
   while (total_read + 1 < cap) {
      int n = esp_http_client_read(c, rbuf + total_read, cap - 1 - total_read);
      if (n <= 0) break;
      total_read += (size_t)n;
   }
   cJSON *root = cJSON_Parse(rbuf);
   heap_caps_free(rbuf);
   if (!root) {
      err = ESP_FAIL;
      goto cleanup;
   }
   cJSON *data = cJSON_GetObjectItem(root, "data");
   if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
      cJSON *embedding = cJSON_GetObjectItem(cJSON_GetArrayItem(data, 0), "embedding");
      if (cJSON_IsArray(embedding)) {
         int dim = cJSON_GetArraySize(embedding);
         float *v = heap_caps_malloc((size_t)dim * sizeof(float),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
         if (v) {
            for (int i = 0; i < dim; i++) {
               cJSON *e = cJSON_GetArrayItem(embedding, i);
               v[i] = e ? (float)e->valuedouble : 0.0f;
            }
            *out_vec = v;
            *out_dim = (size_t)dim;
         } else {
            err = ESP_ERR_NO_MEM;
         }
      } else {
         err = ESP_FAIL;
      }
   } else {
      err = ESP_FAIL;
   }
   cJSON_Delete(root);

cleanup:
   esp_http_client_close(c);
   esp_http_client_cleanup(c);
   s_inflight = NULL;
   free(body_str);
   return err;
}
