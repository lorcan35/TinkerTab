/* openrouter_client — Phase-1 skeleton.
 *
 * Auth helper + 4 stubbed verbs.  The real implementations land
 * across the next 4 commits (LLM streaming, STT, TTS, embeddings).
 *
 * TT #370 — see openrouter_client.h.
 */

#include "openrouter_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h" /* TT #377: WAV → base64 for OpenRouter input_audio */
#include "openrouter_sse.h"
#include "settings.h"

static const char *TAG = "or_client";

#define OR_BASE_URL "https://openrouter.ai/api/v1"
#define OR_REFERER "https://tab5.tinkerclaw.local"

/* W1-D (TT #372): mutex protects s_inflight from use-after-free.
 * Old code: a worker setting s_inflight=NULL during cleanup could race
 * with a concurrent openrouter_cancel_inflight() reading the handle —
 * cancel might call esp_http_client_close on a freed pointer.
 * Now: every read/write of s_inflight is taken under s_inflight_mtx. */
static esp_http_client_handle_t s_inflight = NULL;
static SemaphoreHandle_t s_inflight_mtx = NULL;

static void inflight_init_once(void) {
   if (!s_inflight_mtx) {
      s_inflight_mtx = xSemaphoreCreateMutex();
   }
}
static void inflight_set(esp_http_client_handle_t h) {
   inflight_init_once();
   if (s_inflight_mtx) xSemaphoreTake(s_inflight_mtx, portMAX_DELAY);
   s_inflight = h;
   if (s_inflight_mtx) xSemaphoreGive(s_inflight_mtx);
}

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
   /* W2-B (TT #373): force fresh TCP per call.  Without this OpenRouter
    * sometimes responds Connection: keep-alive and esp_http_client
    * caches the underlying socket; after ~5 min of activity the cached
    * socket goes silently dead and the next call hangs in TLS handshake
    * for 60 s before timing out.  "Connection: close" makes both ends
    * tear down after each response, so every solo turn starts from a
    * clean TCP+TLS handshake. */
   esp_http_client_set_header(c, "Connection", "close");
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
       /* W1-A (TT #372): bumped 2 KB → 8 KB.  Multi-turn chat history
        * generated bodies in the 4-7 KB range; a 2 KB tx buffer caused
        * partial writes that downstream openrouter_chat_stream did NOT
        * loop on, so OpenRouter received truncated JSON → 400 → ESP_FAIL
        * with 0 deltas.  8 KB covers ~25 turns of typical Sonnet history. */
       .buffer_size_tx = 8192,
   };
   esp_http_client_handle_t c = esp_http_client_init(&cfg);
   if (!c) return NULL;
   if (openrouter_apply_auth(c) != ESP_OK) {
      esp_http_client_cleanup(c);
      return NULL;
   }
   inflight_set(c);
   return c;
}

/* Write `body` of `len` bytes to the open handle, looping over partial
 * writes (esp_http_client_write may return < len under tx-buffer or
 * mbedTLS back-pressure).  Returns ESP_OK on full write, ESP_FAIL on
 * any short/error return.  Used by chat/tts/embed senders. */
static esp_err_t write_full(esp_http_client_handle_t c, const char *body, size_t len) {
   size_t off = 0;
   while (off < len) {
      int n = esp_http_client_write(c, body + off, len - off);
      if (n <= 0) {
         ESP_LOGE(TAG, "write_full: short write at offset %u/%u (rc=%d)", (unsigned)off, (unsigned)len, n);
         return ESP_FAIL;
      }
      off += (size_t)n;
   }
   return ESP_OK;
}

/* Drain + log the response body on a non-200.  Eats up to 2 KB into a
 * stack buffer so the OpenRouter error JSON is visible in logs.  Without
 * this, every failure looked like generic ESP_FAIL with no diagnostic. */
static void log_error_body(esp_http_client_handle_t c, const char *path) {
   char buf[2048];
   int total = 0;
   while (total < (int)sizeof(buf) - 1) {
      int n = esp_http_client_read(c, buf + total, sizeof(buf) - 1 - total);
      if (n <= 0) break;
      total += n;
   }
   if (total > 0) {
      buf[total] = '\0';
      ESP_LOGE(TAG, "%s error body (%d bytes): %s", path, total, buf);
   } else {
      ESP_LOGE(TAG, "%s no error body returned", path);
   }
}

