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
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "settings.h"
#include "voice.h"
#include "voice_m5_llm.h"

static const char *TAG = "voice_wakeword";

#define WAKEWORD_TASK_STACK 12288
#define WAKEWORD_TASK_PRIO 4
#define WAKEWORD_DEFAULT_WAKE "hey tinker"
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
/* K144's sherpa-ncnn streaming zipformer consistently substitutes the
 * proper noun "tinker" with "thinker" (T → Th).  Verified live
 * 2026-05-18 — every "Hey Tinker" utterance came back as "hey thinker"
 * / "thinker" / "hay thinker".  Carry an auto-derived alt phrase so
 * a literal substring match still hits.  Set when wake_phrase contains
 * "tinker" (case-insensitive); cleared otherwise. */
static char s_wake_phrase_alt[64];
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

/* TT #578 — TinkerON debug surface: fire counter, last-fire bookkeeping,
 * cached callback/user so reconfigure can re-arm with the same wiring,
 * and a 32-entry ASR transcript ring for /tinkeron/transcripts. */
static uint32_t s_fire_count = 0;
/* TT #595 — VAD pre-gate skip counter.  Increments every time a
 * substring match would have fired but was suppressed because the
 * matching window was too short (likely an ASR hallucination on
 * silence).  Exposed via voice_wakeword_status() for tuning. */
static uint32_t s_vad_skip_count = 0;
static int64_t s_last_fire_us = 0;
static char s_last_match[64] = {0};
static voice_wakeword_cb_t s_cached_cb = NULL;
static void *s_cached_user = NULL;

#define TRANSCRIPT_RING_BYTES 32
static voice_wakeword_transcript_t s_trans_ring[TRANSCRIPT_RING_BYTES];
static size_t s_trans_ring_head = 0; /* next slot to write */
static size_t s_trans_ring_count = 0; /* entries populated (0..32) */
static portMUX_TYPE s_trans_lock = portMUX_INITIALIZER_UNLOCKED;

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
   /* TT #606: clamp add up front so the rest of the function never
    * exceeds WAKE_WINDOW_BYTES.  Old code only handled add > BYTES;
    * add == BYTES underflowed (WAKE_WINDOW_BYTES - add - 1) to
    * SIZE_MAX and overran into adjacent BSS, corrupting
    * s_wake_phrase_alt / s_end_phrase with TV transcript text. */
   if (add > WAKE_WINDOW_BYTES) add = WAKE_WINDOW_BYTES;

   /* Truncate from the left when we'd overflow — keep the most recent
    * WAKE_WINDOW_BYTES.  Cheaper than ring-buffer math for this size.
    * Uses (>=) so add == WAKE_WINDOW_BYTES forces a clean reset rather
    * than the underflow path. */
   if (s_wake_window_len + 1 + add > WAKE_WINDOW_BYTES) {
      size_t keep = (add + 1 >= WAKE_WINDOW_BYTES) ? 0 : (WAKE_WINDOW_BYTES - add - 1);
      if (keep > s_wake_window_len) keep = s_wake_window_len;
      if (keep > 0) memmove(s_wake_window, s_wake_window + s_wake_window_len - keep, keep);
      s_wake_window_len = keep;
   }
   if (s_wake_window_len > 0 && s_wake_window_len < WAKE_WINDOW_BYTES) {
      s_wake_window[s_wake_window_len++] = ' ';
   }
   /* Final defensive clamp — never write past s_wake_window[WAKE_WINDOW_BYTES]
    * regardless of how we got here. */
   if (s_wake_window_len + add > WAKE_WINDOW_BYTES) {
      add = WAKE_WINDOW_BYTES - s_wake_window_len;
   }
   if (add > 0) {
      memcpy(s_wake_window + s_wake_window_len, chunk, add);
      s_wake_window_len += add;
   }
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

/* TT #629 Wave C.4: monotonic counter of DICTATION_FINAL events fired since
 * boot.  Surfaced via /tinkeron/extpcm so the e2e harness + soak tests can
 * distinguish "wakeword is quiet" from "wakeword silently stuck in
 * LISTENING". */
static uint32_t s_dictation_final_count = 0;

