/**
 * @file voice_ext_pcm_stream.c
 * @brief Tab5 mic → K144 ext_pcm streamer (TT #131 — Path A production).
 *
 * Background task that:
 *   1. On arm, issues the bring-up handshake against the K144 daemon:
 *        asr.setup input=sys.pcm    (audio's lazy _cap steals our PUB)
 *        audio.cap_stop_all         (audio releases bind)
 *        ext_pcm.rebind             (ext_pcm reclaims /tmp/llm/pcm.cap.socket)
 *   2. Then on each ~100 ms iteration:
 *        - read 4800 TDM frames @ 48 kHz × 4 channels
 *        - downsample MIC1 slot to 16 kHz mono int16 (3200 bytes)
 *        - base64-encode (~4267 chars)
 *        - wrap as ingest JSON: {"work_id":"ext_pcm","action":"ingest",
 *                                "data":"<base64>"}
 *        - send-and-fire over Port C UART
 *
 *   3. On disarm: send asr.exit + audio.cap_start to restore K144's
 *      onboard mic mode for next session.
 *
 * Mic ownership: same rules as voice_wake_stream — only ONE I2S RX
 * consumer at a time.  Gated on voice_mic_is_active() + voice state
 * being READY (==2).  Boot grace 1.5 s.
 */

#include "voice_ext_pcm_stream.h"

#include <stdint.h>
#include <string.h>

#include "audio.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "m5_stackflow.h"
#include "uart_port_c.h"
#include "voice_m5_llm.h"
#include "voice_onboard.h"
#include "voice_wakeword.h"

#define TAG "voice_ext_pcm_stream"

#define WS_MIC_48K_FRAMES (TAB5_AUDIO_SAMPLE_RATE * TAB5_VOICE_CHUNK_MS / 1000)
#define WS_MIC_TDM_CHANNELS 4
#define WS_MIC_TDM_MIC1_OFF 0
#define WS_DOWNSAMPLE_RATIO (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)
#define WS_CHUNK_SAMPLES (TAB5_VOICE_SAMPLE_RATE * TAB5_VOICE_CHUNK_MS / 1000)
#define WS_CHUNK_BYTES (WS_CHUNK_SAMPLES * sizeof(int16_t))

/* Batch N mic chunks per ingest RPC.  K144's UART JSON parser is
 * single-threaded; flooding it at 50 RPCs/sec (one per 20 ms chunk)
 * caused frame drops + remote_call queue blow-up.  Batching 5 chunks =
 * 100 ms of audio per RPC = 10 RPCs/sec is comfortable for both sides. */
#define INGEST_BATCH_CHUNKS 5
#define INGEST_RAW_BYTES (WS_CHUNK_BYTES * INGEST_BATCH_CHUNKS)
#define INGEST_B64_CAP (((INGEST_RAW_BYTES + 2) / 3) * 4 + 4)
#define INGEST_TX_CAP (INGEST_B64_CAP + 128)

#define EXT_PCM_TASK_STACK 8192
/* PRIO 5 = one above voice_wakeword (4).  When wakeword's recv loop
 * yields between iterations, we preempt and claim the UART lock first. */
#define EXT_PCM_TASK_PRIO 5
#define EXT_PCM_TASK_CORE 1

extern bool voice_mic_is_active(void);
extern void tab5_debug_obs_event(const char *kind, const char *detail);

static TaskHandle_t s_task = NULL;
static volatile bool s_armed = false;
/* handshake state retained as dead-but-harmless flag — drop in next pass */
static volatile int s_voice_state = 0;
static uint32_t s_frame_seq = 0;
static int64_t s_last_log_us = 0;

static bool quiescent_state(int st) {
   /* READY = 2 — same gate voice_wake_stream uses. */
   return (st == 2);
}