/* W2-A (TT #373): one-line health snapshot.  Goes in the log before each
 * outbound call + on every failure path so the sustained-load
 * degradation has a paper trail.  largest_free_block on internal SRAM is
 * the right canary: TLS handshake needs ~6-8 KB contiguous and a
 * fragmented heap (<32 KB largest free) reliably fails the next
 * mbedtls_ssl_handshake even though total free SRAM looks fine. */
static void log_health(const char *path, const char *phase) {
   size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
   size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
   size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
   if (int_largest < 32 * 1024) {
      ESP_LOGW(TAG,
               "%s [%s] internal_free=%uB largest=%uB psram_free=%uMB "
               "(LOW — TLS handshake at risk)",
               path, phase, (unsigned)int_free, (unsigned)int_largest, (unsigned)(psram_free / (1024 * 1024)));
   } else {
      ESP_LOGI(TAG, "%s [%s] internal_free=%uKB largest=%uKB psram_free=%uMB", path, phase, (unsigned)(int_free / 1024),
               (unsigned)(int_largest / 1024), (unsigned)(psram_free / (1024 * 1024)));
   }
}

void openrouter_cancel_inflight(void) {
   /* W1-D: take + clear under mutex so we never call close on a handle
    * that the worker just freed in its cleanup path. */
   inflight_init_once();
   esp_http_client_handle_t h = NULL;
   if (s_inflight_mtx) xSemaphoreTake(s_inflight_mtx, portMAX_DELAY);
   h = s_inflight;
   s_inflight = NULL; /* worker's "s_inflight = NULL" in cleanup is now a no-op */
   if (s_inflight_mtx) xSemaphoreGive(s_inflight_mtx);
   if (h) {
      ESP_LOGW(TAG, "cancel_inflight — closing in-flight HTTP");
      esp_http_client_close(h);
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

   char model[96] = {0};
   tab5_settings_get_or_mdl_stt(model, sizeof model);

   /* TT #377: OpenRouter's /audio/transcriptions takes JSON, not
    * multipart/form-data — `{"model":"...","input_audio":{"data":"<b64>","format":"wav"}}`.
    * Probed live 2026-05-11; previous multipart implementation got
    * `"No number after minus sign in JSON at position 1"` because
    * their gateway hits the body with a JSON parser regardless of
    * Content-Type.  The OpenAI multipart shape is for openai.com
    * directly, not openrouter.ai. */
   const size_t pcm_bytes = samples * sizeof(int16_t);
   const size_t wav_bytes = 44 + pcm_bytes;

   uint8_t *wav = heap_caps_malloc(wav_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!wav) return ESP_ERR_NO_MEM;
   wav16_header(wav, pcm_bytes);
   memcpy(wav + 44, pcm, pcm_bytes);

   /* Base64 grows by 4/3 + padding + null. */
   const size_t b64_cap = ((wav_bytes + 2) / 3) * 4 + 1;
   char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!b64) {
      heap_caps_free(wav);
      return ESP_ERR_NO_MEM;
   }
   size_t b64_olen = 0;
   int mb = mbedtls_base64_encode((unsigned char *)b64, b64_cap, &b64_olen, wav, wav_bytes);
   heap_caps_free(wav);
   if (mb != 0) {
      ESP_LOGE(TAG, "base64_encode failed: -0x%04x", -mb);
      heap_caps_free(b64);
      return ESP_FAIL;
   }
   b64[b64_olen] = '\0';

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "model", model);
   cJSON *input_audio = cJSON_CreateObject();
   cJSON_AddStringToObject(input_audio, "data", b64);
   cJSON_AddStringToObject(input_audio, "format", "wav");
   cJSON_AddItemToObject(body, "input_audio", input_audio);
   char *body_str = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   heap_caps_free(b64);
   if (!body_str) return ESP_ERR_NO_MEM;
   const size_t body_len = strlen(body_str);

   log_health("/audio/transcriptions", "pre");

   int attempt = 0;
   esp_http_client_handle_t c = NULL;
   esp_err_t err = ESP_FAIL;
   int64_t t_open = 0;
