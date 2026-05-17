/**
 * @file voice_wakeword.c
 * @brief Always-on K144 ASR + Tab5 phrase-match state machine.
 *
 * See voice_wakeword.h for the architecture.  This file owns:
 *   - The K144 chain handle (audio + asr)
 *   - The drain task (long-lived FreeRTOS task)
 *   - The 2-state matcher: IDLE → LISTENING → IDLE
 *   - The dictation buffer (PSRAM, default 32 KB)
 *
 * Thread-safety: callback fires from the wakeword task only.  The
 * `voice_wakeword_force_dictation_stop` API is the one cross-thread
 * entry — sets a flag the task polls.
 */

#include "voice_wakeword.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "debug_obs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_m5_llm.h"

static const char *TAG = "voice_wakeword";

#define WAKEWORD_TASK_STACK 12288
#define WAKEWORD_TASK_PRIO 4
#define WAKEWORD_DEFAULT_WAKE "tinker"
#define WAKEWORD_DEFAULT_END "save note"
#define WAKEWORD_DEFAULT_BUF_BYTES (32 * 1024)
#define WAKEWORD_DEFAULT_SILENCE_SEGMENTS 3
#define WAKEWORD_DEFAULT_TIMEOUT_S 14400 /* 4 hours, matches #573 dictation cap */

/* Sliding window for IDLE-state wake-phrase matching.  Just big enough
 * to catch a 2-3-word phrase across segment boundaries — the asr unit
 * sometimes splits "hey tinker" across two finish=true frames. */
#define WAKE_WINDOW_BYTES 96

typedef enum { ST_IDLE, ST_LISTENING } wakeword_state_t;

/* Module state — single global instance (only one mic). */
static voice_m5_wakeword_handle_t *s_handle = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_stop_flag = false;
static volatile bool s_force_dict_stop = false;
static voice_wakeword_cb_t s_cb = NULL;
static void *s_user = NULL;

static char s_wake_phrase[64];
static char s_end_phrase[64];
static size_t s_dict_buf_cap = 0;
static uint8_t s_silence_segments_to_stop = 0;
static uint32_t s_dict_timeout_s = 0;
static bool s_emit_bg = false;

static char *s_dict_buf = NULL;
static size_t s_dict_buf_len = 0;
static wakeword_state_t s_state = ST_IDLE;
static int64_t s_listening_started_us = 0;
static uint8_t s_silence_segments_seen = 0;
static char s_wake_window[WAKE_WINDOW_BYTES + 1];
static size_t s_wake_window_len = 0;

/* Case-insensitive substring search.  Returns pointer into haystack on
 * hit, NULL on miss.  Both strings expected to be UTF-8 ASCII for this
 * use case — wake phrases are English and the K144 zipformer emits
 * uppercase transcripts. */
static const char *istrstr(const char *haystack, const char *needle) {
   if (haystack == NULL || needle == NULL || needle[0] == '\0') return haystack;
   size_t nlen = strlen(needle);
   for (const char *p = haystack; *p; ++p) {
      size_t i;
      for (i = 0; i < nlen; i++) {
         unsigned char a = (unsigned char)p[i];
         unsigned char b = (unsigned char)needle[i];
         if (a == 0) return NULL;
         if (tolower(a) != tolower(b)) break;
      }
      if (i == nlen) return p;
   }
   return NULL;
}

static void emit_event(voice_wakeword_event_t ev, const char *text) {
   if (s_cb != NULL) s_cb(ev, text ? text : "", s_user);
}

static void wake_window_push(const char *chunk) {
   if (chunk == NULL || chunk[0] == '\0') return;
   size_t add = strlen(chunk);
   /* Truncate from the left when we'd overflow — keep the most recent
    * WAKE_WINDOW_BYTES.  Cheaper than ring-buffer math for this size. */
   if (s_wake_window_len + 1 + add > WAKE_WINDOW_BYTES) {
      size_t keep = (add > WAKE_WINDOW_BYTES) ? 0 : (WAKE_WINDOW_BYTES - add - 1);
      if (keep > s_wake_window_len) keep = s_wake_window_len;
      if (keep > 0) memmove(s_wake_window, s_wake_window + s_wake_window_len - keep, keep);
      s_wake_window_len = keep;
   }
   if (s_wake_window_len > 0) {
      s_wake_window[s_wake_window_len++] = ' ';
   }
   if (add > WAKE_WINDOW_BYTES) add = WAKE_WINDOW_BYTES;
   memcpy(s_wake_window + s_wake_window_len, chunk, add);
   s_wake_window_len += add;
   s_wake_window[s_wake_window_len] = '\0';
}

