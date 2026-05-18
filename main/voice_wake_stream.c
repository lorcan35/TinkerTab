/**
 * @file voice_wake_stream.c
 * @brief Tab5 mic → Dragon wakeword streamer (TT #615 Path B).
 *
 * Background task that captures the same downsampled 16 kHz mono PCM
 * stream the existing mic_task produces for voice turns, and ships it
 * to Dragon as WAK0-tagged binary frames whenever:
 *   - The task is armed (voice_wake_stream_arm)
 *   - The Dragon WS is connected
 *   - voice state is quiescent (IDLE or READY)
 *
 * Single I2S RX consumer at a time: when voice state transitions to
 * LISTENING/PROCESSING/SPEAKING/RECONNECTING the in-turn mic_task is
 * the owner; the wake-stream task is gated off via the state check
 * (no I2S reads, no WS sends).  On SPEAKING→READY the in-turn mic
 * task has already disabled itself, so wake-stream resumes naturally.
 *
 * Bandwidth: 16 kHz × 16-bit × continuous = 256 kbps over the existing
 * voice WS.  Header overhead 8 B (WAK0 + BE length) per ~30 ms chunk.
 *
 * Dragon side handles WAK0 frames: sliding window into whisper.cpp,
 * substring match against "hey tinker" / "hey thinker", then sends
 * {"type":"wake"} back; the WS RX handler in voice_ws_proto routes
 * to ui_home_start_voice_turn("dragon_wake").
 */

#include "voice_wake_stream.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "audio.h" /* tab5_mic_read, TAB5_AUDIO_SAMPLE_RATE */
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice.h"
#include "voice_codec.h"

/* Mirror constants from voice.c.  Kept local; the canonical defs live
 * with mic_task and aren't header-exposed.  If they drift, both sites
 * need the same change. */
#define WS_MIC_48K_FRAMES (TAB5_AUDIO_SAMPLE_RATE * TAB5_VOICE_CHUNK_MS / 1000)
#define WS_MIC_TDM_CHANNELS 4
#define WS_MIC_TDM_MIC1_OFF 0
#define WS_MIC_TDM_SAMPLES (WS_MIC_48K_FRAMES * WS_MIC_TDM_CHANNELS)
#define WS_DOWNSAMPLE_RATIO (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)

extern esp_websocket_client_handle_t volatile g_voice_ws;
extern esp_err_t voice_ws_send_binary(const uint8_t *data, size_t len);
extern void tab5_debug_obs_event(const char *kind, const char *detail);
extern bool voice_mic_is_active(void);

static const char *TAG = "voice_wake_stream";

#define WAKE_STREAM_TASK_STACK 8192
#define WAKE_STREAM_TASK_PRIO 4
#define WAKE_STREAM_TASK_CORE 1

/* Matches mic_task's downsample ratio (48 kHz TDM → 16 kHz mono).  Pull
 * the same constants from config.h. */
#define WS_CHUNK_SAMPLES (TAB5_VOICE_SAMPLE_RATE * TAB5_VOICE_CHUNK_MS / 1000)
#define WS_CHUNK_BYTES (WS_CHUNK_SAMPLES * sizeof(int16_t))

static TaskHandle_t s_task = NULL;
static volatile bool s_armed = false;
static volatile int s_voice_state = 0; /* mirror of voice_state_t — checked each iteration */
static int64_t s_last_send_log_us = 0;

static bool quiescent_state(int st) {
   /* IDLE=0, CONNECTING=1, READY=2, LISTENING=3, PROCESSING=4, SPEAKING=5, RECONNECTING=6.
    * Only stream when READY.  IDLE means voice WS isn't up; CONNECTING /
    * RECONNECTING are transient — WS may flap, no point streaming.
    * LISTENING/PROCESSING/SPEAKING: mic_task owns I2S. */
   return (st == 2);
}