retry:
   attempt++;
   c = openrouter_open_post("/audio/transcriptions");
   if (!c) {
      free(body_str);
      log_health("/audio/transcriptions", "post");
      return ESP_ERR_INVALID_STATE;
   }
   esp_http_client_set_header(c, "Content-Type", "application/json");

   t_open = esp_timer_get_time();
   err = esp_http_client_open(c, body_len);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "/audio/transcriptions open failed: %s (errno=%d) after %lldms (attempt %d)", esp_err_to_name(err),
               errno, (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
      if (attempt == 1 && err == ESP_ERR_HTTP_CONNECT) {
         ESP_LOGW(TAG, "/audio/transcriptions transient connect failure — retry-once");
         esp_http_client_close(c);
         esp_http_client_cleanup(c);
         inflight_set(NULL);
         vTaskDelay(pdMS_TO_TICKS(500));
         goto retry;
      }
      goto cleanup;
   }
   err = write_full(c, body_str, body_len);
   if (err != ESP_OK) goto cleanup;

   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "stt status=%d (body_len=%u)", status, (unsigned)body_len);
      log_error_body(c, "/audio/transcriptions");
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
   inflight_set(NULL);
   free(body_str);
   ESP_LOGI(TAG, "/audio/transcriptions done rc=%s in %lldms (attempts=%d)", esp_err_to_name(err),
            (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
   log_health("/audio/transcriptions", "post");
   return err;
}

/* W3-B (TT #374): reuse one persistent parse buffer for the whole
 * stream instead of malloc+free per delta.  Sonnet's 1112-delta replies
 * meant 1112 PSRAM allocations in ~10 s, fragmenting the heap over
 * many turns.  parse_buf grows on demand inside this stream's lifetime
 * and is freed once when the stream ends. */
typedef struct {
   openrouter_chat_delta_cb_t cb;
   void *cb_ctx;
   char *parse_buf;
   size_t parse_cap;
} chat_stream_ctx_t;

static void chat_sse_cb(const char *json, size_t len, void *vctx) {
   chat_stream_ctx_t *c = vctx;
   if (c->parse_cap < len + 1) {
      /* Round up to next power-of-2 above (len+1), capped at 16 KB.
       * Typical OpenRouter SSE deltas are <1 KB; outliers up to ~8 KB. */
      size_t newcap = c->parse_cap ? c->parse_cap : 1024;
      while (newcap < len + 1 && newcap < 16 * 1024) newcap *= 2;
      if (newcap < len + 1) return; /* one chunk > 16 KB — drop */
      char *grown = heap_caps_realloc(c->parse_buf, newcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!grown) return;
      c->parse_buf = grown;
      c->parse_cap = newcap;
   }
   memcpy(c->parse_buf, json, len);
   c->parse_buf[len] = '\0';
   cJSON *root = cJSON_Parse(c->parse_buf);
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

   log_health("/chat/completions", "pre");

   /* W2-C (TT #373): auto-retry-once on transient ESP_ERR_HTTP_CONNECT.
    * After ~5 min of sustained activity the first TCP/TLS attempt
    * sometimes fails — likely a stale cached socket — but a fresh
    * handle on the second try succeeds.  Cap at 1 retry so a real
    * outage doesn't double-amplify the failure latency. */
   int attempt = 0;
   esp_err_t err = ESP_FAIL;
   esp_http_client_handle_t c = NULL;
   openrouter_sse_state_t *sse = NULL;
   chat_stream_ctx_t cctx = {.cb = cb, .cb_ctx = ctx, .parse_buf = NULL, .parse_cap = 0};
   size_t body_len = strlen(body_str);
   int64_t t_open = 0;
retry:
   attempt++;
   c = openrouter_open_post("/chat/completions");
   if (!c) {
      free(body_str);
      return ESP_ERR_INVALID_STATE;
   }
   esp_http_client_set_header(c, "Content-Type", "application/json");
   esp_http_client_set_header(c, "Accept", "text/event-stream");

   if (openrouter_sse_init(&sse, chat_sse_cb, &cctx) != ESP_OK) {
      free(body_str);
      esp_http_client_cleanup(c);
      inflight_set(NULL);
      return ESP_ERR_NO_MEM;
   }

   t_open = esp_timer_get_time();
   err = esp_http_client_open(c, body_len);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "/chat/completions open failed: %s (errno=%d) after %lldms (attempt %d)", esp_err_to_name(err),
               errno, (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
      if (attempt == 1 && err == ESP_ERR_HTTP_CONNECT) {
         ESP_LOGW(TAG, "/chat/completions transient connect failure — retry-once");
         openrouter_sse_free(sse);
         sse = NULL;
         esp_http_client_close(c);
         esp_http_client_cleanup(c);
         inflight_set(NULL);
         vTaskDelay(pdMS_TO_TICKS(500));
         goto retry;
      }
      goto cleanup;
   }
   err = write_full(c, body_str, body_len);
   if (err != ESP_OK) goto cleanup;
   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "chat status=%d (body_len=%u)", status, (unsigned)body_len);
      log_error_body(c, "/chat/completions");
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
   inflight_set(NULL);
   if (cctx.parse_buf) heap_caps_free(cctx.parse_buf); /* W3-B */
   free(body_str);
   ESP_LOGI(TAG, "/chat/completions done rc=%s in %lldms (attempts=%d)", esp_err_to_name(err),
            (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
   log_health("/chat/completions", "post");
   return err;
}

/* ── TT #379: multimodal audio chat ───────────────────────────────────── */

/* Per-stream context for chat_audio's SSE delta dispatcher.  parse_buf is
 * reused across deltas (mirrors chat_stream's W3-B) and decode_buf grows
 * on demand for the largest base64-PCM chunk in the stream. */
typedef struct {
   openrouter_tts_chunk_cb_t audio_cb;
   openrouter_chat_delta_cb_t text_cb;
   void *cb_ctx;
   char *parse_buf;
   size_t parse_cap;
   uint8_t *decode_buf;
   size_t decode_cap;
   size_t audio_chunks; /* DEBUG counters surfaced in the final log line. */
   size_t audio_bytes;
   size_t text_chunks;
   size_t text_chars;
} chat_audio_ctx_t;

/* Grow `parse_buf` (or `decode_buf`) in place to at least `need` bytes,
 * capped at 256 KB.  Returns ESP_OK on success.  Used by the SSE delta
 * dispatcher to bound the per-stream PSRAM footprint while avoiding
 * malloc/free churn on every delta. */
static esp_err_t grow_psram_buf(uint8_t **buf, size_t *cap, size_t need) {
   if (*cap >= need) return ESP_OK;
   size_t newcap = *cap ? *cap : 1024;
   while (newcap < need && newcap < 256 * 1024) newcap *= 2;
   if (newcap < need) return ESP_ERR_NO_MEM;
   uint8_t *grown = heap_caps_realloc(*buf, newcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!grown) return ESP_ERR_NO_MEM;
   *buf = grown;
   *cap = newcap;
   return ESP_OK;
}

static void chat_audio_sse_cb(const char *json, size_t len, void *vctx) {
   chat_audio_ctx_t *c = vctx;
   /* Stage the SSE event into parse_buf so cJSON_Parse sees a NUL-terminated
    * buffer; reuse across deltas (same pattern as chat_sse_cb). */
   if (grow_psram_buf((uint8_t **)&c->parse_buf, &c->parse_cap, len + 1) != ESP_OK) return;
   memcpy(c->parse_buf, json, len);
   c->parse_buf[len] = '\0';
   cJSON *root = cJSON_Parse(c->parse_buf);
   if (!root) return;
   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
      cJSON_Delete(root);
      return;
   }
   cJSON *delta = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta");
   cJSON *audio = delta ? cJSON_GetObjectItem(delta, "audio") : NULL;
   if (!audio) {
      cJSON_Delete(root);
      return;
   }
   /* Transcript text chunk → text_cb (assistant's "voice" rendered as text). */
   cJSON *transcript = cJSON_GetObjectItem(audio, "transcript");
   if (cJSON_IsString(transcript) && transcript->valuestring && transcript->valuestring[0]) {
      size_t tl = strlen(transcript->valuestring);
      c->text_chunks++;
      c->text_chars += tl;
      if (c->text_cb) c->text_cb(transcript->valuestring, tl, c->cb_ctx);
   }
   /* Audio chunk: base64-encoded PCM16 → audio_cb after decode. */
   cJSON *data = cJSON_GetObjectItem(audio, "data");
   if (cJSON_IsString(data) && data->valuestring) {
      const char *b64 = data->valuestring;
      size_t b64_len = strlen(b64);
      size_t need = (b64_len * 3) / 4 + 2;
      if (grow_psram_buf(&c->decode_buf, &c->decode_cap, need) == ESP_OK) {
         size_t out_len = 0;
         int rc = mbedtls_base64_decode(c->decode_buf, c->decode_cap, &out_len, (const unsigned char *)b64, b64_len);
         if (rc == 0 && out_len > 0) {
            c->audio_chunks++;
            c->audio_bytes += out_len;
            if (c->audio_cb) c->audio_cb(c->decode_buf, out_len, c->cb_ctx);
         }
      }
   }
   cJSON_Delete(root);
}

esp_err_t openrouter_chat_audio(const int16_t *pcm, size_t samples, openrouter_tts_chunk_cb_t audio_cb,
                                openrouter_chat_delta_cb_t text_cb, void *ctx) {
   if (!pcm || samples == 0 || (!audio_cb && !text_cb)) return ESP_ERR_INVALID_ARG;

   char model[96] = {0};
   tab5_settings_get_or_mdl_audio(model, sizeof model);
   char voice[32] = {0};
   tab5_settings_get_or_voice(voice, sizeof voice);

   const size_t pcm_bytes = samples * sizeof(int16_t);
   const size_t wav_bytes = 44 + pcm_bytes;

   /* WAV-wrap the mic PCM in PSRAM, then base64-encode it (the input_audio
    * field on a content[] block takes the same shape as /audio/transcriptions). */
   uint8_t *wav = heap_caps_malloc(wav_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!wav) return ESP_ERR_NO_MEM;
   wav16_header(wav, pcm_bytes);
   memcpy(wav + 44, pcm, pcm_bytes);

   const size_t b64_cap = ((wav_bytes + 2) / 3) * 4 + 1;
   char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!b64) {
      heap_caps_free(wav);
      return ESP_ERR_NO_MEM;
   }
   size_t b64_olen = 0;
   int mb = mbedtls_base64_encode((unsigned char *)b64, b64_cap, &b64_olen, wav, wav_bytes);
   heap_caps_free(wav);
   if (mb != 0) {
      heap_caps_free(b64);
      return ESP_FAIL;
   }
   b64[b64_olen] = '\0';

   /* Build the request body:
    *   { model, modalities, audio:{voice,format},
    *     stream:true,
    *     messages:[{role:"user", content:[{type:"input_audio", input_audio:{data,format}}]}] } */
   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "model", model);
   cJSON *modalities = cJSON_CreateArray();
   cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
   cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
   cJSON_AddItemToObject(body, "modalities", modalities);
   cJSON *audio_cfg = cJSON_CreateObject();
   cJSON_AddStringToObject(audio_cfg, "voice", voice);
   cJSON_AddStringToObject(audio_cfg, "format", "pcm16");
   cJSON_AddItemToObject(body, "audio", audio_cfg);
   cJSON_AddBoolToObject(body, "stream", true);
   cJSON *msgs = cJSON_CreateArray();
   cJSON *user_msg = cJSON_CreateObject();
   cJSON_AddStringToObject(user_msg, "role", "user");
   cJSON *content_arr = cJSON_CreateArray();
   cJSON *audio_block = cJSON_CreateObject();
   cJSON_AddStringToObject(audio_block, "type", "input_audio");
   cJSON *input_audio = cJSON_CreateObject();
   cJSON_AddStringToObject(input_audio, "data", b64);
   cJSON_AddStringToObject(input_audio, "format", "wav");
   cJSON_AddItemToObject(audio_block, "input_audio", input_audio);
   cJSON_AddItemToArray(content_arr, audio_block);
   cJSON_AddItemToObject(user_msg, "content", content_arr);
   cJSON_AddItemToArray(msgs, user_msg);
   cJSON_AddItemToObject(body, "messages", msgs);
   char *body_str = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   heap_caps_free(b64);
   if (!body_str) return ESP_ERR_NO_MEM;
   const size_t body_len = strlen(body_str);

   log_health("/chat/completions[audio]", "pre");

   int attempt = 0;
   esp_err_t err = ESP_FAIL;
   esp_http_client_handle_t c = NULL;
   openrouter_sse_state_t *sse = NULL;
   chat_audio_ctx_t actx = {
       .audio_cb = audio_cb,
       .text_cb = text_cb,
       .cb_ctx = ctx,
       .parse_buf = NULL,
       .parse_cap = 0,
       .decode_buf = NULL,
       .decode_cap = 0,
   };
   int64_t t_open = 0;
