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
#include "openrouter_client.h"
#include "task_worker.h"
#include "ui_chat.h"
#include "ui_home.h"
#include "voice.h"

static const char *TAG = "voice_solo";

static bool s_initialized = false;
static volatile bool s_busy = false;

/* Single-flight accumulator — we never overlap two solo turns
 * (s_busy guards the entry path).  PSRAM-backed so a long reply
 * doesn't fragment internal SRAM. */
typedef struct {
   char *acc;
   size_t cap;
   size_t len;
} solo_chat_acc_t;

static void solo_delta_cb(const char *delta, size_t n, void *vctx) {
   solo_chat_acc_t *a = vctx;
   if (a->len + n + 1 < a->cap) {
      memcpy(a->acc + a->len, delta, n);
      a->len += n;
      a->acc[a->len] = '\0';
   }
   /* Stream into chat UI as deltas arrive.  ui_chat_update_last_message
    * replaces the trailing assistant bubble's text every chunk. */
   ui_chat_update_last_message(a->acc);
}

/* Build [{"role":"user","content":<text>}].  Caller frees the
 * returned malloc'd JSON string with free(). */
static char *solo_build_messages_json(const char *user_text) {
   cJSON *arr = cJSON_CreateArray();
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON_AddStringToObject(m, "content", user_text);
   cJSON_AddItemToArray(arr, m);
   char *s = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return s;
}

/* Job function — runs on the shared tab5_worker.  arg is a malloc'd
 * copy of the user text; we own freeing it.  No vTaskSuspend / Delete
 * here since tab5_worker hosts us. */
static void solo_send_text_job(void *arg) {
   char *text = arg;
   s_busy = true;

   voice_set_state(VOICE_STATE_PROCESSING, "solo");
   tab5_debug_obs_event("solo.llm_start", "");

   /* Push the user turn + an empty assistant placeholder so the
    * delta callback can update_last_message into a fresh bubble. */
   ui_chat_push_message("user", text);
   ui_chat_push_message("assistant", "");

   solo_chat_acc_t acc = {0};
   acc.cap = 8192;
   acc.acc = heap_caps_calloc(1, acc.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

   char *msgs_json = solo_build_messages_json(text);
   int64_t t0 = esp_timer_get_time();
   esp_err_t err = ESP_ERR_NO_MEM;
   if (msgs_json && acc.acc) {
      err = openrouter_chat_stream(msgs_json, solo_delta_cb, &acc);
   }
   int64_t dt = (esp_timer_get_time() - t0) / 1000;
   char detail[48];
   snprintf(detail, sizeof detail, "ms=%lld", (long long)dt);
   tab5_debug_obs_event(err == ESP_OK ? "solo.llm_done" : "solo.error", detail);

   if (err != ESP_OK) {
      ESP_LOGE(TAG, "solo LLM failed: %s", esp_err_to_name(err));
      ui_home_show_toast("Solo LLM failed");
   }

   free(msgs_json);
   heap_caps_free(acc.acc);
   free(text);

   voice_set_state(VOICE_STATE_READY, "solo");
   s_busy = false;
}

esp_err_t voice_solo_init(void) {
   if (s_initialized) return ESP_OK;
   /* Follow-up commits: solo_session_init, solo_rag_init. */
   s_initialized = true;
   ESP_LOGI(TAG, "voice_solo_init OK");
   return ESP_OK;
}

esp_err_t voice_solo_send_text(const char *text) {
   if (!text || !*text) return ESP_ERR_INVALID_ARG;
   if (s_busy) return ESP_ERR_INVALID_STATE;

   char *copy = strdup(text);
   if (!copy) return ESP_ERR_NO_MEM;

   esp_err_t e = tab5_worker_enqueue(solo_send_text_job, copy, "solo_text");
   if (e != ESP_OK) {
      free(copy);
      return e;
   }
   return ESP_OK;
}

esp_err_t voice_solo_send_audio(int16_t *pcm, size_t samples) {
   if (!pcm) return ESP_ERR_INVALID_ARG;
   /* Wired into the STT/LLM/TTS chain in a follow-up commit. */
   ESP_LOGW(TAG, "send_audio stub — samples=%u", (unsigned)samples);
   heap_caps_free(pcm);
   return ESP_ERR_NOT_SUPPORTED;
}

void voice_solo_cancel(void) {
   openrouter_cancel_inflight();
   /* s_busy clears in the job's normal cleanup path; cancel just
    * forces the in-flight HTTP to break out faster. */
}

bool voice_solo_busy(void) {
   return s_busy;
}
