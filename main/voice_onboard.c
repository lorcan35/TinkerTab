/**
 * @file voice_onboard.c
 * @brief Implementation — see voice_onboard.h for API contract.
 *
 * Extracted from voice.c in TT #327 Wave 4b — owns the entire vmode=4
 * surface (boot warm-up + per-text-turn failover + autonomous chain).
 * No behavior change vs. the pre-extract code; pure module-boundary
 * cleanup so future K144 work doesn't accrete to voice.c.
 */

#include "voice_onboard.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h" /* tab5_audio_play_raw */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "task_worker.h"  /* tab5_worker_enqueue */
#include "ui_chat.h"      /* ui_chat_add_message */
#include "ui_core.h"      /* tab5_ui_try_lock / tab5_ui_unlock */
#include "ui_home.h"      /* ui_home_show_toast */
#include "voice.h"        /* voice_set_state, VOICE_STATE_* */
#include "voice_m5_llm.h" /* probe / infer / chain_* */

static const char *TAG = "voice_onboard";

/* ---------------------------------------------------------------------- */
/*  Failover state                                                        */
/*                                                                        */
/*  When Dragon WS is unreachable for ≥ 30 s and the NVS voice_mode       */
/*  setting is "Local" (0), the next text turn is routed through          */
/*  voice_m5_llm_infer() on the shared task_worker queue.  Engagement is  */
/*  gated on a successful one-time warm-up so the K144's slow / hung      */
/*  NPU cold-start (LEARNINGS: "K144 cold-start model load can hang for   */
/*  5+ min") never blocks user-facing flows.                              */
/* ---------------------------------------------------------------------- */
typedef enum {
   M5_FAIL_UNKNOWN,     /* boot default — warm-up not yet started */
   M5_FAIL_PROBING,     /* warm-up job posted, infer in progress */
   M5_FAIL_READY,       /* one successful infer completed */
   M5_FAIL_UNAVAILABLE, /* probe / warm-up failed; failover disabled */
} m5_failover_state_t;

static volatile m5_failover_state_t s_m5_failover = M5_FAIL_UNKNOWN;
static volatile bool s_m5_failover_in_flight = false;
static volatile bool s_m5_failover_engaged_during_down = false; /* triggers "Dragon reconnected" toast */
#define M5_FAILOVER_INFER_TIMEOUT_S 60                          /* per-turn budget */
#define M5_FAILOVER_WARMUP_TIMEOUT_S 360                        /* cold-start budget — 6 min cap */

/* ---------------------------------------------------------------------- */
/*  Chain state                                                           */
/* ---------------------------------------------------------------------- */
static volatile bool s_chain_active = false;
static voice_m5_chain_handle_t *s_chain_handle = NULL;
static volatile bool s_chain_stop_flag = false;
static int64_t s_chain_started_us = 0; /* Wave 7: set on chain_start, cleared on drain exit */

/* Per-utterance accumulators — reset on every `finish=true` so multiple
 * back-to-back utterances within one chain session each get their own
 * bubble.  PSRAM-backed; statically declaring 2.5 KB of BSS pushed the
 * internal SRAM budget past the FreeRTOS timer-task stack alloc and the
 * device boot-looped with `vApplicationGetTimerTaskMemory: pxStackBufferTemp
 * != NULL` (same class of failure ui_sessions.c hit and fixed). */
#define CHAIN_ASR_CAP 512
#define CHAIN_LLM_CAP 2048
static char *s_chain_asr_buf = NULL;
static size_t s_chain_asr_len;
static char *s_chain_llm_buf = NULL;
static size_t s_chain_llm_len;

extern void tab5_debug_obs_event(const char *kind, const char *detail);

/* ---------------------------------------------------------------------- */
/*  Failover jobs                                                         */
/* ---------------------------------------------------------------------- */

/* Boot warm-up: probe + one synchronous infer to map the LLM into NPU
 * memory.  Up to 6 minutes — covers the K144's pathologically slow
 * cold-start.  On success the failover gate flips to READY; on any
 * failure (probe timeout, infer hang, NPU stall) it flips to UNAVAILABLE
 * and stays there until the next reboot. */