static void wake_stream_task(void *arg) {
   (void)arg;

   const int WS_MIC_TDM_SAMPLES_LOCAL = WS_MIC_48K_FRAMES * WS_MIC_TDM_CHANNELS;
   int16_t *tdm_buf =
       heap_caps_malloc(WS_MIC_TDM_SAMPLES_LOCAL * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   int16_t *mono_buf = heap_caps_malloc(WS_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   uint8_t *wire_buf = heap_caps_malloc(WS_CHUNK_BYTES + VOICE_WAKE_AUDIO_HEADER_LEN,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!tdm_buf || !mono_buf || !wire_buf) {
      ESP_LOGE(TAG, "buffer alloc failed; wake-stream disabled");
      if (tdm_buf) heap_caps_free(tdm_buf);
      if (mono_buf) heap_caps_free(mono_buf);
      if (wire_buf) heap_caps_free(wire_buf);
      s_task = NULL;
      vTaskSuspend(NULL);
      return;
   }

   ESP_LOGI(TAG, "wake-stream task started (chunk=%d samples)", WS_CHUNK_SAMPLES);

   /* Boot grace — give voice_init + mic_task + audio HW a full second
    * to settle before we even consider touching I2S.  Avoids racing the
    * mic_task's I2S enable on the first voice turn after boot. */
   vTaskDelay(pdMS_TO_TICKS(1500));

   uint32_t frames_sent = 0;
   while (1) {
      bool ws_live = (g_voice_ws != NULL) && esp_websocket_client_is_connected(g_voice_ws);
      bool can_pump = s_armed && ws_live && quiescent_state(s_voice_state);

      /* CRITICAL — never touch I2S while mic_task is using it.  The
       * I2S RX driver is not reentrant; two readers crash the kernel
       * (Tab5 PANIC, reset_reason=PANIC). */
      if (can_pump && voice_mic_is_active()) {
         can_pump = false;
      }

      if (!can_pump) {
         vTaskDelay(pdMS_TO_TICKS(100));
         continue;
      }

      esp_err_t err = tab5_mic_read(tdm_buf, WS_MIC_TDM_SAMPLES_LOCAL, 100);
      if (err != ESP_OK) {
         /* I2S not enabled yet (boot order), or transient — back off. */
         vTaskDelay(pdMS_TO_TICKS(100));
         continue;
      }

      /* Downsample identical to mic_task: take MIC1 slot, average
       * WS_DOWNSAMPLE_RATIO consecutive samples for the 16 kHz output. */
      int out_idx = 0;
      for (int i = 0; i + WS_DOWNSAMPLE_RATIO - 1 < WS_MIC_48K_FRAMES && out_idx < WS_CHUNK_SAMPLES;
           i += WS_DOWNSAMPLE_RATIO) {
         int32_t sum = 0;
         for (int j = 0; j < WS_DOWNSAMPLE_RATIO; j++) {
            sum += tdm_buf[(i + j) * WS_MIC_TDM_CHANNELS + WS_MIC_TDM_MIC1_OFF];
         }
         mono_buf[out_idx++] = (int16_t)(sum / WS_DOWNSAMPLE_RATIO);
      }
      if (out_idx == 0) {
         vTaskDelay(pdMS_TO_TICKS(10));
         continue;
      }

      /* Re-check state after the I2S read (which can block ~30 ms).  If
       * voice transitioned into a turn in the meantime, drop the chunk
       * — mic_task is now the owner.  Also re-check voice_mic_is_active
       * to catch the race where mic_task spun up between our pre-check
       * and the actual read. */
      if (!quiescent_state(s_voice_state) || voice_mic_is_active()) {
         continue;
      }

      size_t body_bytes = (size_t)out_idx * sizeof(int16_t);
      size_t wire_len = voice_codec_pack_wake_audio(wire_buf, WS_CHUNK_BYTES + VOICE_WAKE_AUDIO_HEADER_LEN, mono_buf,
                                                    body_bytes);
      if (wire_len > 0) {
         err = voice_ws_send_binary(wire_buf, wire_len);
         if (err == ESP_OK) {
            frames_sent++;
            int64_t now = esp_timer_get_time();
            if (now - s_last_send_log_us > 5 * 1000000) {
               ESP_LOGI(TAG, "wake-stream pumped %lu frames (last chunk %d samples)", (unsigned long)frames_sent,
                        out_idx);
               s_last_send_log_us = now;
            }
         } else {
            /* WS send failed — likely WS dropped between our ws_live
             * check and now.  Back off a bit then loop, the gating
             * will catch the disconnect on the next iteration. */
            vTaskDelay(pdMS_TO_TICKS(100));
         }
      }
   }
   /* unreachable */
}

esp_err_t voice_wake_stream_init(void) {
   if (s_task != NULL) return ESP_OK;
   BaseType_t ok = xTaskCreatePinnedToCore(wake_stream_task, "voice_wake_stream", WAKE_STREAM_TASK_STACK, NULL,
                                           WAKE_STREAM_TASK_PRIO, &s_task, WAKE_STREAM_TASK_CORE);
   if (ok != pdPASS) {
      ESP_LOGE(TAG, "wake_stream_task spawn failed");
      s_task = NULL;
      return ESP_ERR_NO_MEM;
   }
   ESP_LOGI(TAG, "init done (disarmed)");
   return ESP_OK;
}

void voice_wake_stream_arm(void) {
   if (s_armed) return;
   s_armed = true;
   tab5_debug_obs_event("wake_stream", "arm");
   ESP_LOGI(TAG, "armed");
}

void voice_wake_stream_disarm(void) {
   if (!s_armed) return;
   s_armed = false;
   tab5_debug_obs_event("wake_stream", "disarm");
   ESP_LOGI(TAG, "disarmed");
}

bool voice_wake_stream_is_active(void) {
   if (!s_armed || s_task == NULL) return false;
   if (!quiescent_state(s_voice_state)) return false;
   return (g_voice_ws != NULL) && esp_websocket_client_is_connected(g_voice_ws);
}

void voice_wake_stream_on_state_change(int new_state) {
   s_voice_state = new_state;
}
