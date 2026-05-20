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
#include "freertos/idf_additions.h"  /* xTaskCreatePinnedToCoreWithCaps for watchdog */
#include "freertos/task.h"
#include "settings.h"            /* tab5_settings_get_mic_mute (Wave 7) */
#include "task_worker.h"         /* tab5_worker_enqueue */
#include "uart_port_c.h"         /* tab5_port_c_uart_get_baud — TT #131 baud bump check */
#include "ui_audio_cues.h"       /* ui_audio_cue_play — wake chime (#131-opt2) */
#include "ui_chat.h"             /* ui_chat_add_message */
#include "ui_core.h"             /* tab5_ui_try_lock / tab5_ui_unlock */
#include "ui_home.h"             /* ui_home_show_toast */
#include "voice.h"               /* voice_set_state, VOICE_STATE_* */
#include "voice_m5_llm.h"        /* probe / infer / chain_* */
#include "voice_ext_pcm_stream.h" /* TT #131: auto-arm pump on boot */
#include "voice_messages_sync.h"  /* W3-C-c: Dragon canonical message store */
#include "voice_wakeword.h"

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

/* Wave 13 — auto-retry from UNAVAILABLE.  When mark_k144_unavailable()
 * fires, schedule a one-shot 60 s timer that calls
 * voice_onboard_reset_failover().  Cap retries at 3 per Tab5 boot —
 * after that, the state stays sticky and the banner asks the user for
 * a power-cycle.  Counter resets on Tab5 reboot (static int, no NVS).
 *
 * IMPORTANT — uses esp_timer, NOT FreeRTOS xTimerCreate.  An earlier
 * draft used xTimerCreate which forced a FreeRTOS timer-service task
 * stack alloc (16 KB calloc from MALLOC_CAP_INTERNAL); under boot-time
 * SRAM pressure the calloc fails and trips
 * `vApplicationGetTimerTaskMemory: pxStackBufferTemp != NULL` — same
 * boot-loop class as the Wave 11 BSS-static incident.  esp_timer
 * shares one global dispatcher task so adding new timers stays cheap. */
#define M5_AUTO_RETRY_DELAY_US (60ULL * 1000ULL * 1000ULL) /* 60 s */
#define M5_AUTO_RETRY_MAX 3
static esp_timer_handle_t s_auto_retry_timer = NULL;
static int s_auto_retry_count = 0;
static volatile bool s_auto_retry_exhausted = false;
/* TT #131 — when ext_pcm is running at high baud, the auto-retry
 * sys.reset path would wipe K144's baud setting.  Suppress while the
 * pump owns the channel. */
static volatile bool s_auto_retry_suppressed = false;

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

static void wakeword_event_handler(voice_wakeword_event_t event, const char *text, void *user); /* TT #617 fwd-decl */

/* TT #617 / #131 — Gate K144 onboard wakeword on the wake_src setting.
 *   "k144"    — arm sherpa-ncnn on K144's own mic
 *   "ext_pcm" — arm sherpa-ncnn but the audio source is Tab5's mic via
 *               the ext_pcm ingest path; voice_wakeword still subscribes
 *               to the same asr.utf-8.stream to parse transcripts.
 *   "dragon"/"off" — skip; another path owns wake. */
static esp_err_t voice_onboard_arm_k144_wakeword_internal(void) {
   if (!tab5_settings_wake_src_is("k144") && !tab5_settings_wake_src_is("ext_pcm")) {
      ESP_LOGI(TAG, "wake_src != k144/ext_pcm — skipping K144 wakeword arm");
      tab5_debug_obs_event("wake_src", "skip_k144");
      return ESP_OK;
   }
   return voice_wakeword_start(NULL, wakeword_event_handler, NULL);
}

/* ---------------------------------------------------------------------- */
/*  Wakeword event bridge — voice_wakeword task → LVGL UI                  */
/*                                                                        */
/*  ui_home_show_toast / ui_orb_ripple_for_tool must run on the LVGL      */
/*  thread; the wakeword task is a separate FreeRTOS task, so we marshal  */
/*  via tab5_lv_async_call (LEARNINGS: lv_async_call is NOT thread-safe   */
/*  so we use the wrapped tab5 helper).  Strings are heap-allocated by    */
/*  the producer, freed by the LVGL-thread consumer.                      */
/* ---------------------------------------------------------------------- */
#include "ui_orb.h"

static void wakeword_toast_async(void *user) {
   char *txt = (char *)user;
   if (txt == NULL) return;
   ui_home_show_toast(txt);
   ui_orb_ripple_for_tool("wakeword");
   free(txt);
}

/* WAKE → trigger a real voice turn (orb-tap path), not just a toast.
 * The user said "hey tinker" — they expect to ask a question, not
 * narrate a note.  voice_start_listening() does exactly what the orb
 * tap does: opens the mic, ships PCM to Dragon, runs the STT → LLM →
 * TTS round-trip.
 *
 * We also kill the K144 dictation-buffer auto-capture (which would
 * otherwise mirror the same speech the user is sending to Dragon),
 * so we don't get a stray "Saved: …" toast at the end of every voice
 * turn.  The matcher returns to IDLE and continues listening for the
 * next wake. */