static void wake_window_clear(void) {
   s_wake_window_len = 0;
   s_wake_window[0] = '\0';
}

static void dict_buf_reset(void) {
   if (s_dict_buf != NULL) s_dict_buf[0] = '\0';
   s_dict_buf_len = 0;
}

static void dict_buf_append(const char *chunk) {
   if (chunk == NULL || chunk[0] == '\0') return;
   size_t add = strlen(chunk);
   /* Leave room for a separating space + NUL. */
   size_t need_space = (s_dict_buf_len > 0) ? 1 : 0;
   if (s_dict_buf_len + need_space + add + 1 > s_dict_buf_cap) {
      ESP_LOGW(TAG, "dictation buffer full at %u bytes — truncating", (unsigned)s_dict_buf_len);
      return;
   }
   if (need_space) {
      s_dict_buf[s_dict_buf_len++] = ' ';
   }
   memcpy(s_dict_buf + s_dict_buf_len, chunk, add);
   s_dict_buf_len += add;
   s_dict_buf[s_dict_buf_len] = '\0';
}

static void enter_listening(void) {
   s_state = ST_LISTENING;
   s_silence_segments_seen = 0;
   s_listening_started_us = esp_timer_get_time();
   dict_buf_reset();
   wake_window_clear();
   tab5_debug_obs_event("wakeword.dict", "start");
   emit_event(VOICE_WAKEWORD_EVENT_WAKE, s_wake_phrase);
}

static void finish_dictation(const char *reason) {
   if (s_state != ST_LISTENING) return;
   s_state = ST_IDLE;
   char detail[48];
   snprintf(detail, sizeof(detail), "stop %s len=%u", reason, (unsigned)s_dict_buf_len);
   tab5_debug_obs_event("wakeword.dict", detail);
   ESP_LOGI(TAG, "dictation done (%s): %u bytes", reason, (unsigned)s_dict_buf_len);
   emit_event(VOICE_WAKEWORD_EVENT_DICTATION_FINAL, s_dict_buf ? s_dict_buf : "");
   wake_window_clear();
}

static void asr_partial_cb(const char *delta, bool finish, void *user) {
   (void)user;

   if (s_state == ST_IDLE) {
      /* Push delta into sliding window AND check end-of-segment finish
       * boundary for phrase match.  Match on every push so we catch
       * mid-segment wakes too. */
      if (delta && delta[0]) wake_window_push(delta);
      if (s_emit_bg && delta && delta[0]) {
         emit_event(VOICE_WAKEWORD_EVENT_TRANSCRIPT, delta);
      }
      if (s_wake_window_len > 0 && istrstr(s_wake_window, s_wake_phrase) != NULL) {
         ESP_LOGI(TAG, "wake matched \"%s\" in \"%s\"", s_wake_phrase, s_wake_window);
         tab5_debug_obs_event("wakeword.fire", s_wake_phrase);
         enter_listening();
      } else if (finish) {
         /* Segment closed without match — clear the window so the next
          * unrelated segment doesn't carry a stale fragment forward. */
         wake_window_clear();
      }
      return;
   }

   /* ST_LISTENING — every delta goes into the dictation buffer. */
   if (s_force_dict_stop) {
      s_force_dict_stop = false;
      finish_dictation("forced");
      return;
   }

   if (delta && delta[0]) {
      dict_buf_append(delta);
      s_silence_segments_seen = 0;
      emit_event(VOICE_WAKEWORD_EVENT_DICTATION_PARTIAL, delta);

      /* End-phrase match: scan the LATEST chunk only — the buffer can
       * get huge and we want to react within one segment. */
      if (s_end_phrase[0] && istrstr(delta, s_end_phrase) != NULL) {
         finish_dictation("end_phrase");
         return;
      }
   }

   if (finish) {
      /* Segment ended.  If this was a silent segment, count toward
       * auto-stop. */
      if (!delta || delta[0] == '\0') {
         s_silence_segments_seen++;
         if (s_silence_segments_seen >= s_silence_segments_to_stop) {
            finish_dictation("silence");
            return;
         }
      }
   }

   /* Hard timeout safety net. */
   if (s_dict_timeout_s > 0) {
      int64_t age_s = (esp_timer_get_time() - s_listening_started_us) / 1000000;
      if (age_s >= (int64_t)s_dict_timeout_s) {
         finish_dictation("timeout");
      }
   }
}