static void finish_dictation(const char *reason) {
   if (s_state != ST_LISTENING) return;
   s_state = ST_IDLE;
   s_dictation_final_count++;
   char detail[48];
   snprintf(detail, sizeof(detail), "stop %s len=%u", reason, (unsigned)s_dict_buf_len);
   tab5_debug_obs_event("wakeword.dict", detail);
   ESP_LOGI(TAG, "dictation done (%s): %u bytes", reason, (unsigned)s_dict_buf_len);
   emit_event(VOICE_WAKEWORD_EVENT_DICTATION_FINAL, s_dict_buf ? s_dict_buf : "");
   wake_window_clear();
}

/* Self-wake suppression (2026-05-18): K144's mic is always on, so when
 * Tinker's TTS plays through Tab5's speaker the K144 hears it and the
 * ASR transcribes Tinker's own response.  If that response contains
 * the wake word ("I'm Tinker, here to help!" → "i'm thinker here..."),
 * the matcher fires wake mid-utterance and tries to start a NEW voice
 * turn while Tinker is still speaking.
 *
 * Quietest fix: suppress wake matching while Tab5's voice state is
 * NOT in a quiescent state (IDLE/READY/CONNECTING).  We also clear
 * the wake-window every time we suppress so a fragment captured
 * mid-non-quiescent doesn't pop a stale wake when we return to READY.
 *
 * Barge-in (saying "Hey Tinker" to interrupt TTS) is a separate
 * follow-up that needs voice_cancel() + voice_start_listening()
 * coordination; not in this fix. */
/* TT #131 stability 2026-05-20: tracks the moment voice state returned
 * to READY so we can enforce a post-turn grace period before wake can
 * re-fire.  Without this grace, K144 ASR's buffered transcripts from
 * during the turn (often containing fragments of Tinker's own TTS
 * playback) hit the matcher immediately on READY transition and
 * false-fire.  500-1000 ms is enough for the ASR engine's streaming
 * context to flush + Tab5's wake_window to settle on truly-new audio. */
static int64_t s_last_busy_us = 0;

/* TT #692 — post-cancel suppression deadline.  voice_cancel installs
 * a 3 s window after explicit user cancels so the matcher silently
 * drops wake events fired from K144 ASR partials that came out of
 * buffered audio (TTS tail or user follow-up speech).  Wakeword task
 * stays alive + armed throughout — no stop/restart cycle. */
static int64_t s_post_cancel_until_us = 0;
/* TT #627 Wave B.2 (R3) — bumped 1000 → 1500 ms.  Audit found that
 * K144's streaming-zipformer ASR can take 1.5-2 s to flush a long TTS
 * reply context after SPEAKING→READY.  1000 ms expired before the
 * flush completed, letting lingering "thinker" partials re-fire the
 * matcher.  1500 ms covers the median worst case observed in live
 * sessions.  Adaptive (wait for K144 post-SPEAKING silent finish) is
 * the correct fix but more invasive — bump first, revisit if still
 * firing falsely in the soak. */
#define WAKE_REARM_GRACE_MS 1500

/* TT #131 watchdog 2026-05-20: timestamp of the most-recent ASR
 * delta we received from K144.  Updated in asr_partial_cb.  The
 * watchdog task in voice_onboard.c polls voice_wakeword_last_delta_us()
 * and kicks /m5/reset if it goes stale (>20 s) while the pump is
 * actively sending frames — that pattern means K144's llm-asr
 * cycled out from under us and our cached asr_id is stale. */
static int64_t s_last_delta_us = 0;
int64_t voice_wakeword_last_delta_us(void) { return s_last_delta_us; }

static bool wakeword_suppressed_by_voice_state(void) {
   voice_state_t st = voice_get_state();
   /* TT #131 2026-05-20: matcher's alt phrase is now bare "thinker"
    * (one word, much more permissive than the old "hey thinker").
    * That makes barge-in unsafe — Tinker's own TTS reply commonly
    * contains "Tinker" → ASR transcribes "thinker" → matcher fires
    * → cancels its own reply.  Suppress wake during SPEAKING too so
    * the user gets a "fully idle before re-arm" UX as requested.
    * Net cost: lose mid-TTS barge-in.  Net gain: stability under the
    * sensitive ASR-based wake. */
   if (st == VOICE_STATE_LISTENING || st == VOICE_STATE_PROCESSING || st == VOICE_STATE_RECONNECTING ||
       st == VOICE_STATE_SPEAKING) {
      s_last_busy_us = esp_timer_get_time();
      return true;
   }
   /* Post-busy grace: wait WAKE_REARM_GRACE_MS after state returns to
    * READY before allowing wake to fire again.  Lets K144 ASR's
    * streaming-zipformer context flush its just-completed-turn frames
    * so the next match is against fresh post-turn audio only. */
   if (s_last_busy_us != 0) {
      int64_t since_busy_us = esp_timer_get_time() - s_last_busy_us;
      if (since_busy_us < (int64_t)WAKE_REARM_GRACE_MS * 1000) {
         return true;
      }
      s_last_busy_us = 0; /* grace expired — re-armed */
   }
   /* TT #692: explicit post-cancel suppression window.  voice_cancel
    * sets this when the user (or any other cancel-class caller) ends
    * a turn.  Even if state is already READY and the busy-grace has
    * expired, drop wake hits until the deadline. */
   if (s_post_cancel_until_us != 0) {
      int64_t now = esp_timer_get_time();
      if (now < s_post_cancel_until_us) return true;
      s_post_cancel_until_us = 0; /* window expired */
   }
   return false;
}