static void wakeword_trigger_voice_turn(void *user) {
   (void)user;
   /* TT #597 — Barge-in: if wake fires while Tinker is mid-TTS, cancel
    * the current voice turn first.  voice_cancel() stops in-flight TTS
    * playback + sends the cancel frame to Dragon so the LLM doesn't
    * keep streaming.  Then start a fresh listening session.  Small
    * grace period after cancel so the state machine settles to READY
    * before we try to open the mic. */
   if (voice_get_state() == VOICE_STATE_SPEAKING) {
      ESP_LOGI(TAG, "barge-in: wake during SPEAKING — cancelling current turn");
      tab5_debug_obs_event("wakeword.fire", "barge_in");
      voice_cancel();
      vTaskDelay(pdMS_TO_TICKS(150));
   }

   /* TT #611 — Wake = orb-tap parity, properly.  Earlier attempt
    * (TT #604) called voice_start_listening directly, which matched
    * the audio path but skipped 5 UX wrapper steps (overlay-visibility
    * check, debounce, WS-connected guard with reconnect/toast,
    * dictation pipeline reset, ui_voice_show).  ui_home_start_voice_turn
    * is the single source of truth for "start an Ask voice turn from
    * this device" — same function the orb tap handler calls.  Wake
    * now opens the voice overlay, runs the same guards, identical UX. */
   esp_err_t err = ui_home_start_voice_turn("wakeword");
   if (err != ESP_OK) {
      ESP_LOGW(TAG, "ui_home_start_voice_turn on wake bounced: %s", esp_err_to_name(err));
   }
}

static void wakeword_event_handler(voice_wakeword_event_t event, const char *text, void *user) {
   (void)user;
   switch (event) {
      case VOICE_WAKEWORD_EVENT_WAKE: {
         /* TT #131-opt2: audible wake chime — confirms KWS fired before
          * we start listening for the user's question. */
         ui_audio_cue_play(UI_CUE_INCOMING_HIGH);
         /* Trigger the full voice turn on the LVGL thread (mic + WS
          * dispatch both expect to run on the main task). */
         tab5_lv_async_call(wakeword_trigger_voice_turn, NULL);
         /* Cancel the K144 dictation auto-capture so we don't ALSO
          * mirror the user's question into a "save note". */
         voice_wakeword_force_dictation_stop();
         char *msg = strdup("Tinker listening…");
         if (msg != NULL) tab5_lv_async_call(wakeword_toast_async, msg);
         break;
      }
      case VOICE_WAKEWORD_EVENT_DICTATION_FINAL: {
         /* Show first 40 chars of the transcript as a confirmation toast. */
         const char *src = (text != NULL) ? text : "";
         char *msg = malloc(72);
         if (msg != NULL) {
            size_t take = strlen(src);
            if (take > 40) take = 40;
            snprintf(msg, 72, take > 0 ? "Saved: %.*s%s" : "Note saved (silence)", (int)take, src,
                     take == 40 ? "…" : "");
            tab5_lv_async_call(wakeword_toast_async, msg);
         }
         break;
      }
      case VOICE_WAKEWORD_EVENT_DICTATION_PARTIAL:
      case VOICE_WAKEWORD_EVENT_TRANSCRIPT:
         /* No UI for partials in this first cut — keeps the toast surface
          * uncluttered.  Live transcript display can layer in via the
          * orb caption or a new voice overlay later. */
         break;
   }
}

/* ---------------------------------------------------------------------- */
/*  Failover jobs                                                         */
/* ---------------------------------------------------------------------- */

/* Boot warm-up: probe + one synchronous infer to map the LLM into NPU
 * memory.  Up to 6 minutes — covers the K144's pathologically slow
 * cold-start.  On success the failover gate flips to READY; on any
 * failure (probe timeout, infer hang, NPU stall) it flips to UNAVAILABLE
 * and stays there until the next reboot. */
/* TT #328 Wave 3 P0 #12 — surface K144 warm-up failure so the user knows
 * Onboard mode + Local-failover are dead until reboot.  Marshalled to the
 * LVGL thread because the warmup job runs on tab5_worker.
 *
 * Wave 13 — banner copy adapts to retry-budget state.  While retries
 * remain, tell the user we'll keep trying and where to manually retry.
 * Once exhausted, fall back to the original "needs power-cycle" guidance. */
static void k144_unavailable_banner_async(void *arg) {
   (void)arg;
   /* TT #511 wave-1: banner suppressed — it cluttered the home screen
    * on every K144-less boot.  Diagnostics live in `m5.warmup` /
    * `error.k144` obs events + GET /m5; on-tap toast still fires when
    * the user actively touches Onboard while it's unhealthy. */
}

/* esp_timer callback — fires off the esp_timer dispatcher task.  Must
 * NOT block; we simply enqueue the worker job that does the real
 * sys.reset + re-warmup.  Caller (mark_k144_unavailable) schedules
 * this so it runs M5_AUTO_RETRY_DELAY_US later. */
static void auto_retry_timer_cb(void *arg) {
   (void)arg;
   /* TT #131 — ext_pcm pump suppresses auto-retry while it owns the
    * UART channel at non-default baud.  Skip silently; the pump itself
    * acts as the liveness probe. */
   if (s_auto_retry_suppressed) {
      ESP_LOGD(TAG, "K144 auto-retry suppressed (ext_pcm owns channel)");
      return;
   }
   /* Recheck state — manual reset_failover via UI/debug may have
    * already recovered us; if so, skip the auto-retry. */
   if (s_m5_failover == M5_FAIL_READY || s_m5_failover == M5_FAIL_PROBING) {
      return;
   }
   if (s_auto_retry_count >= M5_AUTO_RETRY_MAX) {
      return;
   }
   s_auto_retry_count++;
   ESP_LOGI(TAG, "K144 auto-retry %d/%d firing...", s_auto_retry_count, M5_AUTO_RETRY_MAX);
   tab5_debug_obs_event("m5.reset", "auto_retry");
   /* Best-effort enqueue.  If the worker queue is full, the auto-retry
    * is dropped silently — next UNAVAILABLE event re-arms the timer. */
   (void)voice_onboard_reset_failover();
}