static void wakeword_task(void *arg) {
   (void)arg;
   ESP_LOGI(TAG, "wakeword task started; draining asr stream");
   tab5_debug_obs_event("wakeword.task", "start");

   esp_err_t err = voice_m5_llm_wakeword_run(s_handle, asr_partial_cb, NULL, &s_stop_flag, 0);
   ESP_LOGI(TAG, "wakeword run exited: %s", esp_err_to_name(err));
   tab5_debug_obs_event("wakeword.task", "stop");

   s_task = NULL;
   vTaskDelete(NULL);
}

esp_err_t voice_wakeword_start(const voice_wakeword_config_t *cfg, voice_wakeword_cb_t cb, void *user) {
   if (s_handle != NULL || s_task != NULL) return ESP_ERR_INVALID_STATE;

   /* Cache config. */
   const char *wp = (cfg && cfg->wake_phrase && cfg->wake_phrase[0]) ? cfg->wake_phrase : WAKEWORD_DEFAULT_WAKE;
   const char *ep = (cfg && cfg->end_phrase) ? cfg->end_phrase : WAKEWORD_DEFAULT_END;
   strncpy(s_wake_phrase, wp, sizeof(s_wake_phrase) - 1);
   s_wake_phrase[sizeof(s_wake_phrase) - 1] = '\0';
   strncpy(s_end_phrase, ep, sizeof(s_end_phrase) - 1);
   s_end_phrase[sizeof(s_end_phrase) - 1] = '\0';
   s_dict_buf_cap = (cfg && cfg->dictation_buf_bytes > 0) ? cfg->dictation_buf_bytes : WAKEWORD_DEFAULT_BUF_BYTES;
   s_silence_segments_to_stop =
       (cfg && cfg->silence_segments_to_stop > 0) ? cfg->silence_segments_to_stop : WAKEWORD_DEFAULT_SILENCE_SEGMENTS;
   s_dict_timeout_s = (cfg && cfg->dictation_timeout_s > 0) ? cfg->dictation_timeout_s : WAKEWORD_DEFAULT_TIMEOUT_S;
   s_emit_bg = (cfg && cfg->emit_background_transcripts);
   s_cb = cb;
   s_user = user;
   s_stop_flag = false;
   s_force_dict_stop = false;
   s_state = ST_IDLE;
   wake_window_clear();

   /* Allocate dictation buffer once — survives wake/dictation cycles. */
   if (s_dict_buf == NULL) {
      s_dict_buf = heap_caps_calloc(1, s_dict_buf_cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (s_dict_buf == NULL) {
         ESP_LOGE(TAG, "dictation buffer alloc failed (%u bytes)", (unsigned)s_dict_buf_cap);
         return ESP_ERR_NO_MEM;
      }
   }
   s_dict_buf[0] = '\0';
   s_dict_buf_len = 0;

   ESP_LOGI(TAG, "starting K144 always-on ASR: wake=\"%s\" end=\"%s\"", s_wake_phrase, s_end_phrase);
   tab5_debug_obs_event("wakeword.start", s_wake_phrase);

   esp_err_t err = voice_m5_llm_wakeword_setup(&s_handle, &s_stop_flag);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "ASR chain setup failed: %s", esp_err_to_name(err));
      char detail[48];
      snprintf(detail, sizeof(detail), "fail %s", esp_err_to_name(err));
      tab5_debug_obs_event("wakeword.start", detail);
      s_handle = NULL;
      return err;
   }

   BaseType_t ok = xTaskCreate(wakeword_task, "wakeword", WAKEWORD_TASK_STACK, NULL, WAKEWORD_TASK_PRIO, &s_task);
   if (ok != pdPASS) {
      ESP_LOGE(TAG, "wakeword task spawn failed");
      voice_m5_llm_wakeword_teardown(s_handle);
      s_handle = NULL;
      return ESP_ERR_NO_MEM;
   }
   return ESP_OK;
}

void voice_wakeword_stop(void) {
   if (s_task == NULL && s_handle == NULL) return;
   ESP_LOGI(TAG, "stopping wakeword chain");
   tab5_debug_obs_event("wakeword.stop", "");
   s_stop_flag = true;
   /* Give the task ~1 s to drop out of its 100 ms UART-recv loop. */
   for (int i = 0; i < 20 && s_task != NULL; i++) {
      vTaskDelay(pdMS_TO_TICKS(50));
   }
   if (s_handle != NULL) {
      voice_m5_llm_wakeword_teardown(s_handle);
      s_handle = NULL;
   }
}

bool voice_wakeword_is_active(void) { return s_handle != NULL && s_task != NULL; }

void voice_wakeword_force_dictation_stop(void) { s_force_dict_stop = true; }