void voice_wakeword_post_cancel_suppress_ms(uint32_t ms) {
   if (ms == 0) {
      s_post_cancel_until_us = 0;
      return;
   }
   s_post_cancel_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
   /* Also reset the wake window so any partial transcript currently
    * buffered (potentially containing TTS-tail "thinker" fragments)
    * can't match the moment the window expires. */
   wake_window_clear();
   tab5_debug_obs_event("wakeword.suppress", "post_cancel_arm");
}

/* TT #578: push every ASR delta into the debug ring buffer.  Cheap —
 * 32-entry FIFO in static storage.  GET /tinkeron/transcripts pulls
 * the latest N. */
static void transcript_ring_push(const char *delta, bool finish) {
   if (delta == NULL) return;
   taskENTER_CRITICAL(&s_trans_lock);
   voice_wakeword_transcript_t *slot = &s_trans_ring[s_trans_ring_head];
   slot->ms = esp_timer_get_time() / 1000;
   slot->finish = finish;
   strncpy(slot->text, delta, sizeof(slot->text) - 1);
   slot->text[sizeof(slot->text) - 1] = '\0';
   s_trans_ring_head = (s_trans_ring_head + 1) % TRANSCRIPT_RING_BYTES;
   if (s_trans_ring_count < TRANSCRIPT_RING_BYTES) s_trans_ring_count++;
   taskEXIT_CRITICAL(&s_trans_lock);
}