static void mark_k144_unavailable(const char *reason) {
   /* TT #131 — while ext_pcm owns the UART at high baud, voice_onboard's
    * health checks (sys.hwinfo refresh, etc.) will fail because they're
    * not aware of the negotiated baud.  Suppress the "unavailable"
    * cascade so it doesn't trigger sys.reset which wipes K144's baud. */
   if (s_auto_retry_suppressed) {
      ESP_LOGD(TAG, "K144 mark_unavailable suppressed (ext_pcm armed): %s", reason);
      return;
   }
   s_m5_failover = M5_FAIL_UNAVAILABLE;
   tab5_debug_obs_event("m5.warmup", "unavailable");
   tab5_debug_obs_event("error.k144", reason);

   /* Wave 13 — schedule the next auto-retry if we haven't exhausted
    * the budget.  Lazy-create the esp_timer on first use (saves the
    * ~16 bytes of timer struct for boards that boot with K144 already
    * healthy). */
   bool exhausted = (s_auto_retry_count >= M5_AUTO_RETRY_MAX);
   s_auto_retry_exhausted = exhausted;
   if (!exhausted) {
      if (s_auto_retry_timer == NULL) {
         const esp_timer_create_args_t cfg = {
             .callback = &auto_retry_timer_cb,
             .arg = NULL,
             .dispatch_method = ESP_TIMER_TASK,
             .name = "m5_auto_retry",
             .skip_unhandled_events = true,
         };
         esp_err_t te = esp_timer_create(&cfg, &s_auto_retry_timer);
         if (te != ESP_OK) {
            ESP_LOGW(TAG, "esp_timer_create failed (%s) — auto-retry disabled", esp_err_to_name(te));
            s_auto_retry_timer = NULL;
         }
      }
      if (s_auto_retry_timer != NULL) {
         /* Stop-then-start re-arms the timer cleanly.  Stop on a
          * non-running timer is ESP_ERR_INVALID_STATE — ignore. */
         (void)esp_timer_stop(s_auto_retry_timer);
         esp_err_t se = esp_timer_start_once(s_auto_retry_timer, M5_AUTO_RETRY_DELAY_US);
         if (se == ESP_OK) {
            ESP_LOGI(TAG, "K144 auto-retry scheduled in 60s (%d/%d remaining)", M5_AUTO_RETRY_MAX - s_auto_retry_count,
                     M5_AUTO_RETRY_MAX);
         } else {
            ESP_LOGW(TAG, "esp_timer_start_once failed (%s)", esp_err_to_name(se));
         }
      }
   }
   tab5_lv_async_call(k144_unavailable_banner_async, (void *)(uintptr_t)exhausted);
}

/* TT #328 Wave 16 — centralised "K144 transitioned to READY" hook.
 * Marshals ui_home_clear_error_banner() to the LVGL thread via
 * tab5_lv_async_call so the auto-retry banner posted in
 * mark_k144_unavailable() actually disappears once recovery
 * succeeds.  Pre-Wave-16 the banner stayed pinned indefinitely
 * because no path called clear_error_banner. */
extern void ui_home_clear_error_banner(void);
static void k144_recovered_banner_async(void *arg) {
   (void)arg;
   ui_home_clear_error_banner();
}

static void mark_k144_recovered(void) {
   /* Reset the auto-retry budget so the NEXT failure starts fresh.
    * The 3-attempt cap protects against infinite retry storms during
    * a single outage; once the K144 has come back, that storm is
    * over and a future outage deserves its own 3 attempts. */
   s_auto_retry_count = 0;
   s_auto_retry_exhausted = false;
   /* Cancel any pending auto-retry timer — recovery already happened
    * (likely manual via tap-to-recover or POST /m5/reset; the timer
    * was set by an earlier mark_k144_unavailable and should not
    * fire post-recovery). */
   if (s_auto_retry_timer != NULL) {
      (void)esp_timer_stop(s_auto_retry_timer);
   }
   tab5_lv_async_call(k144_recovered_banner_async, NULL);
}