retry:
   attempt++;
   c = openrouter_open_post("/chat/completions");
   if (!c) {
      free(body_str);
      return ESP_ERR_INVALID_STATE;
   }
   esp_http_client_set_header(c, "Content-Type", "application/json");
   esp_http_client_set_header(c, "Accept", "text/event-stream");

   if (openrouter_sse_init(&sse, chat_audio_sse_cb, &actx) != ESP_OK) {
      free(body_str);
      esp_http_client_cleanup(c);
      inflight_set(NULL);
      return ESP_ERR_NO_MEM;
   }

   t_open = esp_timer_get_time();
   err = esp_http_client_open(c, body_len);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "/chat/completions[audio] open failed: %s (errno=%d) after %lldms (attempt %d)",
               esp_err_to_name(err), errno, (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
      if (attempt == 1 && err == ESP_ERR_HTTP_CONNECT) {
         ESP_LOGW(TAG, "/chat/completions[audio] transient connect failure — retry-once");
         openrouter_sse_free(sse);
         sse = NULL;
         esp_http_client_close(c);
         esp_http_client_cleanup(c);
         inflight_set(NULL);
         vTaskDelay(pdMS_TO_TICKS(500));
         goto retry;
      }
      goto cleanup;
   }
   err = write_full(c, body_str, body_len);
   if (err != ESP_OK) goto cleanup;
   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "chat_audio status=%d (body_len=%u)", status, (unsigned)body_len);
      log_error_body(c, "/chat/completions[audio]");
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
   inflight_set(NULL);
   if (actx.parse_buf) heap_caps_free(actx.parse_buf);
   if (actx.decode_buf) heap_caps_free(actx.decode_buf);
   free(body_str);
   ESP_LOGI(TAG,
            "/chat/completions[audio] done rc=%s in %lldms (attempts=%d) "
            "audio_chunks=%u audio_bytes=%u text_chunks=%u text_chars=%u",
            esp_err_to_name(err), (long long)((esp_timer_get_time() - t_open) / 1000), attempt,
            (unsigned)actx.audio_chunks, (unsigned)actx.audio_bytes, (unsigned)actx.text_chunks,
            (unsigned)actx.text_chars);
   log_health("/chat/completions[audio]", "post");
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
   /* TT #377: OpenRouter's /audio/speech Zod schema rejects "wav" — only
    * "mp3" or "pcm" are accepted.  We pick pcm (raw 24 kHz mono int16-LE,
    * no container) and skip the WAV-header strip below.  Note: at the
    * time of writing OpenRouter has NO working TTS model anyway — every
    * candidate (openai/tts-1, openai/gpt-audio*, openai/gpt-4o-audio-preview)
    * returns "Model does not exist" on this endpoint.  Code is shape-correct
    * for whenever they add one; tracked separately. */
   cJSON_AddStringToObject(body, "response_format", "pcm");
   char *body_str = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!body_str) return ESP_ERR_NO_MEM;

   log_health("/audio/speech", "pre"); /* W4-C (TT #375) */

   /* W4-C (TT #375): retry-once-on-ESP_ERR_HTTP_CONNECT pattern mirrors
    * chat_stream / embed.  TTS body is a small JSON blob — re-writing
    * it on retry is essentially free. */
   int attempt = 0;
   esp_http_client_handle_t c = NULL;
   esp_err_t err = ESP_FAIL;
   size_t body_len = strlen(body_str);
   int64_t t_open = 0;
