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

/* OpenRouter TTS-1 returns 24 kHz mono PCM; voice_playback_buf_write
 * expects the same rate the existing Dragon-TTS pipeline uses (16 kHz —
 * the playback drain task upsamples 1:3 to 48 kHz on output).  3:2
 * decimation with a 1-tap average across each pair gives a clean
 * downsample without a low-pass filter:
 *   out[2k]   = (in[3k]   + in[3k+1]) / 2
 *   out[2k+1] = (in[3k+1] + in[3k+2]) / 2
 *
 * Caller may feed odd-aligned chunks; we keep up to 2 leftover
 * samples in `s_tts_carry` for the next call. */
static int16_t s_tts_carry[2];
static size_t s_tts_carry_n;

static void solo_tts_chunk_cb(const uint8_t *bytes, size_t len, void *vctx) {
   (void)vctx;
   if (!bytes || len < sizeof(int16_t)) return;
   const int16_t *in = (const int16_t *)bytes;
   size_t in_samples = len / sizeof(int16_t);

   size_t span = s_tts_carry_n + in_samples;
   int16_t *buf = heap_caps_malloc(span * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!buf) return;
   if (s_tts_carry_n) memcpy(buf, s_tts_carry, s_tts_carry_n * sizeof(int16_t));
   memcpy(buf + s_tts_carry_n, in, in_samples * sizeof(int16_t));

   size_t triples = span / 3;
   if (triples == 0) {
      memcpy(s_tts_carry, buf, span * sizeof(int16_t));
      s_tts_carry_n = span;
      heap_caps_free(buf);
      return;
   }
   int16_t *out = heap_caps_malloc(triples * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!out) {
      heap_caps_free(buf);
      return;
   }
   for (size_t k = 0; k < triples; k++) {
      out[2 * k] = (buf[3 * k] + buf[3 * k + 1]) / 2;
      out[2 * k + 1] = (buf[3 * k + 1] + buf[3 * k + 2]) / 2;
   }
   /* Stash leftover (≤2 samples). */
   s_tts_carry_n = span - triples * 3;
   if (s_tts_carry_n) {
      memcpy(s_tts_carry, buf + triples * 3, s_tts_carry_n * sizeof(int16_t));
   }
   voice_playback_buf_write(out, triples * 2);
   heap_caps_free(buf);
   heap_caps_free(out);
}

/* Job function for the audio path (mic → STT → LLM → TTS → playback).
 * arg is a malloc'd struct containing the PCM pointer + sample count;
 * we own freeing both the struct and the PCM buffer. */
typedef struct {
   int16_t *pcm;
   size_t samples;
} solo_audio_job_t;

static void solo_send_audio_job(void *arg) {
   solo_audio_job_t *job = arg;
   /* W1-D: s_busy already set by busy_take() */
   s_cancel_requested = false; /* W1-C */

   voice_set_state(VOICE_STATE_PROCESSING, "solo");
   tab5_debug_obs_event("solo.stt_start", "");

   /* Step 1 — STT. */
   char transcript[1024] = {0};
   int64_t t0 = esp_timer_get_time();
   esp_err_t err = openrouter_stt(job->pcm, job->samples, transcript, sizeof transcript);
   int64_t dt = (esp_timer_get_time() - t0) / 1000;
   char detail[48];
   snprintf(detail, sizeof detail, "ms=%lld", (long long)dt);
   tab5_debug_obs_event(err == ESP_OK ? "solo.stt_done" : "solo.error", detail);
   heap_caps_free(job->pcm);
   if (s_cancel_requested) {
      ESP_LOGI(TAG, "solo audio canceled after STT");
      goto done;
   }
   if (err != ESP_OK || transcript[0] == '\0') {
      ui_home_show_toast("Solo STT failed");
      goto done;
   }

   /* Step 2 — LLM streaming with chat-bubble updates. */
   ui_chat_push_message("user", transcript);
   ui_chat_push_message("assistant", "");

   solo_chat_acc_t acc = {0};
   acc.cap = SOLO_ACC_INITIAL_CAP;
   acc.acc = heap_caps_calloc(1, acc.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   char *msgs = solo_build_messages_json(transcript);
   tab5_debug_obs_event("solo.llm_start", "");
   t0 = esp_timer_get_time();
   if (msgs && acc.acc) {
      err = openrouter_chat_stream(msgs, solo_delta_cb, &acc);
   } else {
      err = ESP_ERR_NO_MEM;
   }
   dt = (esp_timer_get_time() - t0) / 1000;
   snprintf(detail, sizeof detail, "ms=%lld", (long long)dt);
   if (s_cancel_requested) {
      tab5_debug_obs_event("solo.cancel", detail);
      ESP_LOGI(TAG, "solo audio canceled after LLM — skipping session append");
      free(msgs);
      heap_caps_free(acc.acc);
      goto done;
   }
   tab5_debug_obs_event(err == ESP_OK ? "solo.llm_done" : "solo.error", detail);
   free(msgs);
   if (err != ESP_OK) {
      ui_home_show_toast("Solo LLM failed");
      heap_caps_free(acc.acc);
      goto done;
   }
   /* Persist the user (transcript) + assistant turn now — even if
    * TTS fails below, the session log is intact. */
   solo_session_append("user", transcript);
   solo_session_append("assistant", acc.acc ? acc.acc : "");

   /* Step 3 — TTS the assistant text into the playback ring. */
   voice_set_state(VOICE_STATE_SPEAKING, "solo");
   tab5_debug_obs_event("solo.tts_start", "");
   t0 = esp_timer_get_time();
   /* Reset the 24→16 downsample carry between turns. */
   s_tts_carry_n = 0;
   err = openrouter_tts(acc.acc, solo_tts_chunk_cb, NULL);
   dt = (esp_timer_get_time() - t0) / 1000;
   snprintf(detail, sizeof detail, "ms=%lld", (long long)dt);
   tab5_debug_obs_event(err == ESP_OK ? "solo.tts_done" : "solo.error", detail);
   heap_caps_free(acc.acc);

done:
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