/* TT #131 — new architecture: ASR was configured with input=["asr"]
 * by voice_wakeword (Tab5-mic variant), so it subscribes to its OWN
 * inference bus.  We push PCM as inference RPC frames directly to the
 * asr work_id.  No audio.setup, no ext_pcm rebind, no PUB contention —
 * fire-and-forget through llm_sys's remote_call. */

static void ext_pcm_task(void *arg) {
   (void)arg;

   const int tdm_samples = WS_MIC_48K_FRAMES * WS_MIC_TDM_CHANNELS;
   int16_t *tdm_buf =
       heap_caps_malloc(tdm_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   int16_t *mono_buf = heap_caps_malloc(WS_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   /* batch_buf accumulates INGEST_BATCH_CHUNKS×WS_CHUNK_BYTES raw PCM
    * before a single base64 + ingest send.  Reduces UART RPC rate from
    * 50 fps to 10 fps. */
   int16_t *batch_buf = heap_caps_malloc(INGEST_RAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   int batch_chunks = 0;
   char *b64_buf = heap_caps_malloc(INGEST_B64_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   char *tx_buf = heap_caps_malloc(INGEST_TX_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!tdm_buf || !mono_buf || !batch_buf || !b64_buf || !tx_buf) {
      ESP_LOGE(TAG, "buffer alloc failed; ext_pcm stream disabled");
      goto cleanup;
   }

   ESP_LOGI(TAG, "task started (chunk=%d samples, b64_cap=%d)", WS_CHUNK_SAMPLES, INGEST_B64_CAP);
   vTaskDelay(pdMS_TO_TICKS(1500)); /* boot grace */

   while (1) {
      if (!s_armed || !quiescent_state(s_voice_state) || voice_mic_is_active()) {
         vTaskDelay(pdMS_TO_TICKS(100));
         continue;
      }

      /* Gate on voice_wakeword being armed.  That's the only true
       * prerequisite — wakeword's setup arms audio + asr on K144 (NOT
       * llm), so failover_state (which gates on llm.setup) is irrelevant
       * here.  If wakeword is up, asr is up, audio is up; we can claim
       * the PCM PUB URL. */
      if (!voice_wakeword_is_active()) {
         vTaskDelay(pdMS_TO_TICKS(500));
         continue;
      }

      /* No handshake needed — asr was set up with input=["asr"] so it
       * subscribes to its own inference bus.  We just send inference
       * RPCs to the wakeword's asr work_id. */
      const char *asr_id = voice_wakeword_asr_id();
      if (asr_id == NULL || asr_id[0] == '\0') {
         vTaskDelay(pdMS_TO_TICKS(500));
         continue;
      }

      esp_err_t err = tab5_mic_read(tdm_buf, tdm_samples, 100);
      if (err != ESP_OK) {
         vTaskDelay(pdMS_TO_TICKS(100));
         continue;
      }

      int out_idx = 0;
      for (int i = 0; i + WS_DOWNSAMPLE_RATIO - 1 < WS_MIC_48K_FRAMES && out_idx < WS_CHUNK_SAMPLES;
           i += WS_DOWNSAMPLE_RATIO) {
         int32_t sum = 0;
         for (int j = 0; j < WS_DOWNSAMPLE_RATIO; j++) {
            sum += tdm_buf[(i + j) * WS_MIC_TDM_CHANNELS + WS_MIC_TDM_MIC1_OFF];
         }
         mono_buf[out_idx++] = (int16_t)(sum / WS_DOWNSAMPLE_RATIO);
      }
      if (out_idx == 0) continue;

      /* Re-check state after the I2S read in case mic_task spun up. */
      if (!quiescent_state(s_voice_state) || voice_mic_is_active()) continue;

      /* Append this chunk to the batch.  Send only when full. */
      memcpy((uint8_t *)batch_buf + batch_chunks * WS_CHUNK_BYTES, mono_buf,
             (size_t)out_idx * sizeof(int16_t));
      batch_chunks++;
      if (batch_chunks < INGEST_BATCH_CHUNKS) continue;
      batch_chunks = 0;

      size_t b64_len = 0;
      int b_err = mbedtls_base64_encode((unsigned char *)b64_buf, INGEST_B64_CAP, &b64_len,
                                        (const unsigned char *)batch_buf, INGEST_RAW_BYTES);
      if (b_err != 0 || b64_len == 0) {
         ESP_LOGW(TAG, "base64 encode failed: %d", b_err);
         continue;
      }
      b64_buf[b64_len] = '\0';

      char rid[24];
      snprintf(rid, sizeof(rid), "ep%lu", (unsigned long)(++s_frame_seq));
      /* Re-read asr_id in case wakeword was re-armed; harmless if stale. */
      const char *target = voice_wakeword_asr_id();
      if (target == NULL || target[0] == '\0') continue;
      m5_stackflow_request_t req = {
          .request_id = rid,
          .work_id = target, /* asr.NNNN */
          .action = "inference",
          .object = "audio.pcm.base64",
          .data_string = b64_buf,
      };
      int tx_len = m5_stackflow_build_request(&req, tx_buf, INGEST_TX_CAP);
      if (tx_len < 0) {
         ESP_LOGW(TAG, "build_request failed (out_idx=%d, b64_len=%u)", out_idx, (unsigned)b64_len);
         continue;
      }

      /* Per-frame UART lock.  Hold time ~= tx_len/baud → ~30 ms at
       * 1.5 Mbps for a 4.4 KB frame.  Voice_m5_llm calls (when active)
       * will see brief contention but won't starve. */
      if (tab5_port_c_lock(200) != ESP_OK) {
         /* contention — drop this frame, the next one will retry. */
         continue;
      }
      int sent = tab5_port_c_send(tx_buf, (size_t)tx_len);
      tab5_port_c_unlock();
      if (sent != tx_len) continue;

      int64_t now = esp_timer_get_time();
      if (now - s_last_log_us > 5 * 1000000) {
         ESP_LOGI(TAG, "ext_pcm pumped seq=%lu (last %d bytes tx)", (unsigned long)s_frame_seq, tx_len);
         s_last_log_us = now;
      }
   }

cleanup:
   if (tdm_buf) heap_caps_free(tdm_buf);
   if (mono_buf) heap_caps_free(mono_buf);
   if (batch_buf) heap_caps_free(batch_buf);
   if (b64_buf) heap_caps_free(b64_buf);
   if (tx_buf) heap_caps_free(tx_buf);
   s_task = NULL;
   vTaskSuspend(NULL);
}

esp_err_t voice_ext_pcm_stream_init(void) {
   if (s_task != NULL) return ESP_OK;
   BaseType_t ok = xTaskCreatePinnedToCore(ext_pcm_task, "voice_ext_pcm", EXT_PCM_TASK_STACK, NULL,
                                           EXT_PCM_TASK_PRIO, &s_task, EXT_PCM_TASK_CORE);
   if (ok != pdPASS) {
      ESP_LOGE(TAG, "task spawn failed");
      s_task = NULL;
      return ESP_ERR_NO_MEM;
   }
   ESP_LOGI(TAG, "init done (disarmed)");
   return ESP_OK;
}

void voice_ext_pcm_stream_arm(void) {
   if (s_armed) return;
   s_armed = true;
   tab5_debug_obs_event("ext_pcm_stream", "arm");
   ESP_LOGI(TAG, "armed");
}

void voice_ext_pcm_stream_disarm(void) {
   if (!s_armed) return;
   s_armed = false;
   tab5_debug_obs_event("ext_pcm_stream", "disarm");
   ESP_LOGI(TAG, "disarmed");
}

bool voice_ext_pcm_stream_is_active(void) {
   return s_armed && s_task != NULL && quiescent_state(s_voice_state);
}

void voice_ext_pcm_stream_on_state_change(int new_state) {
   s_voice_state = new_state;
}
