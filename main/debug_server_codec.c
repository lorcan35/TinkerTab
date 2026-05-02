/*
 * debug_server_codec.c — codec stack debug HTTP endpoint family.
 *
 * Wave 23b follow-up (#332): third per-family extract from
 * debug_server.c (after K144 #336 and OTA #337).  The opus_test
 * endpoint was added in TT #264 (Wave 19) to bisect the SILK NSQ
 * stack-overflow crash; bisect found 24 KB as the watermark, leading
 * to the MIC_TASK_STACK_SIZE bump in voice.c.  The endpoint stays as
 * a permanent diagnostic for codec health checks + stack-budget
 * validation.
 *
 * Mechanical move from debug_server.c — handlers + the worker task +
 * the opus_test_ctx_t typedef all moved verbatim.  Only call-site
 * changes: `check_auth(req)` → `tab5_debug_check_auth(req)` and
 * `send_json_resp(req, r)` → `tab5_debug_send_json_resp(req, r)`,
 * both via the shared internal-header wrappers.
 */

#include "debug_server_codec.h"

#include <math.h>   /* TT #264 sin() for OPUS encoder synthetic test */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "debug_server_internal.h" /* tab5_debug_check_auth + send_json_resp */
#include "voice_codec.h"           /* voice_codec_encode_uplink + set_uplink */

/* TT #264 — synthetic OPUS encoder smoke test.  Drives
 * voice_codec_encode_uplink directly with N frames of synthetic
 * 16 kHz mono PCM (sine wave at 440 Hz, low amplitude).  Bypasses
 * the WS/mic pipeline so we can isolate "is the SILK NSQ encoder
 * working?" without coordinating Dragon's codec choice or a live mic.
 *
 * Runs the encode loop in a one-shot worker task with a 48 KB stack
 * (the issue reported 16 KB on voice_mic still triggers the SILK NSQ
 * panic — this is investigation step "expand stack until it works").
 *
 * Query params: ?frames=N (1-1000), ?heap=psram|internal|dma,
 * ?align=16|32|64, ?stack=N (KB, default 48).
 *
 * Returns counters: frames_attempted, frames_ok, frames_failed,
 * total_bytes_out.  If Tab5 panics mid-test the response never
 * lands; check uptime + reset_reason via /info to confirm. */
typedef struct {
   const int16_t *pcm;
   size_t n_samples;
   int frames;
   int frames_ok;
   int frames_failed;
   uint32_t total_bytes_out;
   int64_t dt_us;
   SemaphoreHandle_t done;
} opus_test_ctx_t;

static void opus_test_task(void *arg) {
   opus_test_ctx_t *ctx = (opus_test_ctx_t *)arg;
   uint8_t out[1024];
   int64_t t0 = esp_timer_get_time();
   for (int i = 0; i < ctx->frames; i++) {
      size_t out_len = 0;
      esp_err_t e = voice_codec_encode_uplink(ctx->pcm, ctx->n_samples, out, sizeof(out), &out_len);
      if (e == ESP_OK && out_len > 0) {
         ctx->frames_ok++;
         ctx->total_bytes_out += out_len;
      } else {
         ctx->frames_failed++;
      }
   }
   ctx->dt_us = esp_timer_get_time() - t0;
   xSemaphoreGive(ctx->done);
   vTaskDelete(NULL);
}

