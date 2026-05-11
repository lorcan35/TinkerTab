/* voice_solo — Tab5 vmode=5 SOLO_DIRECT orchestrator.
 *
 * Owns the chat UI + state machine + (eventually) STT/TTS chain.
 * voice_solo_send_text enqueues the LLM round-trip onto the shared
 * task_worker so the caller (voice_modes_route_text → voice.c) does
 * not block.  STT + TTS verbs land in follow-up commits.
 *
 * TT #370 — see docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md
 */

#include "voice_solo.h"

#include <stdlib.h>
#include <string.h>

#include "audio.h" /* tab5_audio_speaker_enable */
#include "cJSON.h"
#include "debug_obs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "openrouter_client.h"
#include "solo_rag.h"
#include "solo_session_store.h"
#include "task_worker.h"
#include "ui_chat.h"
#include "ui_home.h"
#include "voice.h"
#include "voice_messages_sync.h" /* W3-C-b: Dragon canonical message store */

static const char *TAG = "voice_solo";

static bool s_initialized = false;
static volatile bool s_busy = false;
/* W1-D (TT #372): mutex makes the s_busy test-and-set atomic across
 * the caller and worker threads.  Without it, two callers could both
 * see s_busy==false before either worker runs, double-enqueue, and
 * then race on session/UI state. */
static SemaphoreHandle_t s_busy_mtx = NULL;
static bool busy_take(void) {
   if (!s_busy_mtx) {
      s_busy_mtx = xSemaphoreCreateMutex();
      if (!s_busy_mtx) return false;
   }
   xSemaphoreTake(s_busy_mtx, portMAX_DELAY);
   bool taken = false;
   if (!s_busy) {
      s_busy = true;
      taken = true;
   }
   xSemaphoreGive(s_busy_mtx);
   return taken;
}
static void busy_release(void) {
   if (!s_busy_mtx) return;
   xSemaphoreTake(s_busy_mtx, portMAX_DELAY);
   s_busy = false;
   xSemaphoreGive(s_busy_mtx);
}
/* W1-C (TT #372): cancel flag checked at each job step.  Old behavior:
 * voice_solo_cancel called openrouter_cancel_inflight() but the worker
 * continued through solo_session_append + voice_set_state(READY), so a
 * canceled mid-LLM turn got persisted as if it succeeded — corrupting
 * the session log for the next turn (likely contributor to U1). */
static volatile bool s_cancel_requested = false;

/* Single-flight accumulator — we never overlap two solo turns
 * (s_busy guards the entry path).  PSRAM-backed so a long reply
 * doesn't fragment internal SRAM.  W3-A (TT #374): grows on demand
 * up to SOLO_ACC_HARD_CAP — old fixed 8 KB silently truncated long
 * Opus replies mid-sentence both in the chat overlay AND the
 * persisted session. */
#define SOLO_ACC_INITIAL_CAP (8 * 1024)
#define SOLO_ACC_HARD_CAP (64 * 1024)
#define SOLO_ACC_TRUNC_MARKER "\n\n[…truncated]"

typedef struct {
   char *acc;
   size_t cap;
   size_t len;
   bool truncated; /* set once the hard cap is hit + marker appended */
} solo_chat_acc_t;

/* Grow the accumulator if `need_bytes` extra won't fit.  Doubles
 * capacity up to SOLO_ACC_HARD_CAP.  Returns true if the new bytes
 * will fit, false if we've hit the hard cap (caller appends the
 * truncation marker once). */
static bool solo_acc_reserve(solo_chat_acc_t *a, size_t need_bytes) {
   if (a->len + need_bytes + 1 < a->cap) return true;
   if (a->cap >= SOLO_ACC_HARD_CAP) return false;
   size_t newcap = a->cap ? a->cap * 2 : SOLO_ACC_INITIAL_CAP;
   while (newcap < a->len + need_bytes + 1 && newcap < SOLO_ACC_HARD_CAP) {
      newcap *= 2;
   }
   if (newcap > SOLO_ACC_HARD_CAP) newcap = SOLO_ACC_HARD_CAP;
   char *grown = heap_caps_realloc(a->acc, newcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!grown) return false;
   a->acc = grown;
   a->cap = newcap;
   return a->len + need_bytes + 1 < a->cap;
}