static void onboard_warmup_job(void *arg) {
   (void)arg;
   s_m5_failover = M5_FAIL_PROBING;
   ESP_LOGI(TAG, "K144 failover warm-up: probing module...");
   tab5_debug_obs_event("m5.warmup", "start");
   /* Belt-and-braces: idempotent stop in case anything left the
    * wakeword task running across boots — see reset_failover_job
    * for the same rationale. */
   voice_wakeword_stop();

   /* TT #131 stability 2026-05-20: auto-detect K144's current baud.
    * Tab5 always boots at 115200 (TAB5_PORT_C_UART_BAUD), but K144
    * retains its baud across Tab5 reboots — if a previous Tab5
    * session ran the bump-to-1.5M sequence, K144 is still at 1.5M
    * after Tab5 cold-boots back to 115200.  Without this detect step
    * the probe fails at 115200, Tab5 marks K144 unavailable, and the
    * user has to manually intervene.  With it, Tab5 just switches
    * its own local baud to match whatever K144 is at. */
   esp_err_t pe = voice_m5_llm_probe();
   if (pe != ESP_OK) {
      ESP_LOGI(TAG, "K144 probe failed at 115200 — trying 1.5 Mbps (may be from prev session)");
      tab5_port_c_uart_set_baud(1500000);
      vTaskDelay(pdMS_TO_TICKS(100));
      pe = voice_m5_llm_probe();
      if (pe == ESP_OK) {
         ESP_LOGI(TAG, "K144 found at 1.5 Mbps — Tab5 baud auto-matched");
      } else {
         ESP_LOGW(TAG, "K144 unreachable at 115200 AND 1.5 Mbps (%s) — failover disabled",
                  esp_err_to_name(pe));
         tab5_port_c_uart_set_baud(115200);  /* revert local */
         mark_k144_unavailable("probe_fail");
         return;
      }
   }
   char scratch[64];
   int64_t t0 = esp_timer_get_time();
   esp_err_t ie = voice_m5_llm_infer("hi", scratch, sizeof(scratch), M5_FAILOVER_WARMUP_TIMEOUT_S);
   int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
   if (ie == ESP_OK) {
      ESP_LOGI(TAG, "K144 warm in %lldms — failover available: '%s'", dt_ms, scratch);
      s_m5_failover = M5_FAIL_READY;
      tab5_debug_obs_event("m5.warmup", "ready");
      mark_k144_recovered(); /* Wave 16 — clear banner + reset retry budget */
      /* Release the warmup LLM unit BEFORE arming wakeword.  Empirically
       * (2026-05-18 live retest on Tab5) leaving llm.NNNN allocated
       * causes the subsequent asr.setup to return err=-21 "task full"
       * — K144's StackFlow daemon caps concurrent units on the NPU.
       * The wakeword path doesn't need the LLM at all (audio + asr
       * only), and vmode=4's chain_start re-allocates llm on its own
       * (~3 s warm cache).  Net: ~3 s slower first-turn in vmode=4,
       * but wakeword actually starts.
       *
       * Wake-word revival: now that the K144 UART is confirmed up,
       * start the always-on ASR chain.  Defaults: wake="hey tinker",
       * end-phrase="save note", 32 KB dictation buffer, 4-hour cap.
       * Failures non-fatal — the LLM path still works without wakeword. */
      voice_m5_llm_release();

      bool boot_to_ext_pcm = tab5_settings_wake_src_is("ext_pcm");

      /* Arm wakeword FIRST at whatever baud we're currently at (115200
       * default or 1.5M from auto-detect).  asr.setup is a multi-step
       * handshake that needs clean UART — empirically the bump-then-
       * setup order failed because asr.setup at 1.5M hit framing
       * errors and never completed.  Setup at 115200 is clean; pump
       * can then run at 1.5M which tolerates framing errors via
       * K144's JSON parser's partial-frame recovery. */
      esp_err_t we = voice_onboard_arm_k144_wakeword_internal();
      if (we == ESP_ERR_INVALID_RESPONSE) {
         /* TT #580: post-Tab5-reflash, K144's previous-session audio +
          * asr units are still alive on the daemon.  Daemon refuses
          * new setup with "task full" / "unit call false".  Trigger
          * a sys.reset to clear the unit table — the reset's success
          * branch re-arms wakeword on a clean daemon.  Capped by the
          * existing 3-attempts-per-boot retry budget. */
         ESP_LOGW(TAG,
                  "wakeword start failed (%s) — queueing sys.reset to "
                  "clear stale K144 daemon units",
                  esp_err_to_name(we));
         (void)voice_onboard_reset_failover();
      } else if (we != ESP_OK && we != ESP_ERR_INVALID_STATE) {
         ESP_LOGW(TAG, "wakeword start skipped: %s", esp_err_to_name(we));
      } else if (we == ESP_OK) {
         /* Wakeword armed cleanly at the current baud.  Now if user
          * opted in to ext_pcm, bump to 1.5 Mbps for real-time pump
          * throughput (10 fps audio vs 3 fps at 115200).  asr.setup
          * already done at 115200 so the bump doesn't affect setup
          * completion; only the pump path runs at 1.5M after this. */
         if (boot_to_ext_pcm) {
            if (tab5_port_c_uart_get_baud() != 1500000) {
               ESP_LOGI(TAG, "wakeword armed — now bumping baud 115200 → 1.5 Mbps for real-time pump");
               voice_onboard_suppress_auto_retry(true);
               esp_err_t be = voice_m5_llm_set_baud(1500000);
               if (be != ESP_OK) {
                  ESP_LOGW(TAG, "post-setup baud bump failed (%s) — pump will run at 115200 (~3 fps)", esp_err_to_name(be));
               } else {
                  ESP_LOGI(TAG, "baud bumped to 1.5 Mbps for pump");
               }
            }
            voice_ext_pcm_stream_arm();
            ESP_LOGI(TAG, "ext_pcm pump armed (Tab5 mic → K144 ASR)");
         }
      }
   } else {
      ESP_LOGW(TAG,
               "K144 warm-up %s after %lldms — failover disabled (NPU likely hung; "
               "see LEARNINGS \"K144 cold-start model load can hang\")",
               esp_err_to_name(ie), dt_ms);
      mark_k144_unavailable("warmup_fail");
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
         /* TT #328 Wave 7 — attach a synthetic receipt with
          * model_short="K144" + mils=0 (free) so the bubble's
          * timestamp row stamps "· K144 · FREE" the same way Cloud
          * replies stamp "· HAIKU · $0.003".  Pre-Wave-7 the
          * failover reply was visually indistinguishable from a
          * Dragon-Local reply; users couldn't tell they got the
          * smaller K144 model.  Reuses chat_msg_view's existing
          * receipt-render path (no bubble-style refactor needed). */
         extern int chat_store_attach_receipt_ex(uint32_t, uint16_t, uint16_t, const char *, bool);
         chat_store_attach_receipt_ex(0, 0, 0, "K144", false);
         extern void ui_chat_refresh_receipts(void);
         ui_chat_refresh_receipts();
         tab5_ui_unlock();
      }
      ESP_LOGI(TAG, "K144 reply: '%s'", reply);
      /* W3-C-c (cross-stack cohesion audit 2026-05-11): POST both
       * sides of the failover turn to Dragon's canonical messages
       * DB.  prompt arrived as text (typed in chat); the K144 reply
       * is text too. */
      voice_messages_sync_post("user", prompt, "text");
      voice_messages_sync_post("assistant", reply, "text");
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

/* ──────────────────────────────────────────────────────────────────── */
/*  TT #131 stability watchdog 2026-05-20                                */
/*                                                                       */
/*  K144's `llm-asr` daemon cycles intermittently (documented quirk).    */
/*  When it does, Tab5's cached asr_id becomes stale: the pump keeps     */
/*  sending frames over UART to a work_id that no longer exists on K144, */
/*  K144 silently drops them, no transcripts come back, wakeword never   */
/*  fires.  Manual recovery = `systemctl restart llm-sys` + /m5/reset    */
/*  + /tinkeron/wake_src=ext_pcm.                                        */
/*                                                                       */
/*  This watchdog automates that: every WATCHDOG_INTERVAL_MS, check if   */
/*  the pump is sending frames cleanly (last_pump_age_ms < 1 s) AND      */
/*  voice_wakeword has received NO ASR delta in WATCHDOG_ASR_STALL_MS    */
/*  (20 s).  Pump healthy + ASR silent = K144 cycled.  Trigger           */
/*  voice_onboard_reset_failover() which re-runs the full chain bringup. */
/*  Cooldown gate prevents thrash: don't re-kick within 90 s of last     */
/*  kick (recovery itself takes ~30 s).                                  */
/* ──────────────────────────────────────────────────────────────────── */
#define WATCHDOG_INTERVAL_MS 10000
#define WATCHDOG_ASR_STALL_MS 20000
#define WATCHDOG_COOLDOWN_MS 60000           /* 60 s — was 90 s; faster recovery */
#define WATCHDOG_PUMP_HEALTHY_MS 1000
#define WATCHDOG_GRACE_AFTER_BOOT_MS 30000  /* don't fire in first 30 s — chain may still be coming up */
#define WATCHDOG_RESET_FAIL_CAP 2            /* after 2 consecutive sys.reset failures → escalate to sys.reboot */
#define WATCHDOG_REBOOT_COOLDOWN_MS 180000   /* 3 min after sys.reboot before considering another */

static volatile int64_t s_watchdog_last_kick_us = 0;
static volatile int64_t s_watchdog_started_us = 0;
static volatile int s_watchdog_reset_fail_count = 0;
static volatile int64_t s_watchdog_last_reboot_us = 0;

static void onboard_watchdog_task(void *arg) {
   (void)arg;
   s_watchdog_started_us = esp_timer_get_time();
   ESP_LOGI(TAG, "K144-ASR watchdog started (poll=%ds, stall=%ds, cooldown=%ds)",
            WATCHDOG_INTERVAL_MS / 1000, WATCHDOG_ASR_STALL_MS / 1000,
            WATCHDOG_COOLDOWN_MS / 1000);

   while (1) {
      vTaskDelay(pdMS_TO_TICKS(WATCHDOG_INTERVAL_MS));

      /* Only watchdog the ext_pcm path.  Other modes don't have this
       * Tab5-pumps-to-cached-work_id failure mode. */
      if (!tab5_settings_wake_src_is("ext_pcm")) continue;

      /* Chain must be in the steady READY state.  Don't kick during
       * PROBING (a reset is already in flight) or UNAVAILABLE
       * (auto-retry will handle it). */
      if (s_m5_failover != M5_FAIL_READY) continue;

      /* Wakeword must be armed — if not, no asr_id binding to be stale. */
      if (!voice_wakeword_is_active()) continue;

      int64_t now = esp_timer_get_time();

      /* Boot grace: chain may still be coming up cleanly. */
      if (now - s_watchdog_started_us < (int64_t)WATCHDOG_GRACE_AFTER_BOOT_MS * 1000) continue;

      /* Cooldown after a previous kick. */
      if (now - s_watchdog_last_kick_us < (int64_t)WATCHDOG_COOLDOWN_MS * 1000) continue;

      voice_ext_pcm_stream_stats_t stats;
      voice_ext_pcm_stream_get_stats(&stats);

      /* Pump must be actively flowing.  If pump is paused (mid voice
       * turn) OR stopped, this check shouldn't fire — there's a legit
       * reason no fresh frames are reaching K144. */
      if (!stats.task_running || !stats.armed) continue;
      if (stats.last_pump_age_ms < 0 || stats.last_pump_age_ms > WATCHDOG_PUMP_HEALTHY_MS) continue;

      /* Pump has been pumping but we still need enough samples to draw
       * a conclusion — fresh setup may not have produced any deltas
       * yet, that's fine. */
      if (stats.frames_pumped < 50) continue;

      int64_t last_delta = voice_wakeword_last_delta_us();
      int64_t delta_age_ms;
      if (last_delta == 0) {
         /* Pump has sent 50+ frames but K144 has emitted no transcript
          * at all — strong signal that asr_id binding is dead. */
         delta_age_ms = (now - s_watchdog_started_us) / 1000;
      } else {
         delta_age_ms = (now - last_delta) / 1000;
      }
      if (delta_age_ms < WATCHDOG_ASR_STALL_MS) continue;

      /* All gates passed: pump flowing, wakeword armed, K144 ready,
       * but no transcript in 20+ seconds.  K144 ASR cycled. */
      ESP_LOGW(TAG,
               "watchdog: K144 ASR stale (last delta %lldms ago, pump_age %lldms, frames=%lu, fails=%d) — kicking recovery",
               delta_age_ms, stats.last_pump_age_ms, (unsigned long)stats.frames_pumped, s_watchdog_reset_fail_count);
      char detail[48];
      snprintf(detail, sizeof(detail), "asr_stale age=%llds frames=%lu fails=%d",
               delta_age_ms / 1000, (unsigned long)stats.frames_pumped, s_watchdog_reset_fail_count);
      tab5_debug_obs_event("watchdog", detail);
      s_watchdog_last_kick_us = now;

      /* TT #131 2026-05-20 escalation: after WATCHDOG_RESET_FAIL_CAP
       * consecutive sys.reset failures, escalate to sys.reboot
       * (full K144 Linux reboot — ~30 s downtime but guaranteed
       * unwedge of llm-sys / llm-asr daemons).  Rate-limited to one
       * sys.reboot per WATCHDOG_REBOOT_COOLDOWN_MS to avoid boot loops. */
      bool do_reboot = s_watchdog_reset_fail_count >= WATCHDOG_RESET_FAIL_CAP &&
                       (now - s_watchdog_last_reboot_us) > (int64_t)WATCHDOG_REBOOT_COOLDOWN_MS * 1000;

      voice_wakeword_stop();
      vTaskDelay(pdMS_TO_TICKS(300));

      if (do_reboot) {
         ESP_LOGW(TAG, "watchdog: %d consecutive reset fails → escalating to sys.reboot", s_watchdog_reset_fail_count);
         tab5_debug_obs_event("watchdog", "escalate_reboot");
         s_watchdog_last_reboot_us = now;
         s_watchdog_reset_fail_count = 0;
         esp_err_t re = voice_m5_llm_sys_reboot();
         if (re != ESP_OK) {
            ESP_LOGW(TAG, "watchdog: sys.reboot send failed (%s) — K144 may be unreachable", esp_err_to_name(re));
         }
         /* Wait ~60s for K144 hardware reboot + daemon start, then trigger
          * reset_failover to re-establish chain on the freshly-rebooted K144. */
         vTaskDelay(pdMS_TO_TICKS(60000));
         tab5_port_c_uart_set_baud(115200);  /* K144 boots at default */
         (void)voice_onboard_reset_failover();
      } else {
         esp_err_t e = voice_onboard_reset_failover();
         if (e != ESP_OK) {
            ESP_LOGW(TAG, "watchdog: reset_failover bounce (%s)", esp_err_to_name(e));
         }
      }

      /* Track recovery outcome: poll over the next 30 s for any new
       * ASR delta.  If we get one → reset successful, clear fail count.
       * If still stale → increment fail count for potential escalation. */
      vTaskDelay(pdMS_TO_TICKS(30000));
      int64_t check = voice_wakeword_last_delta_us();
      if (check > now) {
         /* New delta arrived since the kick — recovery worked. */
         if (s_watchdog_reset_fail_count > 0) {
            ESP_LOGI(TAG, "watchdog: recovery succeeded — clearing fail count (was %d)",
                     s_watchdog_reset_fail_count);
         }
         s_watchdog_reset_fail_count = 0;
      } else {
         s_watchdog_reset_fail_count++;
         ESP_LOGW(TAG, "watchdog: recovery did NOT restore ASR — fail count now %d",
                  s_watchdog_reset_fail_count);
      }
   }
}

esp_err_t voice_onboard_start_warmup(void) {
   if (s_m5_failover != M5_FAIL_UNKNOWN) return ESP_ERR_INVALID_STATE;
   /* Spawn the K144-ASR-cycling watchdog at the same time as the boot
    * warmup.  PSRAM-backed stack to keep internal SRAM headroom. */
   static volatile bool s_watchdog_spawned = false;
   if (!s_watchdog_spawned) {
      s_watchdog_spawned = true;
      BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(onboard_watchdog_task, "onboard_wd",
                                                      4096, NULL, 1, NULL,
                                                      tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
      if (ok != pdPASS) {
         ESP_LOGW(TAG, "watchdog task spawn failed — running without ASR-stall recovery");
         s_watchdog_spawned = false;
      }
   }
   return tab5_worker_enqueue(onboard_warmup_job, NULL, "m5_warmup");
}

/* Wave 13 — recovery job.  Sends sys.reset, waits for daemon to come
 * back, re-runs the warmup probe + hi-infer.  Same async shape as
 * onboard_warmup_job — runs on tab5_worker so the LVGL caller doesn't
 * block on the ~5-10 s round-trip. */
static void onboard_reset_failover_job(void *arg) {
   (void)arg;
   ESP_LOGI(TAG, "K144 reset_failover: requesting daemon restart...");
   tab5_debug_obs_event("m5.reset", "start");
   /* Flip to PROBING immediately so concurrent send_text calls bounce
    * back ESP_ERR_INVALID_STATE instead of racing the reset. */
   s_m5_failover = M5_FAIL_PROBING;

   /* Stop the wakeword listener BEFORE sys.reset.  sys.reset kills
    * K144's audio + asr units mid-stream, leaving Tab5's wakeword
    * task draining a dead UART.  Stop now, re-arm after warmup ready.
    * Idempotent — stop is a no-op when not running.  Verified 2026-
    * 05-18: without this the post-reset re-arm called start() while
    * the prior task was still alive → INVALID_STATE → silent no-op
    * → ASR never came back even though K144 was healthy. */
   voice_wakeword_stop();

   esp_err_t re = voice_m5_llm_sys_reset();
   if (re != ESP_OK) {
      ESP_LOGW(TAG,
               "sys.reset failed (%s) — proceeding to re-probe anyway "
               "(K144 may already be down)",
               esp_err_to_name(re));
      tab5_debug_obs_event("m5.reset", "ack_fail");
   } else {
      tab5_debug_obs_event("m5.reset", "ack_ok");
   }

   /* TT #131 2026-05-20: poll for K144 readiness instead of fixed wait.
    * Daemon needs ~4 s MQTT reconnect + ~8-15 s for llm-llm to register
    * its RPC server.  Earlier 15 s fixed wait hit cases where K144
    * needed >20 s — probe timed out (3s budget) and the whole
    * recovery cycle marked unavailable.  Now: start polling at 8 s
    * (minimum K144 boot), then ping every 2 s up to 90 s.  Exit
    * early as soon as ping succeeds.  Most recoveries land at 10-20 s;
    * pathological at 60-90 s.  Beyond 90 s we give up and escalate. */
   tab5_debug_obs_event("m5.warmup", "start");
   vTaskDelay(pdMS_TO_TICKS(8000));  /* min boot time */
   esp_err_t pe = ESP_ERR_TIMEOUT;
   for (int i = 0; i < 41; i++) {  /* up to 82 s additional, 90 s total */
      pe = voice_m5_llm_probe();
      if (pe == ESP_OK) {
         ESP_LOGI(TAG, "K144 ping success after %d s post-reset", 8 + i * 2);
         break;
      }
      vTaskDelay(pdMS_TO_TICKS(2000));
      if (s_chain_stop_flag) break;
   }
   if (pe != ESP_OK) {
      ESP_LOGW(TAG, "K144 didn't come back within 90 s after sys.reset (%s)", esp_err_to_name(pe));
      mark_k144_unavailable("reset_probe_fail");
      tab5_debug_obs_event("m5.reset", "fail");
      return;
   }
   char scratch[64];
   int64_t t0 = esp_timer_get_time();
   esp_err_t ie = voice_m5_llm_infer("hi", scratch, sizeof(scratch), M5_FAILOVER_WARMUP_TIMEOUT_S);
   int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
   if (ie == ESP_OK) {
      ESP_LOGI(TAG, "K144 recovered in %lldms — failover available again", dt_ms);
      s_m5_failover = M5_FAIL_READY;
      tab5_debug_obs_event("m5.warmup", "ready");
      tab5_debug_obs_event("m5.reset", "recovered");
      mark_k144_recovered(); /* Wave 16 — clear banner + reset retry budget */
      /* Same "free LLM slot before ASR" gate as the initial warmup
       * path — see comment there for why this is required. */
      voice_m5_llm_release();
      /* NOTE: baud bump deliberately NOT done here.  Earlier attempt
       * caused Tab5-vs-K144 baud desync on /m5/reset path because the
       * boot-time bump path can leave Tab5 at 1.5 Mbps while K144's
       * uartsetup sometimes fails silently → both ends out of sync.
       *
       * Order matters: arm wakeword FIRST at 115200 (clean setup),
       * THEN bump for the pump.  See onboard_warmup_job for rationale. */
      bool reset_to_ext_pcm = tab5_settings_wake_src_is("ext_pcm");
      if (reset_to_ext_pcm && tab5_port_c_uart_get_baud() != 115200) {
         /* After sys.reset, K144 is at 115200 default.  Sync Tab5 down. */
         ESP_LOGI(TAG, "reset-path: syncing Tab5 to K144 default 115200 baseline");
         tab5_port_c_uart_set_baud(115200);
         vTaskDelay(pdMS_TO_TICKS(50));
      }
      /* Wakeword revival: same hook as the initial warmup path — once
       * K144 is reachable again, (re-)arm the always-on ASR chain.
       * Idempotent (start refuses if already running). */
      esp_err_t we = voice_onboard_arm_k144_wakeword_internal();
      if (we == ESP_ERR_INVALID_RESPONSE) {
         /* TT #580: still wedged after THIS reset.  Queue another (the
          * retry budget caps at 3/boot). */
         ESP_LOGW(TAG,
                  "wakeword (re)start still failed (%s) — queueing another "
                  "sys.reset cycle",
                  esp_err_to_name(we));
         (void)voice_onboard_reset_failover();
      } else if (we != ESP_OK && we != ESP_ERR_INVALID_STATE) {
         ESP_LOGW(TAG, "wakeword (re)start skipped: %s", esp_err_to_name(we));
      } else if (we == ESP_OK) {
         /* Wakeword armed at 115200 (clean setup), now bump for pump. */
         if (reset_to_ext_pcm) {
            if (tab5_port_c_uart_get_baud() != 1500000) {
               ESP_LOGI(TAG, "reset-path: wakeword armed — bumping baud for pump");
               voice_onboard_suppress_auto_retry(true);
               esp_err_t be = voice_m5_llm_set_baud(1500000);
               if (be != ESP_OK) {
                  ESP_LOGW(TAG, "reset-path baud bump failed (%s)", esp_err_to_name(be));
               }
            }
            voice_ext_pcm_stream_arm();
            ESP_LOGI(TAG, "ext_pcm pump armed via reset path");
         }
      }
   } else {
      ESP_LOGW(TAG, "K144 re-warmup %s after %lldms — still unavailable", esp_err_to_name(ie), dt_ms);
      mark_k144_unavailable("reset_warmup_fail");
      tab5_debug_obs_event("m5.reset", "fail");
   }
}

esp_err_t voice_onboard_reset_failover(void) {
   /* Refuse if a probe (initial warmup OR a prior reset) is already in
    * flight — callers (timer, UI tap, debug endpoint) all converge on
    * the same job which is fine to run once at a time. */
   if (s_m5_failover == M5_FAIL_PROBING) {
      ESP_LOGI(TAG, "reset_failover: already probing — caller can poll state");
      return ESP_ERR_INVALID_STATE;
   }
   return tab5_worker_enqueue(onboard_reset_failover_job, NULL, "m5_reset");
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

      /* W3-C-c (cross-stack cohesion audit 2026-05-11): POST each
       * autonomous-chain commit to Dragon's canonical messages DB.
       * ASR = the user's spoken turn; LLM = K144's reply.  Both
       * land via the chain's stacked K144 mic + LLM + TTS so they
       * carry input_mode="voice". */
      voice_messages_sync_post(from_llm ? "assistant" : "user", buf, "voice");

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

   /* TT #582 — re-arm wakeword now that the chain has released K144's
    * audio + asr units.  Idempotent (start refuses if already running),
    * and only meaningful if user has flipped back to a Dragon-using
    * vmode where wakeword is wanted — but cheap to call regardless. */
   esp_err_t we = voice_onboard_arm_k144_wakeword_internal();
   if (we != ESP_OK && we != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "wakeword re-arm after chain stop skipped: %s",
               esp_err_to_name(we));
   }

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

   /* TT #582 — wakeword + chain BOTH want K144's audio + asr units.
    * If wakeword is running when vmode flips to ONBOARD, the chain's
    * subsequent audio.setup hits err=-21 'task full'.  Tear down the
    * wakeword listener first to free the units.  Idempotent — no-op
    * if not running.  Wakeword re-arms automatically via the
    * onboard_warmup_job path when user flips back to a Dragon-using
    * vmode. */
   voice_wakeword_stop();

   /* TT #328 Wave 7 — defense-in-depth mic-mute guard.  voice.c's
    * voice_start_listening already guards before this is called from
    * the orb-tap path, but ANY future entry point (REST, debug, new
    * user-input path) would bypass the check otherwise.  Mute is a
    * privacy contract: when the user has set mic_mute=1, NO mic
    * (Tab5's OR K144's onboard mic) should listen — the chain
    * silently captures audio on the K144 even though Tab5's mic is
    * blocked, which violates the contract.  Refuse here too. */
   if (tab5_settings_get_mic_mute()) {
      ESP_LOGW(TAG, "voice_onboard_chain_start: mic muted - refusing");
      return ESP_ERR_INVALID_STATE;
   }

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

/* TT #586 — Settings UI entry-point for "Always-on listener" ON.
 * Idempotent; uses the same wakeword_event_handler as the boot
 * warmup path so UI bridge (toast + orb ripple) stays consistent.
 * No-op if K144 is currently UNAVAILABLE — caller should toast a
 * hint if it cares.  Honours the "release LLM slot before ASR"
 * gate from the warmup path. */
void voice_onboard_suppress_auto_retry(bool suppress) {
   s_auto_retry_suppressed = suppress;
   if (suppress) {
      ESP_LOGI(TAG, "K144 auto-retry suppressed (ext_pcm armed)");
      tab5_debug_obs_event("m5.reset", "suppressed");
      /* Force state to READY while suppressed so other consumers that
       * gate on failover_state==2 don't refuse to proceed. */
      s_m5_failover = M5_FAIL_READY;
   } else {
      ESP_LOGI(TAG, "K144 auto-retry re-enabled");
      tab5_debug_obs_event("m5.reset", "unsuppressed");
   }
}

esp_err_t voice_onboard_arm_wakeword(void) {
   if (s_m5_failover == M5_FAIL_UNAVAILABLE) return ESP_ERR_INVALID_STATE;
   voice_m5_llm_release();
   return voice_onboard_arm_k144_wakeword_internal();
}

/* TT #131 — async wakeword arm.  Posted on the shared worker so HTTP /
 * LVGL callers return immediately.
 *
 * The wakeword listener only needs audio + asr on K144 (NO llm), so this
 * path IGNORES the failover_state gate (which gates on llm.setup success)
 * and just tries voice_onboard_arm_wakeword over and over until asr.setup
 * lands.  Retries with 2 s backoff, 15 attempts (30 s total).
 *
 * If we deferred to reset_failover here we'd lose minutes to the llm
 * model-load probe + auto-retry budget — for ext_pcm the LLM doesn't
 * even need to be reachable. */
static void arm_wakeword_async_job(void *arg) {
   int attempts = (int)(intptr_t)arg;
   if (voice_wakeword_is_active()) return; /* already armed */

   voice_m5_llm_release(); /* free NPU slot before ASR claims it */
   esp_err_t we = voice_onboard_arm_k144_wakeword_internal();
   if (we == ESP_OK || we == ESP_ERR_INVALID_STATE /* already running */) {
      tab5_debug_obs_event("arm_wake_async", "ok");
      return;
   }

   if (attempts >= 15) {
      ESP_LOGW(TAG, "arm_wakeword_async: gave up after %d attempts (last err=%s)", attempts,
               esp_err_to_name(we));
      tab5_debug_obs_event("arm_wake_async", "give_up");
      return;
   }
   ESP_LOGW(TAG, "arm_wakeword_async: attempt %d failed (%s) — retrying in 2s", attempts,
            esp_err_to_name(we));
   vTaskDelay(pdMS_TO_TICKS(2000));
   (void)tab5_worker_enqueue(arm_wakeword_async_job, (void *)(intptr_t)(attempts + 1), "arm_wake_retry");
}

esp_err_t voice_onboard_arm_wakeword_async(void) {
   return tab5_worker_enqueue(arm_wakeword_async_job, (void *)(intptr_t)0, "arm_wake_async");
}