static esp_err_t debug_opus_test_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   /* Frame count: 50 frames = 1 second at 20 ms / frame.  Caller can
    * override via ?frames=N (capped at 1000 to avoid DoS-ing a busy
    * mic loop). */
   int frames = 50;
   {
      char qbuf[64];
      if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
         char val[16];
         if (httpd_query_key_value(qbuf, "frames", val, sizeof(val)) == ESP_OK) {
            int n = atoi(val);
            if (n > 0 && n <= 1000) frames = n;
         }
      }
   }

   /* Allocate a 320-sample (20 ms @ 16 kHz) PCM frame.  The buffer
    * heap_caps + alignment are part of what we're testing — issue
    * #264 suspects the SILK encoder requires DMA-capable internal
    * SRAM and/or 32/64-byte alignment.  Caller can override:
    *   ?heap=psram  (default: same as the real mic buffer)
    *   ?heap=internal  (try MALLOC_CAP_INTERNAL — investigation step 2)
    *   ?align=N  (use heap_caps_aligned_alloc — step 1)
    */
   const size_t n_samples = 320;
   int16_t *pcm = NULL;
   uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
   int align = 0;
   {
      char qbuf[64];
      if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
         char val[16];
         if (httpd_query_key_value(qbuf, "heap", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "internal") == 0) {
               caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            } else if (strcmp(val, "dma") == 0) {
               caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            }
         }
         if (httpd_query_key_value(qbuf, "align", val, sizeof(val)) == ESP_OK) {
            int a = atoi(val);
            if (a == 16 || a == 32 || a == 64) align = a;
         }
      }
   }
   if (align > 0) {
      pcm = heap_caps_aligned_alloc((size_t)align, n_samples * sizeof(int16_t), caps);
   } else {
      pcm = heap_caps_malloc(n_samples * sizeof(int16_t), caps);
   }
   if (pcm == NULL) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "PSRAM alloc failed");
      return tab5_debug_send_json_resp(req, r);
   }
   /* Sine wave: 16 kHz sample rate, 440 Hz target.  Amplitude
    * 0.1 × INT16_MAX = ~3276 (low to keep VBR honest). */
   for (size_t i = 0; i < n_samples; i++) {
      double t = (double)i / 16000.0;
      double s = sin(2.0 * 3.14159265 * 440.0 * t);
      pcm[i] = (int16_t)(s * 3276.0);
   }

   /* Force OPUS uplink — encoder spins up here on first call (will
    * fall back to PCM silently if init fails). */
   voice_codec_set_uplink(VOICE_CODEC_OPUS);
   bool encoder_active = (voice_codec_get_uplink() == VOICE_CODEC_OPUS);

   /* Pick the encode-task stack size from query (default 48 KB).
    * Issue #264 reported voice_mic's 16 KB stack triggers SILK NSQ
    * stack-protection-fault panic; this lets us bisect the minimum
    * size that survives.  Cap at 96 KB to keep the alloc bounded. */
   int stack_kb = 48;
   {
      char qbuf[64];
      if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
         char val[16];
         if (httpd_query_key_value(qbuf, "stack", val, sizeof(val)) == ESP_OK) {
            int s = atoi(val);
            if (s >= 8 && s <= 96) stack_kb = s;
         }
      }
   }

   /* Run the encode loop in a dedicated task with the requested
    * stack — httpd's task stack is too small (4-8 KB depending on
    * config) for SILK NSQ working storage.  Wait synchronously via
    * semaphore. */
   opus_test_ctx_t ctx = {
       .pcm = pcm,
       .n_samples = n_samples,
       .frames = frames,
       .done = xSemaphoreCreateBinary(),
   };
   if (ctx.done == NULL) {
      heap_caps_free(pcm);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "semaphore alloc failed");
      return tab5_debug_send_json_resp(req, r);
   }
   BaseType_t tc = xTaskCreate(opus_test_task, "opus_test", (uint32_t)stack_kb * 1024, &ctx, 5, NULL);
   if (tc != pdPASS) {
      vSemaphoreDelete(ctx.done);
      heap_caps_free(pcm);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "xTaskCreate failed");
      cJSON_AddNumberToObject(r, "stack_kb", stack_kb);
      return tab5_debug_send_json_resp(req, r);
   }
   /* Wait up to 10 s for the task to finish.  At ~1 ms per frame
    * even 1000 frames is well under this budget; if the task panics
    * or hangs we'll fail-soft after 10 s with no payload to return. */
   xSemaphoreTake(ctx.done, pdMS_TO_TICKS(10000));
   vSemaphoreDelete(ctx.done);
   int frames_ok = ctx.frames_ok;
   int frames_failed = ctx.frames_failed;
   uint32_t total_bytes_out = ctx.total_bytes_out;
   int64_t dt_us = ctx.dt_us;
   heap_caps_free(pcm);

   cJSON *r = cJSON_CreateObject();
   cJSON_AddBoolToObject(r, "encoder_active", encoder_active);
   cJSON_AddStringToObject(r, "buf_heap", (caps & MALLOC_CAP_INTERNAL) ? "internal" : "psram");
   cJSON_AddNumberToObject(r, "buf_align", align);
   cJSON_AddNumberToObject(r, "task_stack_kb", stack_kb);
   cJSON_AddNumberToObject(r, "frames_attempted", frames);
   cJSON_AddNumberToObject(r, "frames_ok", frames_ok);
   cJSON_AddNumberToObject(r, "frames_failed", frames_failed);
   cJSON_AddNumberToObject(r, "total_bytes_out", (double)total_bytes_out);
   cJSON_AddNumberToObject(r, "elapsed_ms", (double)(dt_us / 1000));
   if (frames > 0) {
      cJSON_AddNumberToObject(r, "avg_bytes_per_frame", (double)total_bytes_out / (double)frames);
      cJSON_AddNumberToObject(r, "us_per_frame", (double)dt_us / (double)frames);
   }
   /* Restore PCM uplink so subsequent real WS traffic isn't
    * unexpectedly OPUS-encoded. */
   voice_codec_set_uplink(VOICE_CODEC_PCM);
   return tab5_debug_send_json_resp(req, r);
}

void debug_server_codec_register(httpd_handle_t server) {
   const httpd_uri_t uri_opus_test = {
       .uri = "/codec/opus_test", .method = HTTP_POST, .handler = debug_opus_test_handler};
   httpd_register_uri_handler(server, &uri_opus_test);
   ESP_LOGI("debug_codec", "Codec endpoint family registered (1 URI)");
}