retry:
   attempt++;
   c = openrouter_open_post("/audio/speech");
   if (!c) {
      free(body_str);
      log_health("/audio/speech", "post");
      return ESP_ERR_INVALID_STATE;
   }
   esp_http_client_set_header(c, "Content-Type", "application/json");

   t_open = esp_timer_get_time();
   err = esp_http_client_open(c, body_len);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "/audio/speech open failed: %s (errno=%d) after %lldms (attempt %d)", esp_err_to_name(err), errno,
               (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
      if (attempt == 1 && err == ESP_ERR_HTTP_CONNECT) {
         ESP_LOGW(TAG, "/audio/speech transient connect failure — retry-once");
         esp_http_client_close(c);
         esp_http_client_cleanup(c);
         inflight_set(NULL);
         vTaskDelay(pdMS_TO_TICKS(500));
         goto retry;
      }
      goto cleanup;
   }
   err = write_full(c, body_str, body_len);
   if (err != ESP_OK) goto cleanup;
   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "tts status=%d (body_len=%u)", status, (unsigned)body_len);
      log_error_body(c, "/audio/speech");
      err = ESP_FAIL;
      goto cleanup;
   }

   /* TT #377: response_format=pcm returns raw PCM-16LE with no container,
    * so we no longer strip a 44-byte WAV RIFF header here. */

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
   inflight_set(NULL);
   free(body_str);
   ESP_LOGI(TAG, "/audio/speech done rc=%s in %lldms (attempts=%d)", esp_err_to_name(err),
            (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
   log_health("/audio/speech", "post"); /* W4-C (TT #375) */
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

   log_health("/embeddings", "pre");

   /* W2-C (TT #373): same retry-once pattern as chat_stream. */
   int attempt = 0;
   esp_http_client_handle_t c = NULL;
   esp_err_t err = ESP_FAIL;
   size_t body_len = strlen(body_str);
   int64_t t_open = 0;
retry:
   attempt++;
   c = openrouter_open_post("/embeddings");
   if (!c) {
      free(body_str);
      return ESP_ERR_INVALID_STATE;
   }
   esp_http_client_set_header(c, "Content-Type", "application/json");

   t_open = esp_timer_get_time();
   err = esp_http_client_open(c, body_len);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "/embeddings open failed: %s (errno=%d) after %lldms (attempt %d)", esp_err_to_name(err), errno,
               (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
      if (attempt == 1 && err == ESP_ERR_HTTP_CONNECT) {
         ESP_LOGW(TAG, "/embeddings transient connect failure — retry-once");
         esp_http_client_close(c);
         esp_http_client_cleanup(c);
         inflight_set(NULL);
         vTaskDelay(pdMS_TO_TICKS(500));
         goto retry;
      }
      goto cleanup;
   }
   err = write_full(c, body_str, body_len);
   if (err != ESP_OK) goto cleanup;
   esp_http_client_fetch_headers(c);
   int status = esp_http_client_get_status_code(c);
   if (status != 200) {
      ESP_LOGE(TAG, "embed status=%d (body_len=%u)", status, (unsigned)body_len);
      log_error_body(c, "/embeddings");
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
         float *v = heap_caps_malloc((size_t)dim * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
   inflight_set(NULL);
   free(body_str);
   ESP_LOGI(TAG, "/embeddings done rc=%s in %lldms (attempts=%d)", esp_err_to_name(err),
            (long long)((esp_timer_get_time() - t_open) / 1000), attempt);
   log_health("/embeddings", "post");
   return err;
}