static void onboard_warmup_job(void *arg) {
   (void)arg;
   s_m5_failover = M5_FAIL_PROBING;
   ESP_LOGI(TAG, "K144 failover warm-up: probing module...");
   tab5_debug_obs_event("m5.warmup", "start");
   esp_err_t pe = voice_m5_llm_probe();
   if (pe != ESP_OK) {
      ESP_LOGW(TAG, "K144 probe failed (%s) — failover disabled", esp_err_to_name(pe));
      s_m5_failover = M5_FAIL_UNAVAILABLE;
      tab5_debug_obs_event("m5.warmup", "unavailable");
      return;
   }
   char scratch[64];
   int64_t t0 = esp_timer_get_time();
   esp_err_t ie = voice_m5_llm_infer("hi", scratch, sizeof(scratch), M5_FAILOVER_WARMUP_TIMEOUT_S);
   int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
   if (ie == ESP_OK) {
      ESP_LOGI(TAG, "K144 warm in %lldms — failover available: '%s'", dt_ms, scratch);
      s_m5_failover = M5_FAIL_READY;
      tab5_debug_obs_event("m5.warmup", "ready");
   } else {
      ESP_LOGW(TAG,
               "K144 warm-up %s after %lldms — failover disabled (NPU likely hung; "
               "see LEARNINGS \"K144 cold-start model load can hang\")",
               esp_err_to_name(ie), dt_ms);
      s_m5_failover = M5_FAIL_UNAVAILABLE;
      tab5_debug_obs_event("m5.warmup", "unavailable");
   }
}

/* Per-turn job: caller mallocs the prompt, this job free()s it.  Renders
 * the K144 reply as a regular assistant chat bubble alongside Dragon
 * replies. */
static void onboard_failover_text_job(void *arg) {
   char *prompt = (char *)arg;
   if (prompt == NULL) return;

   s_m5_failover_in_flight = true;
   s_m5_failover_engaged_during_down = true; /* arms the reconnect-back toast */

   /* Onboard-LLM badge.  voice_set_state(PROCESSING) drives the orb /
    * status bar to "thinking..." just like a Dragon turn.  The USER
    * bubble is added by the caller (/chat handler does
    * `ui_chat_push_message("user", ...)` before voice_send_text), so we
    * don't add it here — would duplicate. */
   if (tab5_ui_try_lock(150)) {
      ui_home_show_toast("Using onboard LLM");
      tab5_ui_unlock();
   }
   voice_set_state(VOICE_STATE_PROCESSING, "K144");

   char reply[1024] = {0};
   esp_err_t ie = voice_m5_llm_infer(prompt, reply, sizeof(reply), M5_FAILOVER_INFER_TIMEOUT_S);

   if (ie == ESP_OK && reply[0] != '\0') {
      if (tab5_ui_try_lock(150)) {
         ui_chat_add_message(reply, /*is_user=*/false);
         tab5_ui_unlock();
      }
      ESP_LOGI(TAG, "K144 reply: '%s'", reply);
   } else {
      if (tab5_ui_try_lock(150)) {
         ui_home_show_toast("Onboard LLM unavailable");
         tab5_ui_unlock();
      }
      ESP_LOGW(TAG, "K144 failover failed (%s) for prompt '%s'", esp_err_to_name(ie), prompt);
      /* If the K144 has gone hung mid-session, lock failover off so we
       * don't keep retrying every text turn. */
      if (ie == ESP_ERR_TIMEOUT) s_m5_failover = M5_FAIL_UNAVAILABLE;
   }

   voice_set_state(VOICE_STATE_READY, NULL);
   free(prompt);
   s_m5_failover_in_flight = false;
}

/* ---------------------------------------------------------------------- */
/*  Public API — failover                                                 */
/* ---------------------------------------------------------------------- */

esp_err_t voice_onboard_start_warmup(void) {
   if (s_m5_failover != M5_FAIL_UNKNOWN) return ESP_ERR_INVALID_STATE;
   return tab5_worker_enqueue(onboard_warmup_job, NULL, "m5_warmup");
}

int voice_onboard_failover_state(void) { return (int)s_m5_failover; }

esp_err_t voice_onboard_send_text(const char *text) {
   if (text == NULL || text[0] == '\0') return ESP_ERR_INVALID_ARG;
   if (s_m5_failover != M5_FAIL_READY) return ESP_ERR_INVALID_STATE;
   if (s_m5_failover_in_flight) return ESP_ERR_NO_MEM;
   char *copy = strdup(text);
   if (copy == NULL) return ESP_ERR_NO_MEM;
   esp_err_t qe = tab5_worker_enqueue(onboard_failover_text_job, copy, "m5_failover");
   if (qe != ESP_OK) {
      free(copy);
      return qe;
   }
   return ESP_OK;
}