static void solo_delta_cb(const char *delta, size_t n, void *vctx) {
   solo_chat_acc_t *a = vctx;
   if (solo_acc_reserve(a, n)) {
      memcpy(a->acc + a->len, delta, n);
      a->len += n;
      a->acc[a->len] = '\0';
   } else if (!a->truncated) {
      /* Hit the hard cap — append the marker exactly once and stop
       * accumulating.  Visible to the user instead of a silent cutoff. */
      const size_t mlen = strlen(SOLO_ACC_TRUNC_MARKER);
      if (a->len + mlen + 1 < a->cap) {
         memcpy(a->acc + a->len, SOLO_ACC_TRUNC_MARKER, mlen);
         a->len += mlen;
         a->acc[a->len] = '\0';
      }
      a->truncated = true;
      ESP_LOGW(TAG, "solo reply hit hard cap %u — marker appended, further deltas dropped",
               (unsigned)SOLO_ACC_HARD_CAP);
   }
   /* Stream into chat UI as deltas arrive.  ui_chat_update_last_message
    * replaces the trailing assistant bubble's text every chunk. */
   ui_chat_update_last_message(a->acc);
}

/* Build [...history..., {"role":"user","content":<text>}].  Caller
 * frees the returned malloc'd JSON string with free().  History is
 * pulled from solo_session_store; on first turn it's just the new
 * user message. */
static char *solo_build_messages_json(const char *user_text) {
   /* 16 KB on the worker task's 16 KB stack triggered Core 1 stack
    * protection fault the moment mbedTLS pushed its handshake frame.
    * Per CLAUDE.md memory rules, anything >4 KB must live in PSRAM. */
   const size_t hist_cap = 16384;
   char *history = heap_caps_calloc(1, hist_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!history) return NULL;
   solo_session_load_recent(20, history, hist_cap);
   cJSON *arr = cJSON_Parse(history);
   if (!arr) arr = cJSON_CreateArray();
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON_AddStringToObject(m, "content", user_text);
   cJSON_AddItemToArray(arr, m);
   char *s = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   heap_caps_free(history);
   return s;
}

/* Job function — runs on the shared tab5_worker.  arg is a malloc'd
 * copy of the user text; we own freeing it.  No vTaskSuspend / Delete
 * here since tab5_worker hosts us. */