static void asr_partial_cb(const char *delta, bool finish, void *user) {
   (void)user;

   /* TT #131 watchdog: timestamp ANY delta arrival (including empty
    * "finish" markers).  Even a finish=true with no text confirms
    * K144's llm-asr is alive and dispatching to us. */
   s_last_delta_us = esp_timer_get_time();

   /* TT #698 — drop consecutive identical non-finish partials.  K144's
    * sherpa-ncnn streaming-zipformer rolling decoder re-emits the SAME
    * partial 5-20× per audio frame (live-observed 10 deltas/sec all
    * carrying identical 80-char text).  Without this gate every
    * duplicate runs the matcher (7× istrstr), pushes the transcript
    * ring (taskENTER_CRITICAL + strncpy), and pushes the wake window
    * (memcpy + bounds checks) for zero new signal.
    *
    * Gate intentionally lets finish=true through (boundary marker) and
    * empty deltas through (silence/heartbeat).  Watchdog timestamp
    * above still fires so K144-aliveness detection isn't affected. */
   static char s_last_delta[96] = {0};
   if (delta != NULL && delta[0] != '\0' && !finish && strncmp(s_last_delta, delta, sizeof(s_last_delta) - 1) == 0) {
      return;
   }
   if (delta != NULL && delta[0] != '\0') {
      strncpy(s_last_delta, delta, sizeof(s_last_delta) - 1);
      s_last_delta[sizeof(s_last_delta) - 1] = '\0';
   } else if (finish) {
      s_last_delta[0] = '\0'; /* segment closed — next partial is fresh */
   }

   /* TT #578: every delta into the debug ring before any state branching. */
   if (delta && delta[0]) transcript_ring_push(delta, finish);

   if (s_state == ST_IDLE) {
      /* Suppress matching while Tinker is mid-turn — K144 hears its
       * own TTS playback and would re-fire wake on every reply that
       * contains "tinker" / "thinker". */
      if (wakeword_suppressed_by_voice_state()) {
         wake_window_clear();
         return;
      }
      /* Push delta into sliding window AND check end-of-segment finish
       * boundary for phrase match.  Match on every push so we catch
       * mid-segment wakes too. */
      if (delta && delta[0]) wake_window_push(delta);
      if (s_emit_bg && delta && delta[0]) {
         emit_event(VOICE_WAKEWORD_EVENT_TRANSCRIPT, delta);
      }
      /* TT #131 2026-05-20: K144's sherpa-ncnn streaming-zipformer-20M
       * is INCONSISTENT in how it transcribes "Hey Tinker" — observed
       * renderings across sessions: "thinker", "hick", "hicker",
       * "hanker", "any hanker thinker", "i'm thinker".  To make the
       * wake reliable we match against a small set of patterns that
       * all map to "user said something that sounds like Hey Tinker".
       *
       * The 8-char VAD pre-gate (below) keeps short hallucinations
       * out — "hick" alone in a 5-char window won't fire; "hick"
       * embedded in a longer window will.  Self-wake during TTS is
       * already suppressed by voice_state.
       *
       * Order matters: try the longest/most-specific first so the
       * match-detail surfaces the best signal. */
      static const char *const k_alt_patterns[] = {
          "thinker", /* T→Th substitution — most common rendering */
          "hicker",  /* contracted "Hey Tinker" */
          "tinker",  /* exact (rare — model usually substitutes) */
          "hick",    /* heavily-contracted rendering, real session 2026-05-20 */
          "hanker",  /* observed in "any hanker thinker" rendering */
      };
      /* TT #692 — anchored matching.  The wake substring must appear in
       * the FRESHLY-ARRIVED delta (not somewhere deep in an accumulated
       * sliding window), AND its position must be within the first half
       * of the delta (allows leading filler like "uh hey tinker" but
       * rejects long buffered partials whose tail happens to contain
       * "thinker" from TTS bleed or background noise).  K144's sherpa-
       * ncnn streaming-zipformer is a rolling decoder — each partial
       * is the cumulative recognition of the current segment, so a real
       * wake utterance lands at the start of the delta that contains
       * its trailing audio. */
      const char *match = NULL;
      const char *match_pos = NULL;
      if (delta && delta[0]) {
         const char *p;
         if ((p = istrstr(delta, s_wake_phrase)) != NULL) {
            match = s_wake_phrase;
            match_pos = p;
         } else if (s_wake_phrase_alt[0] && (p = istrstr(delta, s_wake_phrase_alt)) != NULL) {
            match = s_wake_phrase_alt;
            match_pos = p;
         } else {
            for (size_t i = 0; i < sizeof(k_alt_patterns) / sizeof(k_alt_patterns[0]); i++) {
               if ((p = istrstr(delta, k_alt_patterns[i])) != NULL) {
                  match = k_alt_patterns[i];
                  match_pos = p;
                  break;
               }
            }
         }
         if (match != NULL && match_pos != NULL) {
            size_t pos = (size_t)(match_pos - delta);
            size_t delta_len = strlen(delta);
            /* Position floor: match must be in the first half of the
             * delta + a 4-char slack for short leading fillers.  Rejects
             * cases where a long partial's tail contains the wake word
             * (the TTS-bleed / buffered-noise failure mode). */
            size_t pos_ceiling = delta_len / 2 + 4;
            if (pos > pos_ceiling) {
               ESP_LOGD(TAG, "wake suppressed by anchor: pos=%u of %u in \"%s\"", (unsigned)pos, (unsigned)delta_len,
                        delta);
               match = NULL;
               match_pos = NULL;
            }
         }
      }
      if (match != NULL) {
         /* TT #595 + TT #692 — VAD pre-gate: require the partial
          * delta to be ≥8 chars before a substring match is allowed
          * to fire wake.  K144's sherpa-ncnn streaming ASR confabulates
          * short 1-2 word fragments from background noise during silence
          * ("kincher", "ereb", "thinker") — those would substring-match
          * "thinker" but contain only that noise.  A real "Hey Tinker"
          * utterance produces a partial with leading context, so the
          * 8-char floor filters hallucinations without losing real wakes.
          * Now applied to the freshly-arrived delta (anchored matcher). */
         size_t delta_len = (delta && delta[0]) ? strlen(delta) : 0;
         if (delta_len < 8) {
            ESP_LOGD(TAG, "wake suppressed by VAD pregate: delta=\"%s\" (%u chars)", delta ? delta : "",
                     (unsigned)delta_len);
            s_vad_skip_count++;
         } else {
            ESP_LOGI(TAG, "wake matched \"%s\" in delta=\"%s\"", match, delta);
            tab5_debug_obs_event("wakeword.fire", match);
            /* TT #578: bookkeeping for /tinkeron/status. */
            s_fire_count++;
            s_last_fire_us = esp_timer_get_time();
            strncpy(s_last_match, match, sizeof(s_last_match) - 1);
            s_last_match[sizeof(s_last_match) - 1] = '\0';
            enter_listening();
         }
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

   /* TT #131 2026-05-20: ASR consistently transcribes "Hey Tinker" as
    * jumbled noise containing the substring "thinker" — e.g. "think
    * of any hanker thinker agathip kirkique".  The full-phrase alt
    * "hey thinker" rarely matches because ASR drops/mangles the
    * leading "hey".  Use just "thinker" as alt — the VAD pre-gate
    * (8-char window minimum) still filters single-word noise
    * hallucinations, and self-wake during TTS is already suppressed
    * by voice_state checks.  Net: catches the proper-noun token
    * the ASR consistently emits, even when surrounded by garbage. */
   if (istrstr(s_wake_phrase, "tinker") != NULL) {
      strncpy(s_wake_phrase_alt, "thinker", sizeof(s_wake_phrase_alt) - 1);
      s_wake_phrase_alt[sizeof(s_wake_phrase_alt) - 1] = '\0';
   } else {
      s_wake_phrase_alt[0] = '\0';
   }
   strncpy(s_end_phrase, ep, sizeof(s_end_phrase) - 1);
   s_end_phrase[sizeof(s_end_phrase) - 1] = '\0';
   s_dict_buf_cap = (cfg && cfg->dictation_buf_bytes > 0) ? cfg->dictation_buf_bytes : WAKEWORD_DEFAULT_BUF_BYTES;
   s_silence_segments_to_stop =
       (cfg && cfg->silence_segments_to_stop > 0) ? cfg->silence_segments_to_stop : WAKEWORD_DEFAULT_SILENCE_SEGMENTS;
   s_dict_timeout_s = (cfg && cfg->dictation_timeout_s > 0) ? cfg->dictation_timeout_s : WAKEWORD_DEFAULT_TIMEOUT_S;
   s_emit_bg = (cfg && cfg->emit_background_transcripts);
   s_cb = cb;
   s_user = user;
   /* TT #578: cache for voice_wakeword_reconfigure_phrase() so /tinkeron/
    * wake_phrase can restart with the same callback wiring. */
   s_cached_cb = cb;
   s_cached_user = user;
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

   /* TT #131 2026-05-20: wake_src=ext_pcm now uses the ASR variant
    * (Tab5 mic → K144 main_asr → utf-8 stream → Tab5 string-matcher).
    * KWS variant proved unreliable — even at 0.02 threshold + 5
    * parameter-name-variants + clean voice (RMS 3685+) the
    * sherpa-onnx-kws-zipformer-gigaspeech model wouldn't fire.  The
    * ASR path is acoustically more robust (full sequence-to-sequence
    * transducer vs per-token keyword spotter) AND has live-verified
    * history of producing real transcripts from Tab5-mic audio
    * (K144 daemon journal Aug 22 12:17/12:20 from commit 88fbf14).
    * Tab5's voice_wakeword recv loop already handles both shapes
    * via the is_kws flag — wakeword_setup_tab5_mic doesn't set
    * is_kws so the matcher does string-match on the transcript
    * stream (catches "hey tinker" + "hey thinker" — K144's ASR
    * consistently substitutes T → Th on this phrase). */
   esp_err_t err;
   if (tab5_settings_wake_src_is("ext_pcm")) {
      err = voice_m5_llm_wakeword_setup_tab5_mic(&s_handle, &s_stop_flag);
   } else {
      err = voice_m5_llm_wakeword_setup(&s_handle, &s_stop_flag);
   }
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "ASR chain setup failed: %s", esp_err_to_name(err));
      char detail[48];
      snprintf(detail, sizeof(detail), "fail %s", esp_err_to_name(err));
      tab5_debug_obs_event("wakeword.start", detail);
      s_handle = NULL;
      return err;
   }

   /* TT #131 stability: PSRAM-back the 12 KB task stack via WithCaps.
    * Internal SRAM is tight (~56 KB largest-free at boot); a 12 KB
    * stack here on top of ext_pcm's 8 KB pushed the heap into
    * heap_wd's "sram_exhausted" threshold under sustained operation. */
   BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(wakeword_task, "wakeword", WAKEWORD_TASK_STACK, NULL,
                                                   WAKEWORD_TASK_PRIO, &s_task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
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

const char *voice_wakeword_asr_id(void) { return voice_m5_llm_wakeword_asr_id(s_handle); }

void voice_wakeword_force_dictation_stop(void) { s_force_dict_stop = true; }

/* ── TT #578 — TinkerON debug-surface accessors ───────────────────── */

void voice_wakeword_status(voice_wakeword_status_t *out) {
   if (out == NULL) return;
   out->armed = voice_wakeword_is_active();
   strncpy(out->wake_phrase, s_wake_phrase, sizeof(out->wake_phrase) - 1);
   out->wake_phrase[sizeof(out->wake_phrase) - 1] = '\0';
   strncpy(out->wake_phrase_alt, s_wake_phrase_alt, sizeof(out->wake_phrase_alt) - 1);
   out->wake_phrase_alt[sizeof(out->wake_phrase_alt) - 1] = '\0';
   strncpy(out->end_phrase, s_end_phrase, sizeof(out->end_phrase) - 1);
   out->end_phrase[sizeof(out->end_phrase) - 1] = '\0';
   out->fire_count = s_fire_count;
   out->vad_skip_count = s_vad_skip_count;
   out->last_fire_ms = s_last_fire_us / 1000;
   strncpy(out->last_match, s_last_match, sizeof(out->last_match) - 1);
   out->last_match[sizeof(out->last_match) - 1] = '\0';
}

esp_err_t voice_wakeword_reconfigure_phrase(const char *new_phrase) {
   if (new_phrase == NULL || new_phrase[0] == '\0') return ESP_ERR_INVALID_ARG;
   /* Need the cached cb/user from the last start so we can preserve UI
    * wiring across the restart.  If never started, bail. */
   if (s_cached_cb == NULL && s_cached_user == NULL) {
      /* Allow user==NULL but require at least one prior start to have
       * happened — detect via s_wake_phrase being non-empty (cleared
       * never). */
      if (s_wake_phrase[0] == '\0') return ESP_ERR_INVALID_STATE;
   }
   voice_wakeword_cb_t cb = s_cached_cb;
   void *user = s_cached_user;
   voice_wakeword_stop();
   voice_wakeword_config_t cfg = {
      .wake_phrase = new_phrase,
      .end_phrase = s_end_phrase[0] ? s_end_phrase : NULL,
      .dictation_buf_bytes = s_dict_buf_cap,
      .silence_segments_to_stop = s_silence_segments_to_stop,
      .dictation_timeout_s = s_dict_timeout_s,
      .emit_background_transcripts = s_emit_bg,
   };
   return voice_wakeword_start(&cfg, cb, user);
}

/* TT #629 Wave C.4: small observability getters surfaced via /tinkeron/extpcm
 * for the e2e harness + soak test.  Wakeword internal state machine is
 * normally invisible — these expose its shape without leaking the enum
 * to other modules. */
int voice_wakeword_get_state_value(void) { return (int)s_state; }

const char *voice_wakeword_get_state_name(void) {
   switch (s_state) {
      case ST_IDLE:
         return "IDLE";
      case ST_LISTENING:
         return "LISTENING";
      default:
         return "?";
   }
}

int64_t voice_wakeword_get_ms_since_busy(void) {
   if (s_last_busy_us == 0) return -1;
   return (esp_timer_get_time() - s_last_busy_us) / 1000;
}

uint32_t voice_wakeword_get_dictation_final_count(void) { return s_dictation_final_count; }

size_t voice_wakeword_get_recent_transcripts(voice_wakeword_transcript_t *out, size_t max) {
   if (out == NULL || max == 0) return 0;
   size_t n;
   taskENTER_CRITICAL(&s_trans_lock);
   n = s_trans_ring_count < max ? s_trans_ring_count : max;
   /* Copy newest-last (oldest-first within the n window).  Head points
    * to next-to-write, so oldest = head - count, wrapping. */
   size_t start = (s_trans_ring_head + TRANSCRIPT_RING_BYTES - s_trans_ring_count)
                  % TRANSCRIPT_RING_BYTES;
   size_t skip = s_trans_ring_count - n;  /* drop oldest if caller wants fewer */
   start = (start + skip) % TRANSCRIPT_RING_BYTES;
   for (size_t i = 0; i < n; i++) {
      size_t idx = (start + i) % TRANSCRIPT_RING_BYTES;
      out[i] = s_trans_ring[idx];
   }
   taskEXIT_CRITICAL(&s_trans_lock);
   return n;
}