bool voice_onboard_consume_engagement_flag(void) {
   if (!s_m5_failover_engaged_during_down) return false;
   s_m5_failover_engaged_during_down = false;
   return true;
}

/* ---------------------------------------------------------------------- */
/*  Chain — autonomous voice-assistant pipeline (vmode=4 mic-tap)         */
/* ---------------------------------------------------------------------- */

/* Wave 3a Eigen workaround: synthesize one LLM reply text via the K144's
 * one-shot TTS path (NOT the chained tts.setup that crashes), upsample
 * 1:3 to 48 kHz, play through Tab5's speaker.  Posted to tab5_worker by
 * onboard_text_callback on every LLM finish=true so it runs off the
 * chain drain task (which is busy reading more frames). */
static void onboard_chain_tts_job(void *arg) {
   char *text = (char *)arg;
   if (text == NULL) return;
   /* PSRAM scratch — 8 sec @ 16 kHz mono = 128 KB samples = 256 KB. */
   const size_t cap = 16 * 8 * 1024;
   int16_t *pcm16 = heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (pcm16 == NULL) {
      free(text);
      return;
   }
   size_t got = 0;
   esp_err_t te = voice_m5_llm_tts(text, pcm16, cap, &got, 30);
   if (te == ESP_OK && got > 0) {
      const size_t cap48 = got * 3;
      int16_t *pcm48 = heap_caps_malloc(cap48 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (pcm48 != NULL) {
         for (size_t i = 0; i < got; i++) {
            const int16_t cur = pcm16[i];
            const int16_t nxt = (i + 1 < got) ? pcm16[i + 1] : cur;
            for (int j = 0; j < 3; j++) {
               pcm48[i * 3 + j] = (int16_t)(cur + (int32_t)(nxt - cur) * j / 3);
            }
         }
         tab5_audio_play_raw(pcm48, cap48);
         heap_caps_free(pcm48);
      }
   } else if (te != ESP_OK) {
      ESP_LOGW(TAG, "chain per-utterance TTS failed (%s) for '%.60s'", esp_err_to_name(te), text);
   }
   heap_caps_free(pcm16);
   free(text);
}

static void onboard_text_callback(const char *text, bool from_llm, bool finish, void *user) {
   (void)user;
   char *buf = from_llm ? s_chain_llm_buf : s_chain_asr_buf;
   size_t *len = from_llm ? &s_chain_llm_len : &s_chain_asr_len;
   const size_t cap = from_llm ? CHAIN_LLM_CAP : CHAIN_ASR_CAP;
   if (buf == NULL) return; /* race with teardown */
   const size_t add = strlen(text);

   /* sherpa-ncnn ASR streams CUMULATIVE partials — each delta is the full
    * transcription so far, growing incrementally.  K144 LLM streams
    * ADDITIVE tokens — each delta is the next chunk to append.  Different
    * accumulator strategies. */
   if (from_llm) {
      if (*len + add < cap - 1) {
         memcpy(buf + *len, text, add);
         *len += add;
         buf[*len] = '\0';
      }
      /* Wave 6 (audit #8): stream chain LLM tokens through voice_set_llm_text
       * so ui_chat's poll_voice picks them up via the same streaming-bubble
       * code path Dragon's WS path uses.  First delta of an LLM turn flips
       * state to PROCESSING — that's the trigger poll_voice watches for to
       * begin the live bubble.  finish=true returns to LISTENING (chain
       * keeps draining) so subsequent ASR finalisations don't get dropped. */
      voice_set_llm_text(buf);
      if (*len == add) {
         /* This was the first delta of this LLM turn (len was 0 before). */
         voice_set_state(VOICE_STATE_PROCESSING, "K144");
      }
   } else {
      const size_t copy = (add < cap - 1) ? add : cap - 1;
      memcpy(buf, text, copy);
      buf[copy] = '\0';
      *len = copy;
   }

   if (finish && *len > 0) {
      ESP_LOGI(TAG, "chain %s commit: '%s'", from_llm ? "LLM" : "ASR", buf);

      /* Wave 3a Eigen workaround: chain doesn't include tts.setup any
       * more (K144's SummerTTS crashes mid-stream from LLM).  Instead,
       * on every LLM finish=true, kick off a per-utterance synth via
       * voice_m5_llm_tts on the worker queue.  The worker's
       * voice_m5_llm_tts call serialises on the same UART mutex the
       * chain holds per-iteration (Wave 1) so they don't collide. */
      if (from_llm) {
         char *copy = strdup(buf);
         if (copy != NULL) {
            if (tab5_worker_enqueue(onboard_chain_tts_job, copy, "chain_tts") != ESP_OK) {
               ESP_LOGW(TAG, "chain TTS enqueue failed (queue full?), reply text-only");
               free(copy);
            }
         }
      }

      if (from_llm) {
         /* Wave 6: poll_voice's streaming-bubble code path manages the
          * TINKER bubble create + update + commit lifecycle (sees s_llm_text
          * during PROCESSING/SPEAKING).  Returning to LISTENING ends the
          * stream — poll_voice calls chat_msg_view_end_streaming on the
          * PROCESSING→non-PROCESSING transition. */
         voice_set_state(VOICE_STATE_LISTENING, "K144");
      } else {
         /* ASR is the user's spoken turn — commit as user bubble (no
          * streaming UX here; ASR commit-on-finish is fine). */
         if (tab5_ui_try_lock(150)) {
            ui_chat_add_message(buf, /*is_user=*/true);
            tab5_ui_unlock();
         }
      }
      *len = 0;
      buf[0] = '\0';
   }
}

/* No audio callback — Wave 3a stripped tts.setup from the chain after
 * the K144 SummerTTS Eigen crash documented in LEARNINGS.  Chain_run
 * accepts a NULL audio_cb (unmatched tts frames are dropped in the
 * dispatch).  Per-utterance synth lives in onboard_chain_tts_job
 * instead. */

static void onboard_free_buffers(void) {
   if (s_chain_asr_buf) {
      heap_caps_free(s_chain_asr_buf);
      s_chain_asr_buf = NULL;
   }
   if (s_chain_llm_buf) {
      heap_caps_free(s_chain_llm_buf);
      s_chain_llm_buf = NULL;
   }
   s_chain_asr_len = 0;
   s_chain_llm_len = 0;
}

static esp_err_t onboard_alloc_buffers(void) {
   if (s_chain_asr_buf == NULL) {
      s_chain_asr_buf = heap_caps_malloc(CHAIN_ASR_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (s_chain_asr_buf == NULL) goto fail;
   }
   if (s_chain_llm_buf == NULL) {
      s_chain_llm_buf = heap_caps_malloc(CHAIN_LLM_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (s_chain_llm_buf == NULL) goto fail;
   }
   s_chain_asr_buf[0] = '\0';
   s_chain_llm_buf[0] = '\0';
   s_chain_asr_len = 0;
   s_chain_llm_len = 0;
   return ESP_OK;
fail:
   onboard_free_buffers();
   return ESP_ERR_NO_MEM;
}

static void onboard_chain_drain_task(void *arg) {
   (void)arg;

   /* Setup runs HERE on the worker task, NOT on the LVGL tap callback —
    * issuing four serialised UART round-trips with NPU cold-start delays
    * blocks for ~5 sec, which would WDT-reset the LVGL task on a tap. */
   voice_m5_chain_handle_t *h = NULL;
   esp_err_t e = voice_m5_llm_chain_setup(&h, &s_chain_stop_flag);
   if (e != ESP_OK || h == NULL) {
      ESP_LOGW(TAG, "chain setup failed: %s", esp_err_to_name(e));
      onboard_free_buffers();
      s_chain_active = false;
      voice_set_state(VOICE_STATE_READY, NULL);

      /* Wave 7: map common failure modes to specific user toasts so the
       * user knows what to do, not just that it didn't work.  Audit
       * UX #4: the previous generic "Onboard chain unavailable" was
       * not actionable. */
      const char *toast;
      switch (e) {
         case ESP_ERR_TIMEOUT:
            /* No ACK from K144 within the setup window — module wedged
             * (NPU hang, USB-C unplugged, services restarting). */
            toast = "K144 not responding — power-cycle?";
            break;
         case ESP_ERR_INVALID_RESPONSE:
            /* K144 NACK'd a setup (e.g. -21 task full from previous
             * accumulated work_ids).  Service restart usually clears. */
            toast = "K144 busy — restart services?";
            break;
         case ESP_ERR_INVALID_STATE:
            /* User tapped stop during NPU cold-start; chain_setup_unit
             * bailed early via stop_flag.  Quiet return. */
            toast = NULL;
            break;
         case ESP_ERR_NO_MEM:
            toast = "Out of memory for K144 chain";
            break;
         default:
            toast = "Onboard chain unavailable";
            break;
      }
      if (toast != NULL && tab5_ui_try_lock(150)) {
         ui_home_show_toast(toast);
         tab5_ui_unlock();
      }
      vTaskDelete(NULL);
      return;
   }
   s_chain_handle = h;
   voice_set_state(VOICE_STATE_LISTENING, "K144");
   if (tab5_ui_try_lock(150)) {
      ui_home_show_toast("Onboard chat — speak at the K144");
      tab5_ui_unlock();
   }
   ESP_LOGI(TAG, "chain ready — entering drain loop");

   /* 10-min hard cap; user-initiated stop happens earlier via stop_flag. */
   esp_err_t re = voice_m5_llm_chain_run(h, onboard_text_callback, /*audio_cb=*/NULL, NULL, &s_chain_stop_flag, 600);
   ESP_LOGI(TAG, "chain drain exited: %s", esp_err_to_name(re));

   voice_m5_llm_chain_teardown(h);
   onboard_free_buffers();
   s_chain_handle = NULL;
   /* Audit #12: gate flips LAST so a fast double-tap re-entry of
    * voice_onboard_chain_start sees freed buffers (onboard_alloc_buffers
    * will re-alloc cleanly) rather than the prior session's still-mapped
    * pointers. */
   s_chain_started_us = 0;
   s_chain_active = false;

   voice_set_state(VOICE_STATE_READY, NULL);
   if (tab5_ui_try_lock(150)) {
      ui_home_show_toast("Onboard chat ended");
      tab5_ui_unlock();
   }
   vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------- */
/*  Public API — chain                                                    */
/* ---------------------------------------------------------------------- */

esp_err_t voice_onboard_chain_start(void) {
   if (s_chain_active) return ESP_ERR_INVALID_STATE;

   esp_err_t be = onboard_alloc_buffers();
   if (be != ESP_OK) {
      ESP_LOGW(TAG, "chain buffer alloc failed");
      return be;
   }

   s_chain_stop_flag = false;
   s_chain_started_us = esp_timer_get_time();
   s_chain_active = true;

   /* Returns immediately; the drain task does the heavy chain_setup() so
    * the LVGL tap callback isn't blocked for the K144's 5-sec NPU cold
    * start.  PROCESSING is the right transient — drain task flips to
    * LISTENING once setup completes, READY on teardown.
    *
    * 8 KB stack — drain loop calls cJSON parse + base64 decode + the audio
    * upsample inline; matches voice's other long-lived tasks. */
   BaseType_t r = xTaskCreate(onboard_chain_drain_task, "m5_chain", 8192, NULL, 5, NULL);
   if (r != pdPASS) {
      s_chain_active = false;
      onboard_free_buffers();
      return ESP_ERR_NO_MEM;
   }

   voice_set_state(VOICE_STATE_PROCESSING, "K144 setup");
   if (tab5_ui_try_lock(150)) {
      ui_home_show_toast("Onboard chat starting…");
      tab5_ui_unlock();
   }
   ESP_LOGI(TAG, "chain start dispatched to drain task");
   tab5_debug_obs_event("m5.chain", "start");
   return ESP_OK;
}

esp_err_t voice_onboard_chain_stop(void) {
   if (!s_chain_active) return ESP_ERR_INVALID_STATE;
   ESP_LOGI(TAG, "chain stop requested");
   tab5_debug_obs_event("m5.chain", "stop");
   s_chain_stop_flag = true;
   /* Drain task notices stop_flag, tears down, transitions state to READY,
    * shows toast, deletes itself.  Do NOT free the handle here — the task
    * owns its lifetime. */
   return ESP_OK;
}

bool voice_onboard_chain_active(void) { return s_chain_active; }

int64_t voice_onboard_chain_uptime_ms(void) {
   if (!s_chain_active || s_chain_started_us == 0) return 0;
   return (esp_timer_get_time() - s_chain_started_us) / 1000;
}