static void solo_send_text_job(void *arg) {
   char *text = arg;
   /* W1-D: s_busy already set by busy_take() in voice_solo_send_text. */
   s_cancel_requested = false; /* W1-C: clear from any prior cancel */

   voice_set_state(VOICE_STATE_PROCESSING, "solo");
   tab5_debug_obs_event("solo.llm_start", "");

   /* W1-E (TT #372): do NOT push the "user" bubble here — upstream
    * /chat/handler + voice_ws_proto already pushed it before routing
    * the text to us.  Pushing again caused doubled user bubbles in
    * the chat overlay (visible in screenshots).  We still push the
    * empty assistant placeholder so the delta callback has a bubble
    * to update_last_message into. */
   ui_chat_push_message("assistant", "");

   /* W3-A: initial allocation; solo_acc_reserve grows on demand up to
    * SOLO_ACC_HARD_CAP. */
   solo_chat_acc_t acc = {0};
   acc.cap = SOLO_ACC_INITIAL_CAP;
   acc.acc = heap_caps_calloc(1, acc.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

   char *msgs_json = solo_build_messages_json(text);
   int64_t t0 = esp_timer_get_time();
   esp_err_t err = ESP_ERR_NO_MEM;
   if (s_cancel_requested) {
      err = ESP_ERR_INVALID_STATE;
   } else if (msgs_json && acc.acc) {
      err = openrouter_chat_stream(msgs_json, solo_delta_cb, &acc);
   }
   int64_t dt = (esp_timer_get_time() - t0) / 1000;
   char detail[48];
   snprintf(detail, sizeof detail, "ms=%lld", (long long)dt);

   /* W1-C: distinguish cancel from genuine failure.  Persisting a
    * canceled or partial reply to the session pollutes the next turn's
    * history. */
   if (s_cancel_requested) {
      tab5_debug_obs_event("solo.cancel", detail);
      ESP_LOGI(TAG, "solo turn canceled — skipping session append");
   } else if (err != ESP_OK) {
      tab5_debug_obs_event("solo.error", detail);
      ESP_LOGE(TAG, "solo LLM failed: %s", esp_err_to_name(err));
      ui_home_show_toast("Solo LLM failed");
   } else {
      tab5_debug_obs_event("solo.llm_done", detail);
      /* Persist both turns to the SD-backed session.  acc.acc is
       * NUL-terminated by the delta callback. */
      solo_session_append("user", text);
      solo_session_append("assistant", acc.acc ? acc.acc : "");
      /* W3-C-b (cross-stack cohesion audit 2026-05-11): also POST to
       * Dragon's canonical messages DB so the dashboard's Conversations
       * tab and cross-session memory search see the SOLO turn. */
      voice_messages_sync_post("user", text, "text");
      voice_messages_sync_post("assistant", acc.acc ? acc.acc : "", "text");
   }

   free(msgs_json);
   heap_caps_free(acc.acc);
   free(text);

   voice_set_state(VOICE_STATE_READY, "solo");
   s_cancel_requested = false;
   busy_release(); /* W1-D */
}

esp_err_t voice_solo_init(void) {
   if (s_initialized) return ESP_OK;
   solo_session_init();        /* mkdir /sdcard/sessions, idempotent */
   solo_session_open(NULL, 0); /* warm the active sid from NVS */
   solo_rag_init();            /* touch /sdcard/rag.bin, idempotent */
   s_initialized = true;
   ESP_LOGI(TAG, "voice_solo_init OK");
   return ESP_OK;
}

esp_err_t voice_solo_send_text(const char *text) {
   if (!text || !*text) return ESP_ERR_INVALID_ARG;
   /* W1-D: atomic test-and-set so two concurrent callers can't both
    * pass the busy check before either worker runs. */
   if (!busy_take()) return ESP_ERR_INVALID_STATE;

   char *copy = strdup(text);
   if (!copy) {
      busy_release();
      return ESP_ERR_NO_MEM;
   }

   esp_err_t e = tab5_worker_enqueue(solo_send_text_job, copy, "solo_text");
   if (e != ESP_OK) {
      free(copy);
      busy_release();
      return e;
   }
   return ESP_OK;
}

/* OpenRouter's audio models (gpt-4o-audio-preview, tts-1) return PCM-16LE
 * at 24 kHz mono.  voice_playback_buf_write expects 48 kHz (the I2S codec
 * native rate per bsp_config.h TAB5_AUDIO_SAMPLE_RATE).  The Dragon TTS
 * path in voice_ws_proto.c upsamples 16→48 kHz before writing; ours has
 * to upsample 24→48 kHz (1:2 ratio).  Linear interpolation with one
 * boundary-carry sample so a chunk break doesn't produce a click. */
static int16_t s_tts_carry;
static bool s_tts_carry_valid;

static void solo_tts_chunk_cb(const uint8_t *bytes, size_t len, void *vctx) {
   (void)vctx;
   if (!bytes || len < sizeof(int16_t)) return;
   const int16_t *in = (const int16_t *)bytes;
   size_t in_samples = len / sizeof(int16_t);
   if (in_samples == 0) return;

   size_t out_samples = in_samples * 2;
   int16_t *out = heap_caps_malloc(out_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!out) return;

   /* Bridge the chunk boundary: if we have a previous carry, interpolate
    * from carry → in[0] for the first output pair.  Otherwise just hold
    * in[0] for the first half-step (silence until the next sample). */
   int16_t prev = s_tts_carry_valid ? s_tts_carry : in[0];
   for (size_t k = 0; k < in_samples; k++) {
      int16_t curr = in[k];
      out[2 * k] = (int16_t)(((int32_t)prev + (int32_t)curr) / 2);
      out[2 * k + 1] = curr;
      prev = curr;
   }
   s_tts_carry = in[in_samples - 1];
   s_tts_carry_valid = true;

   /* Backpressure: OpenRouter audio chunks are large (~24 K upsampled
    * samples each = ~500 ms of 48 kHz audio).  voice_playback_buf_write
    * is non-blocking — anything past the ring capacity is silently
    * dropped, which sounds like dropouts in the playback.  Slice the
    * upsampled chunk into ~1 K-sample writes and yield when the ring
    * pushes back so the drain task can catch up. */
   size_t off = 0;
   while (off < out_samples) {
      size_t slice = out_samples - off;
      if (slice > 1024) slice = 1024;
      size_t wrote = voice_playback_buf_write(out + off, slice);
      off += wrote;
      if (wrote < slice) {
         /* Ring full — wait one playback frame (~21 ms at 48 kHz × 1 K)
          * for the drain task to free space. */
         vTaskDelay(pdMS_TO_TICKS(20));
      }
   }
   heap_caps_free(out);
}

/* Job function for the audio path (mic → STT → LLM → TTS → playback).
 * arg is a malloc'd struct containing the PCM pointer + sample count;
 * we own freeing both the struct and the PCM buffer. */
typedef struct {
   int16_t *pcm;
   size_t samples;
} solo_audio_job_t;

/* TT #379 + follow-up: audio path runs STT first to recover the user-side
 * transcript (the multimodal model doesn't echo input back), then the
 * single multimodal /chat/completions[audio] call for the assistant
 * audio + transcript.  Two calls instead of three (was STT + LLM + TTS) —
 * STT for the user bubble, chat_audio for everything else. */
static void solo_send_audio_job(void *arg) {
   solo_audio_job_t *job = arg;
   /* W1-D: s_busy already set by busy_take() */
   s_cancel_requested = false; /* W1-C */

   voice_set_state(VOICE_STATE_PROCESSING, "solo");

   /* Step 1 — STT to recover the user transcript.  Cheap (~2 s for a
    * 4 s clip).  On failure, fall back to "[voice input]" placeholder
    * so the user bubble is never blank. */
   char user_text[1024] = {0};
   tab5_debug_obs_event("solo.stt_start", "");
   int64_t t0 = esp_timer_get_time();
   esp_err_t err = openrouter_stt(job->pcm, job->samples, user_text, sizeof user_text);
   int64_t dt = (esp_timer_get_time() - t0) / 1000;
   char detail[48];
   snprintf(detail, sizeof detail, "ms=%lld", (long long)dt);
   tab5_debug_obs_event(err == ESP_OK ? "solo.stt_done" : "solo.error", detail);

   if (s_cancel_requested) {
      ESP_LOGI(TAG, "solo audio canceled after STT");
      goto done_free_pcm;
   }
   if (err != ESP_OK || user_text[0] == '\0') {
      /* Don't fail the whole turn — STT is just nice-to-have for the
       * user bubble.  The multimodal call below still hears the audio. */
      ESP_LOGW(TAG, "solo STT failed/empty — falling back to placeholder");
      strncpy(user_text, "[voice input]", sizeof user_text - 1);
   }

   ui_chat_push_message("user", user_text);
   ui_chat_push_message("assistant", "");

   solo_chat_acc_t acc = {0};
   acc.cap = SOLO_ACC_INITIAL_CAP;
   acc.acc = heap_caps_calloc(1, acc.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

   /* Audio chunks start arriving during the same stream — flip state
    * to SPEAKING the moment we get the first one so the orb pulses. */
   voice_set_state(VOICE_STATE_SPEAKING, "solo");
   s_tts_carry_valid = false; /* reset 24→48 upsample boundary between turns */
   /* Power on the NS4150B speaker amp now so the first audio chunk
    * lands on a live output.  The playback drain task disables it
    * again once the ring empties + s_tts_done flips. */
   tab5_audio_speaker_enable(true);

   tab5_debug_obs_event("solo.audio_start", "");
   t0 = esp_timer_get_time();
   if (acc.acc) {
      err = openrouter_chat_audio(job->pcm, job->samples, solo_tts_chunk_cb, solo_delta_cb, &acc);
   } else {
      err = ESP_ERR_NO_MEM;
   }
   dt = (esp_timer_get_time() - t0) / 1000;
   snprintf(detail, sizeof detail, "ms=%lld", (long long)dt);

   if (s_cancel_requested) {
      tab5_debug_obs_event("solo.cancel", detail);
      ESP_LOGI(TAG, "solo audio canceled mid-stream — skipping session append");
      heap_caps_free(acc.acc);
      goto done_free_pcm;
   }
   tab5_debug_obs_event(err == ESP_OK ? "solo.audio_done" : "solo.error", detail);
   if (err != ESP_OK) {
      ui_home_show_toast("Solo audio chat failed");
      heap_caps_free(acc.acc);
      tab5_audio_speaker_enable(false); /* abort path — speaker off */
      goto done_free_pcm;
   }
   /* Persist both sides of the turn.  user_text is either the real
    * transcript or the "[voice input]" fallback. */
   solo_session_append("user", user_text);
   solo_session_append("assistant", acc.acc ? acc.acc : "");
   /* W3-C-b (cross-stack cohesion audit 2026-05-11): also POST to
    * Dragon's canonical messages DB.  input_mode=voice marks the
    * audio path so the dashboard can filter by modality. */
   voice_messages_sync_post("user", user_text, "voice");
   voice_messages_sync_post("assistant", acc.acc ? acc.acc : "", "voice");
   heap_caps_free(acc.acc);

   /* Signal the playback drain task that no more audio is coming for
    * this turn.  It'll drain the ring, disable the speaker amp, and
    * transition state back to READY itself.  We do NOT call
    * voice_set_state(READY) below — the drain task owns that
    * transition so we don't race it. */
   s_tts_done = true;
   /* Skip the unconditional READY transition at the bottom; drain
    * task handles it.  Falls through to free pcm + release busy. */
   heap_caps_free(job->pcm);
   free(job);
   s_cancel_requested = false;
   busy_release(); /* W1-D */
   return;

done_free_pcm:
   heap_caps_free(job->pcm);
   free(job);
   voice_set_state(VOICE_STATE_READY, "solo");
   s_cancel_requested = false;
   busy_release(); /* W1-D */
}

esp_err_t voice_solo_send_audio(int16_t *pcm, size_t samples) {
   if (!pcm || samples == 0) {
      heap_caps_free(pcm);
      return ESP_ERR_INVALID_ARG;
   }
   /* W1-D: atomic test-and-set */
   if (!busy_take()) {
      heap_caps_free(pcm);
      return ESP_ERR_INVALID_STATE;
   }
   solo_audio_job_t *job = malloc(sizeof *job);
   if (!job) {
      heap_caps_free(pcm);
      busy_release();
      return ESP_ERR_NO_MEM;
   }
   job->pcm = pcm;
   job->samples = samples;
   esp_err_t e = tab5_worker_enqueue(solo_send_audio_job, job, "solo_audio");
   if (e != ESP_OK) {
      heap_caps_free(pcm);
      free(job);
      busy_release();
      return e;
   }
   return ESP_OK;
}

void voice_solo_cancel(void) {
   /* W1-C: set the flag FIRST, then break the HTTP.  Order matters —
    * if the job sees the cancel after openrouter_chat_stream returns
    * normally, the s_cancel_requested check skips session append. */
   s_cancel_requested = true;
   openrouter_cancel_inflight();
   ESP_LOGI(TAG, "voice_solo_cancel: flag set + inflight closed");
}

bool voice_solo_busy(void) { return s_busy; }
