/**
 * Voice WebSocket protocol layer (Tab5 ↔ Dragon) — implementation.
 *
 * Wave 23 SOLID-audit closure for TT #331 (extract A1).  See
 * voice_ws_proto.h for the layer contract.  Pre-extract this all
 * lived inline in voice.c at L525-1948.
 */
#include "voice_ws_proto.h"

#include <limits.h> /* INT_MAX (W14-L02 send-bound check) */
#include <string.h>

#include "audio.h" /* tab5_audio_speaker_enable on tts_start */
#include "cJSON.h"
#include "chat_msg_store.h" /* tool indicator bubbles */
#include "config.h"
#include "debug_obs.h" /* tab5_debug_obs_event */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" /* s_state_mutex / s_play_sem externs below */
#include "freertos/task.h"
#include "lvgl.h"     /* lv_tick_get for the widget_action rate limiter */
#include "md_strip.h" /* md_strip_tool_markers on llm streaming */
#include "settings.h"
#include "task_worker.h" /* tab5_worker_enqueue for stop-WS workers */
#include "tool_log.h"    /* tool_log_push_call / _push_result */
#include "ui_chat.h"
#include "ui_core.h" /* tab5_lv_async_call */
#include "ui_home.h"
#include "ui_notes.h"
#include "ui_notification.h"
#include "voice.h"               /* g_voice_ws extern + voice_set_state */
#include "voice_billing.h"       /* receipt + budget */
#include "voice_codec.h"         /* VOICE_CODEC_OPUS_UPLINK_ENABLED */
#include "voice_dictation.h"     /* PR 1: pipeline state machine transitions */
#include "voice_messages_sync.h" /* W3-C-d: drain offline queue on reconnect */
#include "voice_onboard.h"       /* K144 failover hooks */
#include "voice_video.h"         /* VID0 magic peek + downlink decode */
#include "voice_widget_ws.h"     /* widget_* WS dispatch */
#include "widget.h"              /* widget_live_update / widget_live_dismiss */
#include "wifi.h"                /* tab5_wifi_hard_kick / tab5_wifi_connected */

static const char *TAG = "tab5_voice_ws";

/* US-C04: counter for audio frames dropped under WS back-pressure.
 * Internal to voice_ws_send_binary; pre-extract this was a file-static
 * in voice.c.  No external readers — kept here so the dropped-frame
 * accounting stays colocated with the sender that increments it. */
static int s_audio_drop_count = 0;
/* PR 2 polish: grace-period timer for TRANSCRIBING WS drops.  Armed when
 * the WS disconnects mid-transcription, fires after ~45 s, only flips
 * the pipeline to FAILED if it's still stuck at TRANSCRIBING.  Cancelled
 * by a fresh DICT_SAVED / DICT_FAILED transition from the summary
 * handlers, which leave the pipeline in a terminal state the timer's
 * check skips. */
static esp_timer_handle_t s_tx_grace_timer = NULL;

static void tx_grace_timer_fired(void *arg) {
   (void)arg;
   dict_event_t e = voice_dictation_get();
   if (e.state == DICT_TRANSCRIBING) {
      ESP_LOGW(TAG, "TRANSCRIBING grace window expired — flipping pipeline to FAILED/NETWORK");
      voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, (uint32_t)(esp_timer_get_time() / 1000));
   } else {
      ESP_LOGI(TAG, "TRANSCRIBING grace timer fired but pipeline already resolved (state=%s) — no-op",
               voice_dictation_state_name(e.state));
   }
}

void voice_ws_arm_transcribe_grace_timer(uint32_t timeout_ms) {
   if (s_tx_grace_timer == NULL) {
      const esp_timer_create_args_t args = {
          .callback = tx_grace_timer_fired,
          .arg = NULL,
          .dispatch_method = ESP_TIMER_TASK,
          .name = "tx_grace",
      };
      if (esp_timer_create(&args, &s_tx_grace_timer) != ESP_OK) {
         ESP_LOGE(TAG, "Failed to create TRANSCRIBING grace timer; falling back to immediate FAIL");
         voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, (uint32_t)(esp_timer_get_time() / 1000));
         return;
      }
   }
   /* Stop any prior arming so we always have a full fresh window from
    * the latest disconnect. */
   esp_timer_stop(s_tx_grace_timer);
   esp_timer_start_once(s_tx_grace_timer, (uint64_t)timeout_ms * 1000);
   ESP_LOGI(TAG, "TRANSCRIBING grace timer armed: %u ms before pipeline flips to FAILED", (unsigned)timeout_ms);
}

/* SemaphoreHandle_t externs — defined in voice.c, used by the JSON RX
 * dispatcher + UI-async helpers below.  Declared here (not in voice.h)
 * to keep <freertos/semphr.h> out of voice.h's public surface. */
extern SemaphoreHandle_t s_state_mutex;
extern SemaphoreHandle_t s_play_sem;

/* TT #331 Wave 23 SRP-A1: voice.c's s_initialized was promoted to non-
 * static for cross-TU access, but it stays out of voice.h to dodge the
 * collision with ui_voice.c's local static of the same name.  Declared
 * here so the WS event handler can guard async-call dispatches. */
extern volatile bool s_initialized;

/* WS event handler tunables — moved from voice.c.  Only this TU
 * consumes them. */
/* v4·D connectivity audit T1.1: exponential backoff + full jitter on
 * WS reconnect.  Formula:
 *   exp = min(BACKOFF_CAP_MS, BACKOFF_MIN_MS << min(attempt, 5))
 *   delay = esp_random() % (exp/2) + exp/2
 * attempt resets to 0 on WEBSOCKET_EVENT_CONNECTED. */
#define WS_CLIENT_BACKOFF_MIN_MS 1000
#define WS_CLIENT_BACKOFF_CAP_MS 30000

/* Ngrok fallback after this many consecutive failed handshakes against
 * the local LAN URI (only in conn_mode=0 "auto"). */
#define NGROK_FALLBACK_THRESHOLD 4

/* γ3-Tab5 (issue #198): stop retrying after this many consecutive
 * 401 handshake failures. */
#define WS_AUTH_FAIL_THRESHOLD 3

// ---------------------------------------------------------------------------
// WebSocket send helpers (wrapping esp_websocket_client_send_*)
// ---------------------------------------------------------------------------
esp_err_t voice_ws_send_text(const char *msg) {
   if (!g_voice_ws) return ESP_ERR_INVALID_STATE;
   if (!esp_websocket_client_is_connected(g_voice_ws)) return ESP_ERR_INVALID_STATE;
   if (!msg) return ESP_ERR_INVALID_ARG;

   size_t len = strlen(msg);
   /* Wave 14 W14-L02: bound the size_t→int cast.  Realistic messages
    * are <256 bytes but an unchecked cast would silently truncate on
    * an accidental >2 GB string (e.g. a corrupted LLM response
    * flowing through text_update). */
   if (len > INT_MAX) return ESP_ERR_INVALID_ARG;
   int w = esp_websocket_client_send_text(g_voice_ws, msg, (int)len, pdMS_TO_TICKS(1000));
   if (w < 0) {
      ESP_LOGW(TAG, "WS text send failed (%zu bytes)", len);
      return ESP_FAIL;
   }
   return ESP_OK;
}

esp_err_t voice_ws_send_binary(const void *data, size_t len) {
   if (!g_voice_ws) return ESP_ERR_INVALID_STATE;
   if (!esp_websocket_client_is_connected(g_voice_ws)) return ESP_ERR_INVALID_STATE;
   if (!data) return ESP_ERR_INVALID_ARG;
   /* W14-L02: same bound as text. */
   if (len > INT_MAX) return ESP_ERR_INVALID_ARG;

   /* Short 100 ms timeout — we drop frames under pressure rather than
    * block the mic task. If WiFi stalls, I2S DMA would overflow. */
   int w = esp_websocket_client_send_bin(g_voice_ws, (const char *)data, (int)len, pdMS_TO_TICKS(100));
   if (w < 0) {
      s_audio_drop_count++;
      if (s_audio_drop_count % 10 == 1) {
         ESP_LOGW(TAG, "WS binary send dropped — %d frames lost", s_audio_drop_count);
      }
      return ESP_ERR_TIMEOUT;
   }
   if (s_audio_drop_count > 0) {
      ESP_LOGI(TAG, "WS binary send recovered after %d drops", s_audio_drop_count);
      s_audio_drop_count = 0;
   }
   return ESP_OK;
}

/* #266: thin public wrapper for voice_video.c (and any future binary
 * sender).  Behavior is identical to voice_ws_send_binary — same
 * 100 ms send timeout, same drop accounting. */
esp_err_t voice_ws_send_binary_public(const void *data, size_t len) { return voice_ws_send_binary(data, len); }

// ---------------------------------------------------------------------------
// Device registration — sent as FIRST text frame from the CONNECTED handler.
// This is the #76 fix: register is sent from inside the event handler, AFTER
// the client's event task is running and ready to dispatch the server's
// session_start reply. The legacy code sent register synchronously before
// spawning the receive task, which opened a 50–500ms window where Dragon's
// reply arrived while nothing was draining the SDIO RX buffer.
// ---------------------------------------------------------------------------
esp_err_t voice_ws_send_register(void) {
   char device_id[16] = {0};
   char hardware_id[20] = {0};
   char session_id[64] = {0};

   tab5_settings_get_device_id(device_id, sizeof(device_id));
   tab5_settings_get_hardware_id(hardware_id, sizeof(hardware_id));
   tab5_settings_get_session_id(session_id, sizeof(session_id));

   cJSON *root = cJSON_CreateObject();
   if (!root) return ESP_ERR_NO_MEM;

   cJSON_AddStringToObject(root, "type", "register");
   cJSON_AddStringToObject(root, "device_id", device_id);
   cJSON_AddStringToObject(root, "hardware_id", hardware_id);
   cJSON_AddStringToObject(root, "name", "Tab5");
   cJSON_AddStringToObject(root, "firmware_ver", TAB5_FIRMWARE_VER);
   cJSON_AddStringToObject(root, "platform", TAB5_PLATFORM);

   if (session_id[0] != '\0') {
      cJSON_AddStringToObject(root, "session_id", session_id);
   } else {
      cJSON_AddNullToObject(root, "session_id");
   }

   cJSON *caps = cJSON_AddObjectToObject(root, "capabilities");
   cJSON_AddBoolToObject(caps, "mic", true);
   cJSON_AddBoolToObject(caps, "speaker", true);
   cJSON_AddBoolToObject(caps, "screen", true);
   cJSON_AddBoolToObject(caps, "camera", true);
   cJSON_AddBoolToObject(caps, "sd_card", true);
   cJSON_AddBoolToObject(caps, "touch", true);
   /* #266: live JPEG video uplink.  Tab5 sends per-frame binary WS
    * frames prefixed with the "VID0" 4-byte magic + 4-byte length.
    * Dragon detects the magic to route video vs audio. */
   cJSON_AddBoolToObject(caps, "video_send", true);
   cJSON_AddBoolToObject(caps, "video_recv", true); /* #268 Phase 3B */
   cJSON_AddNumberToObject(caps, "video_max_fps", 10);
   cJSON_AddNumberToObject(caps, "video_format", 0); /* 0 = JPEG */
   /* #262: advertise audio codec support.  Dragon picks one and replies
    * via config_update.audio_codec.  Tab5 stays on PCM (current behavior)
    * until Dragon switches it.  Per-direction so the broken uplink
    * encoder (#262 follow-up: silk_NSQ_c crashes on this build) doesn't
    * starve the working downlink decoder; until the encoder ships,
    * advertise opus only on the downlink. */
   cJSON *acodecs = cJSON_AddArrayToObject(caps, "audio_codec");
   cJSON_AddItemToArray(acodecs, cJSON_CreateString("pcm"));
#if VOICE_CODEC_OPUS_UPLINK_ENABLED
   cJSON_AddItemToArray(acodecs, cJSON_CreateString("opus"));
#endif
   cJSON *acodecs_dl = cJSON_AddArrayToObject(caps, "audio_downlink_codec");
   cJSON_AddItemToArray(acodecs_dl, cJSON_CreateString("pcm"));
   cJSON_AddItemToArray(acodecs_dl, cJSON_CreateString("opus"));
   /* v4·D audit P0 fix: widget_capability was spec-only -- now wired.
    * Skills can downgrade widget content for low-end clients (smaller
    * list, lower image res).  Match the actual Tab5 limits we've
    * built out across widget_store.c / ui_home.c. */
   cJSON *widgets = cJSON_AddObjectToObject(caps, "widgets");
   cJSON *types = cJSON_AddArrayToObject(widgets, "types");
   cJSON_AddItemToArray(types, cJSON_CreateString("live"));
   cJSON_AddItemToArray(types, cJSON_CreateString("card"));
   cJSON_AddItemToArray(types, cJSON_CreateString("list"));
   cJSON_AddItemToArray(types, cJSON_CreateString("chart"));
   cJSON_AddItemToArray(types, cJSON_CreateString("media"));
   cJSON_AddItemToArray(types, cJSON_CreateString("prompt"));
   cJSON_AddNumberToObject(widgets, "list_max_items", 5);
   cJSON_AddNumberToObject(widgets, "chart_max_points", 12);
   cJSON_AddNumberToObject(widgets, "prompt_max_choices", 3);
   cJSON_AddNumberToObject(widgets, "screen_w", 720);
   cJSON_AddNumberToObject(widgets, "screen_h", 1280);
   cJSON_AddNumberToObject(widgets, "media_max_w", 660);
   cJSON_AddNumberToObject(widgets, "media_max_h", 440);
   cJSON_AddNumberToObject(widgets, "action_rate_per_sec", 4);

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!json) return ESP_ERR_NO_MEM;

   ESP_LOGI(TAG, "Sending register: device=%s hw=%s session=%s", device_id, hardware_id,
            session_id[0] ? session_id : "(new)");

   esp_err_t err = voice_ws_send_text(json);
   cJSON_free(json);
   return err;
}

// ---------------------------------------------------------------------------
// Outbound widget_action (Tab5 → Dragon).  Pure WS-proto concern: builds a
// JSON frame, rate-limits, sends via voice_ws_send_text.  Pre-extract this
// lived inline in voice.c at the bottom of the public-API section.
// ---------------------------------------------------------------------------
esp_err_t voice_send_widget_action(const char *card_id, const char *event, const char *payload_json) {
   if (!card_id || !event) return ESP_ERR_INVALID_ARG;
   if (!g_voice_ws || !esp_websocket_client_is_connected(g_voice_ws)) return ESP_ERR_INVALID_STATE;

   /* Rate limit: 4/sec max (see docs/WIDGETS.md §11). */
   static uint32_t s_action_bucket = 4;
   static uint32_t s_action_tick = 0;
   uint32_t now = lv_tick_get();
   if (now - s_action_tick >= 1000) {
      s_action_bucket = 4;
      s_action_tick = now;
   }
   if (s_action_bucket == 0) {
      ESP_LOGW(TAG, "widget_action rate-limited (card=%s event=%s)", card_id, event);
      return ESP_ERR_INVALID_STATE;
   }
   s_action_bucket--;

   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "type", "widget_action");
   cJSON_AddStringToObject(msg, "card_id", card_id);
   cJSON_AddStringToObject(msg, "event", event);
   if (payload_json && payload_json[0]) {
      cJSON *p = cJSON_Parse(payload_json);
      if (p) cJSON_AddItemToObject(msg, "payload", p);
   }
   char *json = cJSON_PrintUnformatted(msg);
   cJSON_Delete(msg);
   if (!json) return ESP_ERR_NO_MEM;

   esp_err_t ret = voice_ws_send_text(json);
   cJSON_free(json);
   if (ret == ESP_OK) {
      ESP_LOGI(TAG, "widget_action: %s → %s", card_id, event);
   }
   return ret;
}

// ---------------------------------------------------------------------------
// Task 1.6: UI-async helper machinery (toast / banner / badge dispatchers,
// device-evicted + auth-fail stop workers) + JSON RX dispatcher
// (voice_ws_proto_handle_text, formerly handle_text_message in voice.c).
// voice_debug_inject_text is the public TT #328 Wave 2 e2e injection point.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// US-C02: Generation-guarded async call wrappers.
// ---------------------------------------------------------------------------
typedef struct {
   uint32_t gen;
   char *text;
} voice_async_toast_t;

typedef struct {
   uint32_t gen;
} voice_async_badge_t;

/* γ2-H8 (issue #196): worker to stop the WS client off the WS task.
 * esp_websocket_client_stop() rejects calls from inside the WS task
 * itself (logs "Client cannot be stopped from websocket task" and
 * no-ops).  This worker runs on the shared task_worker queue, so
 * the stop happens on a different task and actually takes effect.
 *
 * Triggered when Tab5 receives a `device_evicted` error frame —
 * another device claimed our slot, and auto-reconnect would just
 * trigger another eviction.  s_disconnecting was already set in
 * the WS handler so the WEBSOCKET_EVENT_DISCONNECTED branch won't
 * try to reconnect when the stop completes. */
static void _voice_stop_ws_worker_fn(void *arg) {
   (void)arg;
   if (g_voice_ws) {
      ESP_LOGW(TAG, "device_evicted worker: stopping WS client now");
      esp_websocket_client_stop(g_voice_ws);
   }
}

/* γ3-Tab5 (issue #198): worker for the auth-failed stop path.
 * Same task-hop pattern as _voice_stop_ws_worker_fn — must run
 * off the WS task or esp_websocket_client_stop() no-ops.
 * Separate function (not shared with the eviction worker) so the
 * ESP_LOG line clearly identifies WHICH path triggered the stop —
 * makes ops triage on a misconfigured-token device much easier
 * than a generic "stop_ws" trace. */
static void _voice_auth_fail_stop_worker_fn(void *arg) {
   (void)arg;
   if (g_voice_ws) {
      ESP_LOGW(TAG, "auth_failed worker: stopping WS after %d consecutive 401s", s_auth_fail_cnt);
      esp_websocket_client_stop(g_voice_ws);
   }
}

static void async_show_toast_cb(void *arg) {
   voice_async_toast_t *t = (voice_async_toast_t *)arg;
   if (!t) return;
   if (t->gen == s_session_gen && t->text) {
      ui_home_show_toast(t->text);
   } else if (t->text) {
      ESP_LOGD(TAG, "Stale async toast dropped (gen %lu vs %lu)", (unsigned long)t->gen, (unsigned long)s_session_gen);
   }
   free(t->text);
   free(t);
}

static void async_refresh_badge_cb(void *arg) {
   voice_async_badge_t *b = (voice_async_badge_t *)arg;
   if (!b) return;
   if (b->gen == s_session_gen) {
      ui_home_refresh_mode_badge();
   } else {
      ESP_LOGD(TAG, "Stale async badge refresh dropped (gen %lu vs %lu)", (unsigned long)b->gen,
               (unsigned long)s_session_gen);
   }
   free(b);
}

void voice_async_toast(char *text) {
   voice_async_toast_t *t = malloc(sizeof(voice_async_toast_t));
   if (!t) {
      /* Wave 14 W14-M08: log the silent-drop so ops can see we
       * squeezed internal SRAM and a user-facing toast never reached
       * LVGL. */
      ESP_LOGW(TAG, "voice_async_toast OOM — dropping toast (text=%.40s)", text ? text : "");
      free(text);
      return;
   }
   t->gen = s_session_gen;
   t->text = text;
   tab5_lv_async_call(async_show_toast_cb, t);
}

/* TT #328 Wave 3 — async fire of ui_home_show_error_banner from voice WS
 * task.  Persistent error banner survives across the 2-3 s toast lifetime
 * so the user can't miss a fatal-state notification (auth lockout, device
 * eviction, etc).  static-string variant: caller must pass a string literal
 * or otherwise outlives the call (no malloc/free shuttle). */
typedef struct {
   const char *text; /* MUST outlive the async dispatch */
} voice_banner_async_t;

static void async_show_error_banner_cb(void *arg) {
   voice_banner_async_t *b = (voice_banner_async_t *)arg;
   if (!b) return;
   if (b->text) {
      ui_home_show_error_banner(b->text, NULL /* non-dismissable */);
   }
   free(b);
}

static void voice_async_error_banner(const char *static_text) {
   if (!static_text) return;
   voice_banner_async_t *b = malloc(sizeof(*b));
   if (!b) {
      ESP_LOGW(TAG, "voice_async_error_banner OOM — banner dropped");
      return;
   }
   b->text = static_text;
   tab5_lv_async_call(async_show_error_banner_cb, b);
}

/* TT #328 Wave 2 — dynamic-string banner variant.  Dragon-side error
 * messages (config_update.error, transient `error` frames with
 * non-static strings) need a heap-owned copy so the WS rx task can
 * safely return before LVGL processes the async call.  Strdups into
 * a heap-owned struct, the cb shows the banner + frees both.  Pass
 * `dismissable=true` to allow user-tap dismiss (the standard recovery
 * path); `false` for system-cleared-only banners. */
typedef struct {
   char *text; /* heap, freed in cb after lvgl reads it */
   bool dismissable;
} voice_banner_dyn_t;

static void async_dismiss_banner_cb(void) {
   /* Wave 2: tap-to-dismiss handler.  ui_home_show_error_banner installs
    * us as the cb when dismissable=true; firing means the user
    * acknowledged so we just clear. */
   ui_home_clear_error_banner();
}

static void async_show_error_banner_dyn_cb(void *arg) {
   voice_banner_dyn_t *b = (voice_banner_dyn_t *)arg;
   if (!b) return;
   if (b->text) {
      ui_home_show_error_banner(b->text, b->dismissable ? async_dismiss_banner_cb : NULL);
      free(b->text);
   }
   free(b);
}

static void voice_async_error_banner_dyn(const char *text, bool dismissable) {
   if (!text || !text[0]) return;
   voice_banner_dyn_t *b = heap_caps_malloc(sizeof(*b), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!b) {
      ESP_LOGW(TAG, "voice_async_error_banner_dyn OOM (struct) — banner dropped");
      return;
   }
   size_t len = strlen(text) + 1;
   b->text = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!b->text) {
      ESP_LOGW(TAG, "voice_async_error_banner_dyn OOM (text) — banner dropped");
      free(b);
      return;
   }
   memcpy(b->text, text, len);
   b->dismissable = dismissable;
   tab5_lv_async_call(async_show_error_banner_dyn_cb, b);
}

/* TT #328 Wave 2 — recovery-aware banner state.  When config_update.error
 * fires we mark the fallback banner active; on the next successful
 * llm_done we auto-clear it (recovery signal).  Volatile because the
 * flag is written from the WS rx task and read from llm_done handling
 * on the same task — no contention, but the compiler might reorder
 * across the lv_async_call boundary otherwise. */
static volatile bool s_fallback_banner_active = false;

static void async_clear_banner_cb(void *arg) {
   (void)arg;
   ui_home_clear_error_banner();
}

static void voice_async_refresh_badge(void) {
   voice_async_badge_t *b = malloc(sizeof(voice_async_badge_t));
   if (!b) {
      ESP_LOGW(TAG, "voice_async_refresh_badge OOM — badge will lag a tick");
      return;
   }
   b->gen = s_session_gen;
   tab5_lv_async_call(async_refresh_badge_cb, b);
}

// ---------------------------------------------------------------------------
// JSON message handling (Dragon -> Tab5)
// ---------------------------------------------------------------------------

/* TT #328 Wave 2 — debug entry-point exposed via voice.h so the e2e
 * harness can fire synthetic WS frames into the dispatcher without
 * needing Dragon to misbehave.  Tiny wrapper, no validation beyond
 * the len > 0 guard — the JSON parser inside handle_text_message
 * already rejects malformed input. */
void voice_debug_inject_text(const char *data, int len) {
   if (!data || len <= 0) return;
   voice_ws_proto_handle_text(data, len);
}

/* TT #331 Wave 23 SRP-A1: was static void handle_text_message — promoted + renamed for the voice_ws_proto layer. */
void voice_ws_proto_handle_text(const char *data, int len) {
   cJSON *root = cJSON_ParseWithLength(data, len);
   if (!root) {
      ESP_LOGW(TAG, "Failed to parse JSON: %.*s", len, data);
      return;
   }

   cJSON *type = cJSON_GetObjectItem(root, "type");
   if (!cJSON_IsString(type)) {
      ESP_LOGW(TAG, "JSON missing 'type': %.*s", len, data);
      cJSON_Delete(root);
      return;
   }

   const char *type_str = type->valuestring;

   if (strcmp(type_str, "stt_partial") == 0) {
      cJSON *text = cJSON_GetObjectItem(root, "text");
      /* U12 (#206): regardless of dictation mode, surface the partial
       * as a live caption above the chat input pill.  No-op when chat
       * is closed (s_input is NULL inside ui_chat). */
      if (cJSON_IsString(text) && text->valuestring) {
         ui_chat_show_partial(text->valuestring);
      }
      if (cJSON_IsString(text) && text->valuestring && voice_get_mode() == VOICE_MODE_DICTATE && s_dictation_text) {
         /* v4·D audit P1 fix: use bounded copy instead of unchecked
          * strcat.  Previously the guard compared cur+add+2 against
          * the buffer size, but under mutex-less concurrent writes
          * cur_len could change between the check and the strcat.
          * Take the state mutex and re-bound with snprintf so an
          * overflow is impossible. */
         if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
         size_t cur_len = strlen(s_dictation_text);
         size_t remaining = (cur_len + 1 < DICTATION_TEXT_SIZE) ? (DICTATION_TEXT_SIZE - cur_len - 1) : 0;
         if (remaining > 1) {
            const char *sep = (cur_len > 0) ? " " : "";
            snprintf(s_dictation_text + cur_len, remaining, "%s%s", sep, text->valuestring);
         }
         if (s_state_mutex) xSemaphoreGive(s_state_mutex);
         ESP_LOGI(TAG, "STT partial: \"%s\" (total %u chars)", text->valuestring, (unsigned)strlen(s_dictation_text));
         voice_set_state(VOICE_STATE_LISTENING, s_dictation_text);
      }
   } else if (strcmp(type_str, "stt") == 0) {
      cJSON *text = cJSON_GetObjectItem(root, "text");
      /* U12 (#206): final STT result lands as a real chat bubble — clear
       * the live partial caption so it doesn't linger above the pill. */
      ui_chat_show_partial(NULL);
      if (cJSON_IsString(text) && text->valuestring) {
         if (voice_get_mode() == VOICE_MODE_DICTATE) {
            if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            strncpy(s_stt_text, text->valuestring, MAX_TRANSCRIPT_LEN - 1);
            s_stt_text[MAX_TRANSCRIPT_LEN - 1] = '\0';
            if (s_state_mutex) xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "Dictation complete: %u chars", (unsigned)strlen(text->valuestring));
            voice_set_state(VOICE_STATE_READY, "dictation_done");
         } else {
            /* W7-E.4b: if a channel reply is armed, route the transcript
             * to voice_send_channel_reply instead of normal LLM dispatch.
             * Atomic read-and-clear via voice_consume_channel_reply. */
            char reply_ch[16] = {0}, reply_thread[64] = {0}, reply_sender[64] = {0};
            if (voice_consume_channel_reply(reply_ch, reply_thread, reply_sender)) {
               ESP_LOGI(TAG, "STT armed reply → ch=%s thread=%s text=%.40s", reply_ch, reply_thread, text->valuestring);
               esp_err_t r = voice_send_channel_reply(reply_ch, reply_thread, text->valuestring);
               char detail[64];
               snprintf(detail, sizeof(detail), "dictated ch=%.8s err=%d", reply_ch, (int)r);
               tab5_debug_obs_event("ui.notif.reply", detail);

               char *toast = malloc(96);
               if (toast) {
                  if (r == ESP_OK) {
                     snprintf(toast, 96, "Reply sent to %.40s", reply_sender[0] ? reply_sender : reply_ch);
                  } else {
                     snprintf(toast, 96, "Reply not sent — Dragon offline");
                  }
                  voice_async_toast(toast);
               }
               voice_set_state(VOICE_STATE_READY, NULL);
               cJSON_Delete(root);
               return;
            }

            if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            strncpy(s_stt_text, text->valuestring, MAX_TRANSCRIPT_LEN - 1);
            s_stt_text[MAX_TRANSCRIPT_LEN - 1] = '\0';
            strncpy(s_transcript, text->valuestring, MAX_TRANSCRIPT_LEN - 1);
            s_transcript[MAX_TRANSCRIPT_LEN - 1] = '\0';
            s_llm_text[0] = '\0';
            if (s_state_mutex) xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "STT: \"%s\"", s_stt_text);
            ui_chat_push_message("user", text->valuestring);
            voice_set_state(VOICE_STATE_PROCESSING, s_stt_text);
         }
      }
   } else if (strcmp(type_str, "tts_start") == 0) {
      /* Audit #80 DMA leak hunt (wave 9): log heap state at the 5
       * interesting boundaries of a chat turn (llm_done, tts_start,
       * tts_end, media, text_update) so we can diff which stage leaks.
       * Heap caps are DMA-capable internal SRAM — same pool the WiFi
       * Rx ring needs to stay alive. */
      ESP_LOGI(TAG, "TTS start — preparing playback | heap_dma_free=%u largest=%u",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
      tab5_audio_speaker_enable(true);
      voice_playback_buf_reset();
      voice_ws_proto_upsample_reset(); /* TT #568: clean context per turn */
      voice_set_state(VOICE_STATE_SPEAKING, NULL);
   } else if (strcmp(type_str, "tts_end") == 0) {
      ESP_LOGI(TAG, "TTS end — drain task will finish playback | heap_dma_free=%u largest=%u",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
      s_tts_done = true;
      if (s_play_sem) xSemaphoreGive(s_play_sem);
   } else if (strcmp(type_str, "llm") == 0) {
      /* TinkerBox #91 follow-up (#193): ignore late llm tokens that
       * arrive after a cancel.  Tab5 transitions to READY locally on
       * cancel-send (see voice_cancel below); Dragon now actually
       * interrupts the LLM stream within ~1 ms (was ~65 s pre-#91),
       * but tokens already in TCP flight at cancel-time still arrive
       * a few hundred ms later.  Without this guard, the unconditional
       * voice_set_state below pulls the orb back into PROCESSING with
       * the partial cancelled response text — the cancel APPEARS to
       * not have worked from the user's perspective.
       *
       * Honor llm tokens only when the user is actively waiting for a
       * turn (PROCESSING or SPEAKING).  All other states (READY after
       * cancel, IDLE on disconnect, LISTENING when a new mic recording
       * already started) mean the previous turn is no longer the user's
       * focus and any late tokens belong to a turn they cancelled.
       */
      voice_state_t cur_for_llm = voice_get_state();
      if (cur_for_llm != VOICE_STATE_PROCESSING && cur_for_llm != VOICE_STATE_SPEAKING) {
         ESP_LOGD(TAG, "llm token ignored — state=%d (post-cancel late arrival)", cur_for_llm);
         cJSON_Delete(root);
         return;
      }
      cJSON *text = cJSON_GetObjectItem(root, "text");
      if (cJSON_IsString(text) && text->valuestring) {
         if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
         size_t llm_len = strlen(s_llm_text);
         size_t add_len = strlen(text->valuestring);
         if (llm_len + add_len < MAX_TRANSCRIPT_LEN - 1) {
            strcat(s_llm_text, text->valuestring);
         }
         size_t cur_len = strlen(s_transcript);
         if (cur_len + add_len < MAX_TRANSCRIPT_LEN - 1) {
            strcat(s_transcript, text->valuestring);
         }
         if (s_state_mutex) xSemaphoreGive(s_state_mutex);
         ESP_LOGD(TAG, "LLM token: \"%s\"", text->valuestring);
         voice_set_state(VOICE_STATE_PROCESSING, s_llm_text);
      }
   } else if (strcmp(type_str, "cancel_ack") == 0) {
      /* TinkerBox #91: server-side confirmation that cancel landed.
       * Today we just log it — the state machine already transitioned
       * to READY when voice_cancel was called, and the llm-token
       * guard above handles the late-arrival grace window.
       * If we ever add a "definitely no more late tokens, safe to
       * resume" semantic, this is where it would slot in. */
      cJSON *cancelled = cJSON_GetObjectItem(root, "cancelled");
      int n = cJSON_IsArray(cancelled) ? cJSON_GetArraySize(cancelled) : 0;
      ESP_LOGI(TAG, "cancel_ack: cancelled=%d slot(s)", n);
   } else if (strcmp(type_str, "error") == 0) {
      /* γ2-H8 (issue #196): route Dragon error frames by severity.
       *
       * Pre-fix every {"type":"error"} frame landed in the voice-
       * overlay caption regardless of what went wrong — a recoverable
       * STT-empty toast and an unrecoverable session-invalid banner
       * both ended up in the same place.  Dragon's γ1 taxonomy
       * (TinkerBox PR #102) added structured `severity` ("transient"
       * vs "fatal") and `scope` fields; this handler honours them:
       *
       *   - TRANSIENT → non-blocking ui_home_show_toast(), stay in
       *     READY.  User can keep talking.
       *   - FATAL → existing voice-state caption path (more
       *     permanent surface).  Reset playback + speaker since a
       *     fatal error means we shouldn't keep half-playing TTS.
       *
       * Special case: code="device_evicted" (TinkerBox γ2-M5, PR
       * #109) means another device claimed our slot.  Auto-reconnect
       * would just trigger an eviction loop, so we set the
       * disconnect guard + stop the WS client.  User must
       * power-cycle or re-launch via the debug endpoint.
       */
      cJSON *msg = cJSON_GetObjectItem(root, "message");
      cJSON *severity = cJSON_GetObjectItem(root, "severity");
      cJSON *code = cJSON_GetObjectItem(root, "code");
      const char *err_src = cJSON_IsString(msg) ? msg->valuestring : "unknown";
      /* Default to "fatal" for unknown / pre-γ1 frames — safer to
       * surface in caption (more visible) than to silently toast.
       * Tab5 is forward-compatible with future severity values too:
       * anything not "transient" routes to caption. */
      const char *sev_src = cJSON_IsString(severity) ? severity->valuestring : "fatal";
      const char *code_src = cJSON_IsString(code) ? code->valuestring : "";
      ESP_LOGE(TAG, "Dragon error [%s/%s]: %s", sev_src, code_src, err_src);

      char err_buf[128];
      strncpy(err_buf, err_src, sizeof(err_buf) - 1);
      err_buf[sizeof(err_buf) - 1] = '\0';

      bool is_transient = (strcmp(sev_src, "transient") == 0);

      if (is_transient) {
         /* TRANSIENT → toast via the existing voice_async_toast()
          * helper.  This handler runs on the WS task, NOT the LVGL
          * task — voice_async_toast() takes ownership of a strdup'd
          * buffer, queues it via lv_async_call, and stamps the
          * session_gen so a stale toast from a prior connection
          * doesn't surface after a reconnect. */
         char *toast_copy = strdup(err_buf);
         if (toast_copy) {
            voice_async_toast(toast_copy);
         }
      } else {
         /* FATAL → existing caption path. */
         voice_playback_buf_reset();
         tab5_audio_speaker_enable(false);
         bool connected = (g_voice_ws != NULL) && esp_websocket_client_is_connected(g_voice_ws);
         voice_set_state(connected ? VOICE_STATE_READY : VOICE_STATE_IDLE, err_buf);

         /* device_evicted: don't loop the eviction.  Stop the WS
          * client + set the disconnect guard so the
          * WEBSOCKET_EVENT_DISCONNECTED handler doesn't try to
          * reconnect.  User regains connectivity via power-cycle
          * or the /voice/reconnect debug endpoint.
          *
          * IMPORTANT: esp_websocket_client_stop() rejects calls
          * from the WS task itself (logs "Client cannot be
          * stopped from websocket task" and no-ops).  Hop to the
          * shared worker queue (task_worker.{c,h}) so the stop
          * runs on a non-WS task. */
         if (strcmp(code_src, "device_evicted") == 0 && g_voice_ws) {
            ESP_LOGW(TAG, "device_evicted: scheduling WS stop to prevent reconnect loop");
            s_disconnecting = true;
            tab5_worker_enqueue(_voice_stop_ws_worker_fn, NULL, "device_evicted_stop");
         }
      }
   } else if (strcmp(type_str, "session_start") == 0) {
      cJSON *sid = cJSON_GetObjectItem(root, "session_id");
      if (cJSON_IsString(sid) && sid->valuestring) {
         tab5_settings_set_session_id(sid->valuestring);
         ESP_LOGI(TAG, "Session: %s", sid->valuestring);
      }
      cJSON *resumed = cJSON_GetObjectItem(root, "resumed");
      cJSON *msg_cnt = cJSON_GetObjectItem(root, "message_count");
      ESP_LOGI(TAG, "session_start: resumed=%s messages=%d", (cJSON_IsTrue(resumed) ? "yes" : "no"),
               cJSON_IsNumber(msg_cnt) ? msg_cnt->valueint : 0);

      if (voice_get_state() == VOICE_STATE_CONNECTING) {
         voice_set_state(VOICE_STATE_READY, NULL);
         ESP_LOGI(TAG, "Connected to Dragon voice server");

         uint8_t saved_mode = tab5_settings_get_voice_mode();
         char saved_model[64] = {0};
         tab5_settings_get_llm_model(saved_model, sizeof(saved_model));
         ESP_LOGI(TAG, "Restoring voice_mode=%d model='%s' on reconnect", saved_mode,
                  saved_model[0] ? saved_model : "(default)");
         voice_send_config_update((int)saved_mode, saved_model[0] ? saved_model : NULL);

         ui_notes_sync_pending();
      }
   } else if (strcmp(type_str, "session_messages") == 0) {
      /* Audit C8/K15 (2026-04-20): Dragon replays the tail of session
       * messages on a resumed connect.  Rehydrate chat_store so the
       * user sees their conversation after a reconnect.  Pushed via
       * ui_chat_push_message which is thread-safe + lv_async_call'd. */
      cJSON *items = cJSON_GetObjectItem(root, "items");
      if (cJSON_IsArray(items)) {
         int n = cJSON_GetArraySize(items);
         ESP_LOGI(TAG, "session_messages replay: %d items", n);
         for (int i = 0; i < n; i++) {
            cJSON *m = cJSON_GetArrayItem(items, i);
            if (!m) continue;
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
            const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
            if (!role || !content || !content[0]) continue;
            /* Skip system/tool rows — chat UI only renders
             * user/assistant today. */
            if (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0) continue;
            ui_chat_push_message(role, content);
         }
      }
   } else if (strcmp(type_str, "dictation_postprocessing") == 0) {
      /* TinkerBox#94 H4: Dragon spawned the title+summary LLM call after
       * `stt`.  Pre-fix the user stared at the bare transcript for
       * 10-20 s with no signal that more was coming.  Show a status
       * caption so the wait feels intentional. */
      ESP_LOGI(TAG, "Dictation post-process started");
      voice_set_state(VOICE_STATE_PROCESSING, "Generating summary...");
   } else if (strcmp(type_str, "dictation_postprocessing_error") == 0) {
      /* TinkerBox#94 H4: LLM failed or wasn't available.  Note already
       * saved (the transcript landed via the prior `stt` event); user
       * just doesn't get an auto-generated title/summary.  Clear the
       * "Generating summary..." caption and toast the friendly
       * message. */
      cJSON *msg = cJSON_GetObjectItem(root, "message");
      const char *m = cJSON_IsString(msg) ? msg->valuestring : "Note saved — summary unavailable";
      ESP_LOGW(TAG, "Dictation post-process error: %s", m);
      char buf[160];
      strncpy(buf, m, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      voice_async_toast(strdup(buf));
      voice_set_state(VOICE_STATE_READY, "dictation_done");
   } else if (strcmp(type_str, "dictation_postprocessing_cancelled") == 0) {
      /* TinkerBox#94 H4: a NEW dictation superseded the prior in-flight
       * post-process.  The new dictation will emit its own
       * `dictation_postprocessing` event a few ms later — silently log
       * here so we don't fight the new caption.
       *
       * TT #328 Wave 2 (audit error-surfacing) — the original handler
       * was log-only, but on the cancellation-WITHOUT-followup path
       * (Dragon shut down, user disconnected mid-dictation, failover
       * race) the user is left staring at "Generating summary..."
       * forever because no dictation_postprocessing comes to override
       * it.  Reset to READY so the caption clears; if a legitimate
       * follow-up _postprocessing fires it'll re-set the caption a
       * few ms later anyway, no UI flicker visible. */
      ESP_LOGI(TAG, "Dictation post-process cancelled (superseded or aborted)");
      voice_set_state(VOICE_STATE_READY, "dictation_cancelled");
      /* PR 1: pipeline transition for the cancelled path. */
      voice_dictation_set_state(DICT_FAILED, DICT_FAIL_CANCELLED, (uint32_t)(esp_timer_get_time() / 1000));
      /* #537: discard the pipeline-armed WAV — no transcript is coming. */
      tab5_lv_async_call((lv_async_cb_t)ui_notes_pipeline_cancel_recording, NULL);
   } else if (strcmp(type_str, "dictation_summary") == 0) {
      cJSON *title = cJSON_GetObjectItem(root, "title");
      cJSON *summary = cJSON_GetObjectItem(root, "summary");
      if (cJSON_IsString(title)) {
         strncpy(s_dictation_title, title->valuestring, sizeof(s_dictation_title) - 1);
         s_dictation_title[sizeof(s_dictation_title) - 1] = '\0';
      }
      if (cJSON_IsString(summary)) {
         strncpy(s_dictation_summary, summary->valuestring, sizeof(s_dictation_summary) - 1);
         s_dictation_summary[sizeof(s_dictation_summary) - 1] = '\0';
      }
      ESP_LOGI(TAG, "Dictation summary: \"%s\"", s_dictation_title);
      voice_set_state(VOICE_STATE_READY, "dictation_summary");

      /* PR 1: pipeline transition.  Non-empty summary → SAVED; empty
       * both → FAILED(EMPTY). */
      const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
      if (s_dictation_title[0] || s_dictation_summary[0]) {
         voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, now);
         /* PR 3 follow-up: surface the dictation as a local Tab5 note
          * so it appears on the Notes timeline.  Path A (local
          * "+ NEW VOICE NOTE" → ui_notes_start_recording) already owns
          * its slot; this handler only fires for Path B (FAB / home
          * Dictate chip via voice_start_dictation with no local slot).
          * The async helper marshals to LVGL thread + no-ops if a
          * local slot is already active so we don't duplicate. */
         const char *transcript = voice_get_dictation_text();
         if (transcript && transcript[0]) {
            ui_notes_add_dictated_async(transcript);
         } else if (s_dictation_summary[0]) {
            ui_notes_add_dictated_async(s_dictation_summary);
         } else {
            ui_notes_add_dictated_async(s_dictation_title);
         }
         /* PR 4: parse Dragon's optional `proposed_action` classifier
          * output + attach it as a pending_chip on the freshly-added
          * slot.  Forward-compat: missing field → no chip rendered.
          * Confidence floor (75) is enforced inside ui_notes. */
         cJSON *pa = cJSON_GetObjectItem(root, "proposed_action");
         if (cJSON_IsObject(pa)) {
            cJSON *jkind = cJSON_GetObjectItem(pa, "kind");
            cJSON *jconf = cJSON_GetObjectItem(pa, "confidence");
            cJSON *jpayload = cJSON_GetObjectItem(pa, "payload");
            uint8_t kind = 0;
            if (cJSON_IsString(jkind) && jkind->valuestring) {
               if (strcmp(jkind->valuestring, "reminder") == 0)
                  kind = 1;
               else if (strcmp(jkind->valuestring, "list") == 0)
                  kind = 2;
            }
            uint8_t confidence = 0;
            if (cJSON_IsNumber(jconf)) {
               double c = jconf->valuedouble;
               /* Spec sends 0.0-1.0; multiply to 0-100.  Be lenient if a
                * future Dragon build sends 0-100 directly. */
               confidence = (uint8_t)(c > 1.0 ? c : c * 100.0);
            }
            char payload_buf[128] = {0};
            if (cJSON_IsObject(jpayload)) {
               /* Reminder: extract `when` (ISO datetime).  List: payload
                * is informational; we currently only need the kind tag
                * since Tab5 splits on commas itself. */
               cJSON *jwhen = cJSON_GetObjectItem(jpayload, "when");
               if (cJSON_IsString(jwhen) && jwhen->valuestring) {
                  strncpy(payload_buf, jwhen->valuestring, sizeof(payload_buf) - 1);
               }
            }
            if (kind != 0) {
               ui_notes_attach_pending_chip_async(kind, confidence, payload_buf);
            }
         }
      } else {
         voice_dictation_set_state(DICT_FAILED, DICT_FAIL_EMPTY, now);
      }
   } else if (strcmp(type_str, "note_created") == 0) {
      cJSON *nid = cJSON_GetObjectItem(root, "note_id");
      cJSON *ntitle = cJSON_GetObjectItem(root, "title");
      ESP_LOGI(TAG, "Dragon auto-created note: id=%s title=\"%s\"", cJSON_IsString(nid) ? nid->valuestring : "?",
               cJSON_IsString(ntitle) ? ntitle->valuestring : "?");
   } else if (strcmp(type_str, "dictation_postprocessing_error") == 0) {
      /* PR 2 polish: Dragon's STT + auto-note-create completed but the
       * LLM summary step failed (no_llm_available, generation error,
       * etc.).  The note IS saved on Dragon (it was created from the
       * STT transcript before the LLM step), so transition the pipeline
       * to SAVED rather than leaving it stuck at TRANSCRIBING.  We use
       * SAVED here even though there's no title/summary because the
       * user's intent was captured — the note exists, just without an
       * auto-generated heading.  Tab5's Notes screen will pick it up
       * on next sync with a fallback title from the transcript. */
      cJSON *err = cJSON_GetObjectItem(root, "error");
      const char *err_str = (cJSON_IsString(err) && err->valuestring) ? err->valuestring : "unknown";
      ESP_LOGW(TAG, "Dictation post-processing failed: %s — pipeline → SAVED (note already exists)", err_str);
      voice_set_state(VOICE_STATE_READY, "dictation_postprocessing_error");
      voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, (uint32_t)(esp_timer_get_time() / 1000));
      /* PR 3 follow-up: same local-note creation path as
       * dictation_summary above.  Dragon auto-created its own note
       * before the LLM step failed, so the transcript is the user's
       * captured content even though we don't have a title/summary.
       * Surface it on Tab5's Notes timeline. */
      const char *transcript = voice_get_dictation_text();
      if (transcript && transcript[0]) {
         ui_notes_add_dictated_async(transcript);
      }
   } else if (strcmp(type_str, "llm_done") == 0) {
      cJSON *ms = cJSON_GetObjectItem(root, "llm_ms");
      ESP_LOGI(TAG, "LLM done (%.0fms) | heap_dma_free=%u largest=%u", cJSON_IsNumber(ms) ? ms->valuedouble : 0.0,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
      /* TT #328 Wave 2 — recovery hook.  The fallback banner that fires
       * on config_update.error sticks until the user dismisses or until
       * a successful turn comes back through.  llm_done is the
       * canonical "things are working again" signal — clear it here so
       * the user sees the "things are back to normal" state without
       * having to manually tap the banner. */
      if (s_fallback_banner_active) {
         ESP_LOGI(TAG, "Wave 2: clearing fallback banner (recovery via llm_done)");
         s_fallback_banner_active = false;
         tab5_lv_async_call(async_clear_banner_cb, NULL);
      }
      /* #293: emit obs event for e2e harness. */
      {
         char latency_str[24];
         snprintf(latency_str, sizeof(latency_str), "%.0f", cJSON_IsNumber(ms) ? ms->valuedouble : 0.0);
         tab5_debug_obs_event("chat.llm_done", latency_str);
      }
      /* Prefer the full text field in llm_done (TC bypass uses it)
       * falling back to the accumulated streamed tokens. */
      cJSON *full = cJSON_GetObjectItem(root, "text");
      const char *bubble_text = s_llm_text;
      if (cJSON_IsString(full) && full->valuestring && full->valuestring[0]) {
         bubble_text = full->valuestring;
      }
      if (bubble_text && bubble_text[0]) {
         /* #78 + #160: defensive Tab5-side scrub of any <tool>...</tool>
          * + <args>...</args> markers that survived Dragon's
          * server-side stripper.  Without this the user sometimes
          * sees raw "<tool>recall</tool><args>{\"query\":...}</args>"
          * land as a chat bubble (caught by the audit screenshot in
          * #160).  The strip handles the bubble destined for chat;
          * s_llm_text continues to hold the raw text in case other
          * paths need it. */
         char clean[MAX_TRANSCRIPT_LEN];
         md_strip_tool_markers(bubble_text, clean, sizeof(clean));
         if (clean[0]) {
            ui_chat_push_message("assistant", clean);
         }
      }
      /* v4·D TC polish: if no TTS is coming (TC bypass never sends
       * tts_start -- gateway is text-only), transition to READY
       * directly.  Without this, state sits in PROCESSING forever
       * after a TC text turn, chat input pill stays on "Thinking...",
       * voice overlay stays blocked. */
      if (voice_get_state() == VOICE_STATE_PROCESSING) {
         voice_set_state(VOICE_STATE_READY, "llm_done");
      }
   } else if (strcmp(type_str, "receipt") == 0) {
      /* SOLID-audit SRP-3 (2026-05-03): receipt accounting + cap-downgrade
       * policy + chat-bubble stamp deferral all moved to voice_billing.c.
       * Voice.c parses the JSON fields and hands them in typed; the
       * billing module owns the rest of the chain. */
      const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(root, "model"));
      cJSON *ptok = cJSON_GetObjectItem(root, "prompt_tokens");
      cJSON *ctok = cJSON_GetObjectItem(root, "completion_tokens");
      cJSON *mils = cJSON_GetObjectItem(root, "cost_mils");
      cJSON *retried_j = cJSON_GetObjectItem(root, "retried");
      voice_billing_record_receipt(model, cJSON_IsNumber(ptok) ? (int)ptok->valuedouble : 0,
                                   cJSON_IsNumber(ctok) ? (int)ctok->valuedouble : 0,
                                   cJSON_IsNumber(mils) ? (int)mils->valuedouble : 0, cJSON_IsTrue(retried_j));
   } else if (strcmp(type_str, "text_update") == 0) {
      const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
      if (text) {
         ESP_LOGI(TAG, "Text update: replacing last AI message");
         ui_chat_update_last_message(text);
      }
   } else if (strcmp(type_str, "vision_capability") == 0) {
      /* v4·D Phase 4b: Dragon advertises which model (if any) can see a
       * camera frame.  Cached for ui_camera to render its violet chip.
       * Empty/zero on local-only or non-vision models.  Triggered via
       * Dragon's config_update ACK path so it lands whenever mode
       * changes. */
      cJSON *can = cJSON_GetObjectItem(root, "can_see");
      cJSON *mdl = cJSON_GetObjectItem(root, "model");
      cJSON *fpm = cJSON_GetObjectItem(root, "per_frame_mils");
      s_vision_capable = cJSON_IsTrue(can);
      s_vision_per_frame_mils = cJSON_IsNumber(fpm) ? (int)fpm->valuedouble : 0;
      const char *m = (cJSON_IsString(mdl) && mdl->valuestring) ? mdl->valuestring : "";
      snprintf(s_vision_model, sizeof(s_vision_model), "%s", m);
      ESP_LOGI(TAG, "Vision capability: %s (model=%s, per_frame=%d mils)", s_vision_capable ? "YES" : "no",
               s_vision_model[0] ? s_vision_model : "none", s_vision_per_frame_mils);
   } else if (strcmp(type_str, "pong") == 0) {
      /* Dragon-level JSON pong — logged only. WS-level ping/pong is
       * handled automatically by esp_websocket_client (pingpong_timeout_sec). */
      ESP_LOGD(TAG, "App-level pong");
   } else if (strcmp(type_str, "cap_downgrade") == 0) {
      /* W5-C (cross-stack audit 2026-05-11): Dragon hit its server-side
       * BUDGET_DAILY_CENTS cap and is telling us to switch to Local.
       * Frame shape:
       *   { "type":"cap_downgrade", "reason":"daily_cap_hit",
       *     "spent_cents":N, "cap_cents":M, "day":"YYYY-MM-DD",
       *     "turn_id":"..." }
       * Mirrors the Tab5→Dragon direction already handled in
       * voice_billing.c — Dragon owns server-side cap enforcement
       * (W5-A/B), Tab5 owns its own NVS budget.  Either side hitting
       * its cap drops both back to Local mode + surfaces a toast. */
      cJSON *spent_j = cJSON_GetObjectItem(root, "spent_cents");
      cJSON *cap_j = cJSON_GetObjectItem(root, "cap_cents");
      int spent = cJSON_IsNumber(spent_j) ? spent_j->valueint : -1;
      int cap = cJSON_IsNumber(cap_j) ? cap_j->valueint : -1;
      ESP_LOGW(TAG, "Server cap hit: spent=%dc cap=%dc — flipping to Local", spent, cap);
      char detail[48];
      snprintf(detail, sizeof detail, "spent=%dc cap=%dc", spent, cap);
      tab5_debug_obs_event("voice.cap_downgrade_recv", detail);
      /* Flip NVS vmode to LOCAL only if we're not already on a Tab5-
       * side-only tier the user explicitly picked (ONBOARD=4 /
       * SOLO=5).  Those modes don't use Dragon's billable backends
       * anyway, so the cap is moot for them; user's pick stands. */
      uint8_t cur = tab5_settings_get_voice_mode();
      if (cur != VMODE_LOCAL_ONBOARD && cur != VMODE_SOLO_DIRECT && cur != 0) {
         tab5_settings_set_voice_mode(0);
         voice_async_refresh_badge();
         ESP_LOGI(TAG, "Cap downgrade: voice_mode %u → 0 (Local)", cur);
      }
      /* User-visible toast.  Pair with persistent banner so a quick
       * dismiss doesn't hide the fact that they got auto-downgraded. */
      const char *msg = "Daily budget cap reached — switched to Local mode";
      voice_async_toast(strdup(msg));
   } else if (strcmp(type_str, "channel_reply_ack") == 0) {
      /* W7-E.4: Dragon's confirmation that a channel_reply landed.
       * Schema: {"type":"channel_reply_ack","thread_id":"...","ok":bool,
       *          "platform_message_id":"...","channel":"...",
       *          ["error":"..."]}.  Toast on either side so the user
       *          sees the outcome. */
      cJSON *ok_node = cJSON_GetObjectItem(root, "ok");
      cJSON *ch_node = cJSON_GetObjectItem(root, "channel");
      cJSON *err_node = cJSON_GetObjectItem(root, "error");
      bool ok = cJSON_IsTrue(ok_node);
      const char *ch = (cJSON_IsString(ch_node) && ch_node->valuestring) ? ch_node->valuestring : "channel";
      if (ok) {
         char *toast = malloc(64);
         if (toast) {
            snprintf(toast, 64, "Replied via %.40s", ch);
            voice_async_toast(toast);
         }
         tab5_debug_obs_event("ui.notif.reply", "ack_ok");
      } else {
         const char *errmsg =
             (cJSON_IsString(err_node) && err_node->valuestring) ? err_node->valuestring : "send failed";
         char *toast = malloc(128);
         if (toast) {
            snprintf(toast, 128, "Reply failed: %.80s", errmsg);
            voice_async_toast(toast);
         }
         char detail[48];
         snprintf(detail, sizeof(detail), "ack_fail %.30s", errmsg);
         tab5_debug_obs_event("ui.notif.reply", detail);
      }
   } else if (strcmp(type_str, "channel_message") == 0) {
      /* W7-E.1: route gateway-watched channel messages (Telegram,
       * WhatsApp, Discord, ...) into the notification surface.  v0
       * implements the toast path only; high-priority / now-card
       * routing lands in W7-E.2. */
      channel_message_t msg;
      memset(&msg, 0, sizeof(msg));

      cJSON *ch = cJSON_GetObjectItem(root, "channel");
      if (cJSON_IsString(ch) && ch->valuestring) {
         strncpy(msg.channel, ch->valuestring, sizeof(msg.channel) - 1);
      }
      cJSON *mid = cJSON_GetObjectItem(root, "message_id");
      if (cJSON_IsString(mid) && mid->valuestring) {
         strncpy(msg.message_id, mid->valuestring, sizeof(msg.message_id) - 1);
      }
      cJSON *tid = cJSON_GetObjectItem(root, "thread_id");
      if (cJSON_IsString(tid) && tid->valuestring) {
         strncpy(msg.thread_id, tid->valuestring, sizeof(msg.thread_id) - 1);
      }
      cJSON *sender = cJSON_GetObjectItem(root, "sender");
      if (cJSON_IsObject(sender)) {
         cJSON *dn = cJSON_GetObjectItem(sender, "display_name");
         if (cJSON_IsString(dn) && dn->valuestring) {
            strncpy(msg.sender, dn->valuestring, sizeof(msg.sender) - 1);
         }
         cJSON *starred = cJSON_GetObjectItem(sender, "starred");
         msg.sender_starred = cJSON_IsTrue(starred);
      }
      /* Prefer `preview` (short) over `text` (full).  Fall back to
       * text when preview is missing so a minimal injection still
       * renders a useful toast. */
      cJSON *preview = cJSON_GetObjectItem(root, "preview");
      cJSON *text_full = cJSON_GetObjectItem(root, "text");
      const char *preview_src = NULL;
      if (cJSON_IsString(preview) && preview->valuestring && preview->valuestring[0]) {
         preview_src = preview->valuestring;
      } else if (cJSON_IsString(text_full) && text_full->valuestring) {
         preview_src = text_full->valuestring;
      }
      if (preview_src) {
         strncpy(msg.preview, preview_src, sizeof(msg.preview) - 1);
      }
      cJSON *pri = cJSON_GetObjectItem(root, "priority");
      if (cJSON_IsString(pri) && pri->valuestring) {
         strncpy(msg.priority, pri->valuestring, sizeof(msg.priority) - 1);
      }
      cJSON *needs = cJSON_GetObjectItem(root, "needs_reply");
      msg.needs_reply = cJSON_IsTrue(needs);

      ESP_LOGI(TAG, "channel_message: ch=%s sender=%s pri=%s preview=\"%.40s\"", msg.channel, msg.sender, msg.priority,
               msg.preview);
      ui_notification_show(&msg);
   } else if (strcmp(type_str, "config_update") == 0) {
      cJSON *error = cJSON_GetObjectItem(root, "error");
      if (cJSON_IsString(error) && error->valuestring[0]) {
         ESP_LOGW(TAG, "Config update error from Dragon: %s", error->valuestring);
         /* TT #317 Phase 5: don't clobber a Tab5-only ONBOARD pick. */
         if (tab5_settings_get_voice_mode() != VMODE_LOCAL_ONBOARD) {
            tab5_settings_set_voice_mode(0);
         }
         voice_async_refresh_badge();
         {
            size_t elen = strlen(error->valuestring);
            if (elen > 80) elen = 80;
            char *toast_msg = malloc(elen + 1);
            if (toast_msg) {
               memcpy(toast_msg, error->valuestring, elen);
               toast_msg[elen] = '\0';
               voice_async_toast(toast_msg);
            }
         }
         /* TT #328 Wave 2 (audit error-surfacing) — toast was the
          * only surface; auto-dismissed in 3 s.  Users glancing
          * away missed the fact that they got auto-downgraded to
          * Local + lost cloud STT/TTS quality.  Pair the toast
          * with a persistent dismissable error banner that sits
          * until the user acknowledges OR until the next
          * successful llm_done auto-clears it (recovery signal). */
         voice_async_error_banner_dyn(error->valuestring, true /* dismissable */);
         s_fallback_banner_active = true;
         voice_set_state(VOICE_STATE_READY, error->valuestring);
      }
      cJSON *vmode = cJSON_GetObjectItem(root, "voice_mode");
      if (!cJSON_IsNumber(vmode)) {
         cJSON *config_obj = cJSON_GetObjectItem(root, "config");
         if (config_obj) {
            vmode = cJSON_GetObjectItem(config_obj, "voice_mode");
         }
      }
      if (cJSON_IsNumber(vmode)) {
         uint8_t mode = (uint8_t)vmode->valueint;
         /* TT #317 Phase 5 + TT #370 Phase 1: ONBOARD (4) and
          * SOLO_DIRECT (5) are both Tab5-side-only tiers.  Dragon
          * never knows about them — its ACK always echoes 0/1/2/3.
          * If user has set one of those locally, ignore Dragon's echo. */
         uint8_t cur = tab5_settings_get_voice_mode();
         if (cur != VMODE_LOCAL_ONBOARD && cur != VMODE_SOLO_DIRECT) {
            tab5_settings_set_voice_mode(mode);
            ESP_LOGI(TAG, "Config update: voice_mode=%d (persisted)", mode);
         } else {
            ESP_LOGD(TAG, "Config update: ignoring Dragon vmode=%d (we're in %u)", mode, cur);
         }
         voice_async_refresh_badge();
      }
      cJSON *config = cJSON_GetObjectItem(root, "config");
      bool applied_cloud = false;
      if (config) {
         cJSON *cloud = cJSON_GetObjectItem(config, "cloud_mode");
         if (cJSON_IsBool(cloud)) {
            bool is_cloud = cJSON_IsTrue(cloud);
            if (!cJSON_IsNumber(vmode)) {
               tab5_settings_set_voice_mode(is_cloud ? 1 : 0);
               voice_async_refresh_badge();
               applied_cloud = true;
            }
         }
      }
      /* #262: codec negotiation reply.  Dragon picks one of the
       * codecs Tab5 advertised in `register` and tells us via
       * config_update.audio_codec.  Two flavours so it can choose
       * uplink (mic) and downlink (TTS) independently:
       *   - audio_codec        : applies to both (legacy / shorthand)
       *   - audio_uplink_codec : mic only
       *   - audio_downlink_codec: TTS only
       * Unrecognized strings fall back to PCM. */
      cJSON *acu = cJSON_GetObjectItem(root, "audio_uplink_codec");
      cJSON *acd = cJSON_GetObjectItem(root, "audio_downlink_codec");
      cJSON *ac = cJSON_GetObjectItem(root, "audio_codec");
      if (cJSON_IsString(acu)) {
         voice_codec_set_uplink(voice_codec_from_name(acu->valuestring));
      } else if (cJSON_IsString(ac)) {
         voice_codec_set_uplink(voice_codec_from_name(ac->valuestring));
      }
      if (cJSON_IsString(acd)) {
         voice_codec_set_downlink(voice_codec_from_name(acd->valuestring));
      } else if (cJSON_IsString(ac)) {
         voice_codec_set_downlink(voice_codec_from_name(ac->valuestring));
      }
      /* Wave 14 W14-L03: if neither voice_mode nor cloud_mode was
       * present, the handler used to exit silently and Tab5 would
       * disagree with Dragon about the active mode with no trace.
       * Log at DEBUG so it's visible via /log without spamming INFO. */
      if (!cJSON_IsNumber(vmode) && !applied_cloud && !cJSON_IsString(error)) {
         ESP_LOGD(TAG,
                  "config_update received with no voice_mode/"
                  "cloud_mode/error field — no-op");
      }
   } else if (strcmp(type_str, "tool_call") == 0) {
      cJSON *tool = cJSON_GetObjectItem(root, "tool");
      const char *tool_name = cJSON_IsString(tool) ? tool->valuestring : "unknown";
      ESP_LOGI(TAG, "Tool call: %s", tool_name);

      const char *status_text = "Thinking...";
      if (strcmp(tool_name, "web_search") == 0) {
         status_text = "Searching the web...";
      } else if (strcmp(tool_name, "remember") == 0 || strcmp(tool_name, "memory_store") == 0) {
         status_text = "Remembering...";
      } else if (strcmp(tool_name, "memory_search") == 0 || strcmp(tool_name, "recall") == 0) {
         status_text = "Recalling...";
      } else if (strcmp(tool_name, "browser") == 0 || strcmp(tool_name, "browse") == 0) {
         status_text = "Browsing...";
      } else if (strcmp(tool_name, "calculator") == 0 || strcmp(tool_name, "math") == 0) {
         status_text = "Calculating...";
      } else {
         status_text = "Using tools...";
      }
      voice_set_state(VOICE_STATE_PROCESSING, status_text);
      /* Tab5 audit D5: also push a system bubble so the user can see
       * tool activity in chat (not just on the voice overlay label).
       * CLAUDE.md's "thinking + tool indicator bubbles" claim was wired
       * only to the overlay-state string previously. */
      ui_chat_push_system(status_text);
      /* U7+U8 (#206): record activity so the agents/focus surfaces
       * can render real history instead of the v5 demo rows. */
      tool_log_push_call(tool_name, status_text);
      /* TT #501: visual ripple from the orb, color-coded by source
       * bucket (W7-A.3 / W7-G).  Browser tools = blue, channel = rose,
       * mode-3 gateway = violet, else emerald.  Safe from WS task
       * (ui_home_orb_ripple_for_tool marshals via tab5_lv_async_call). */
      ui_home_orb_ripple_for_tool(tool_name);
   } else if (strcmp(type_str, "tool_result") == 0) {
      cJSON *tool = cJSON_GetObjectItem(root, "tool");
      cJSON *exec_ms = cJSON_GetObjectItem(root, "execution_ms");
      const char *tool_name = cJSON_IsString(tool) ? tool->valuestring : "unknown";
      double ms = cJSON_IsNumber(exec_ms) ? exec_ms->valuedouble : 0.0;
      ESP_LOGI(TAG, "Tool result: %s (%.0fms)", tool_name, ms);
      voice_set_state(VOICE_STATE_PROCESSING, "Thinking...");
      /* Tab5 audit D5: close the loop with a completion bubble so the
       * chat timeline shows what ran + how long it took. */
      char done_buf[80];
      snprintf(done_buf, sizeof(done_buf), "%s done (%.0fms)", tool_name, ms);
      ui_chat_push_system(done_buf);
      /* U7+U8 (#206): mark the tool_log entry done so the
       * agents/focus surfaces can show "DONE  •  234 ms". */
      tool_log_push_result(tool_name, (uint32_t)ms);
   } else if (strcmp(type_str, "media") == 0) {
      const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
      const char *mtype = cJSON_GetStringValue(cJSON_GetObjectItem(root, "media_type"));
      cJSON *w_item = cJSON_GetObjectItem(root, "width");
      cJSON *h_item = cJSON_GetObjectItem(root, "height");
      int w = cJSON_IsNumber(w_item) ? (int)w_item->valuedouble : 0;
      int h = cJSON_IsNumber(h_item) ? (int)h_item->valuedouble : 0;
      const char *alt = cJSON_GetStringValue(cJSON_GetObjectItem(root, "alt"));
      if (url) {
         ESP_LOGI(TAG, "Media: %s %dx%d", mtype ? mtype : "image", w, h);
         ui_chat_push_media(url, mtype, w, h, alt);
      }
   } else if (strcmp(type_str, "card") == 0) {
      const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
      const char *sub = cJSON_GetStringValue(cJSON_GetObjectItem(root, "subtitle"));
      const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(root, "image_url"));
      const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "description"));
      if (title) {
         ESP_LOGI(TAG, "Card: %s", title);
         ui_chat_push_card(title, sub, img, desc);
      }
      /* Wave 23b SOLID-audit SRP-2: nine widget WS verbs (widget_card,
       * widget_live, widget_live_update, widget_list, widget_chart,
       * widget_media, widget_prompt, widget_live_dismiss, widget_dismiss)
       * extracted to voice_widget_ws.{c,h}.  Returns true iff the verb
       * was recognized + handled; false falls through to the unknown-
       * verb log below. */
   } else if (voice_widget_ws_dispatch(type_str, root)) {
      /* widget verb handled inside voice_widget_ws.c */
   } else if (strcmp(type_str, "audio_clip") == 0) {
      const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
      cJSON *dur_item = cJSON_GetObjectItem(root, "duration_s");
      float dur = cJSON_IsNumber(dur_item) ? (float)dur_item->valuedouble : 0.0f;
      const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(root, "label"));
      if (url) {
         ESP_LOGI(TAG, "Audio clip: %s (%.1fs)", label ? label : "", dur);
         ui_chat_push_audio_clip(url, dur, label);
      }
   } else {
      ESP_LOGW(TAG, "Unknown message type: %s (full: %.*s)", type_str, len, data);
   }

   cJSON_Delete(root);
}

/* TT #331 Wave 23 SRP-A1: mirror voice.c's UPSAMPLE_* macros so
 * voice_ws_proto_handle_binary can size its half-buffers.  These MUST
 * match the voice.c definitions — the upsample buffer (s_upsample_buf,
 * allocated in voice_init) is sized by UPSAMPLE_BUF_CAPACITY there. */
#define UPSAMPLE_RATIO (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)
#define UPSAMPLE_BUF_CAPACITY (8192 * 2)

/* PSRAM upsample buffer — owned + allocated in voice.c voice_init,
 * referenced here from voice_ws_proto_handle_binary. */
extern int16_t *s_upsample_buf;

// ---------------------------------------------------------------------------
// Task 1.7: Binary frame handling (voice_ws_proto_handle_binary).
// Dragon sends PCM int16 mono at 16kHz; I2S TX runs at 48kHz, so we
// upsample 1:3 before writing to the playback ring.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Binary frame handling (TTS audio from Dragon) — called from event handler.
// Dragon sends PCM int16 mono at 16kHz; I2S TX runs at 48kHz, so we upsample
// 1:3 before writing to the playback ring buffer. Playback drain task
// handles I2S writes.
// ---------------------------------------------------------------------------
/* TT #568: 4-tap cubic Hermite (Catmull-Rom variant) replaces the naive
 * linear interp.  Linear interp had aliasing on Kokoro's sibilants;
 * Hermite is smoother.
 *
 * Indexing (CORRECTED — initial PR #569 was off-by-one which made
 * audio sound robotic/choppy):
 *
 *   For each input sample in[i], emit a triple at output position 3i:
 *     y0 = in[i-1]  (previous, from tail at chunk start)
 *     y1 = in[i]    (current — outputs as identity at t=0)
 *     y2 = in[i+1]  (next, edge-mirrors at chunk tail)
 *     y3 = in[i+2]  (after-next, edge-mirrors at chunk tail)
 *
 *     out[3i+0] = y1                                       (current sample)
 *     out[3i+1] = (-2·y0 + 21·y1 +  9·y2 -   y3) / 27      (1/3 between y1, y2)
 *     out[3i+2] = (  -y0 +  9·y1 + 21·y2 - 2·y3) / 27      (2/3 between y1, y2)
 *
 * Hermite weights derived from the standard basis with finite-
 * difference tangents.  Worst-case int32 magnitude:
 * 33·INT16_MAX ≈ 1.08e9 — well under INT32_MAX.
 *
 * `s_upsample_tail` holds the LAST input sample of the previous chunk
 * so the new chunk's i=0 iteration has a real y0 instead of edge-
 * mirroring in[0].  Reset by voice_ws_proto_upsample_reset() at
 * tts_start so state doesn't leak across turns.
 */
static int16_t s_upsample_tail = 0;
static bool s_upsample_tail_valid = false;

void voice_ws_proto_upsample_reset(void) {
   s_upsample_tail = 0;
   s_upsample_tail_valid = false;
}

static inline int16_t hermite_sample_a(int16_t y0, int16_t y1, int16_t y2, int16_t y3) {
   /* t = 1/3 → out = (-2y0 + 21y1 + 9y2 - y3) / 27 */
   int32_t v = -2 * (int32_t)y0 + 21 * (int32_t)y1 + 9 * (int32_t)y2 - (int32_t)y3;
   /* Round-half-up division by 27 with sign handling. */
   v = (v >= 0) ? (v + 13) / 27 : (v - 13) / 27;
   if (v > 32767) v = 32767;
   if (v < -32768) v = -32768;
   return (int16_t)v;
}

static inline int16_t hermite_sample_b(int16_t y0, int16_t y1, int16_t y2, int16_t y3) {
   /* t = 2/3 → out = (-y0 + 9y1 + 21y2 - 2y3) / 27 */
   int32_t v = -(int32_t)y0 + 9 * (int32_t)y1 + 21 * (int32_t)y2 - 2 * (int32_t)y3;
   v = (v >= 0) ? (v + 13) / 27 : (v - 13) / 27;
   if (v > 32767) v = 32767;
   if (v < -32768) v = -32768;
   return (int16_t)v;
}

/* TT #331 Wave 23 SRP-A1: was static — file-scope inside voice_ws_proto.c. */
static size_t voice_ws_proto_upsample_16k_to_48k(const int16_t *in, size_t in_samples, int16_t *out,
                                                 size_t out_capacity) {
   if (in_samples == 0) return 0;

   size_t out_idx = 0;

   /* y0 for i=0 comes from the previous chunk's last sample (if any),
    * else edge-mirror in[0].  Subsequent iterations roll forward. */
   int16_t prev_in = s_upsample_tail_valid ? s_upsample_tail : in[0];

   for (size_t i = 0; i < in_samples; i++) {
      if (out_idx + UPSAMPLE_RATIO > out_capacity) break;
      int16_t y0 = prev_in;
      int16_t y1 = in[i];
      int16_t y2 = (i + 1 < in_samples) ? in[i + 1] : in[in_samples - 1];
      int16_t y3 = (i + 2 < in_samples) ? in[i + 2] : in[in_samples - 1];
      out[out_idx++] = y1;                               /* t = 0   (identity = in[i]) */
      out[out_idx++] = hermite_sample_a(y0, y1, y2, y3); /* t = 1/3 between in[i] and in[i+1] */
      out[out_idx++] = hermite_sample_b(y0, y1, y2, y3); /* t = 2/3 between in[i] and in[i+1] */
      prev_in = y1;                                      /* for next iteration's y0 */
   }

   /* Save the last input sample for the next call's leading y0. */
   s_upsample_tail = in[in_samples - 1];
   s_upsample_tail_valid = true;

   return out_idx;
}

/* TT #331 Wave 23 SRP-A1: was handle_binary_message (non-static after task 1.6 promotion). */
void voice_ws_proto_handle_binary(const char *data, int len) {
   /* #268: video frames carry the 4-byte "VID0" magic.  Route them
    * to voice_video before any audio-state checks — video plays
    * regardless of voice state (unlike TTS, which only plays in
    * SPEAKING/PROCESSING).  Audio frames (no magic) fall through. */
   if (voice_video_peek_downlink_magic(data, (size_t)len)) {
      voice_video_on_downlink_frame((const uint8_t *)data, (size_t)len);
      return;
   }

   /* #272 Phase 3E: call-audio frames carry the 4-byte "AUD0" magic.
    * Strip the header + write the int16 PCM body straight to the
    * playback buffer with the same upsample 16k->48k as TTS.  No
    * state guards — call audio plays even when voice is READY. */
   if (voice_codec_peek_call_audio_magic(data, (size_t)len)) {
      const uint8_t *body = NULL;
      size_t body_len = 0;
      if (voice_codec_unpack_call_audio((const uint8_t *)data, (size_t)len, &body, &body_len) && body_len >= 2 &&
          s_upsample_buf) {
         tab5_audio_speaker_enable(true);
         const size_t in_samples = body_len / sizeof(int16_t);
         const size_t max_out = in_samples * UPSAMPLE_RATIO;
         if (max_out <= UPSAMPLE_BUF_CAPACITY) {
            size_t out_n =
                voice_ws_proto_upsample_16k_to_48k((const int16_t *)body, in_samples, s_upsample_buf, max_out);
            voice_playback_buf_write(s_upsample_buf, out_n);
         }
      }
      return;
   }

   /* v4·D audit P0 fix: snapshot voice_get_state() so we never race
    * with voice_set_state on another task.  The checks below formerly
    * read s_state twice without locking — voice_get_state takes the
    * state mutex internally so a single call is atomic.
    *
    * TT #331 Wave 23 SRP-A1: pre-extract this site held s_state_mutex
    * directly while reading the static.  Re-locking around
    * voice_get_state() (which takes the same non-recursive mutex)
    * deadlocked → TASK_WDT reset.  Single getter call is the fix. */
   voice_state_t cur = voice_get_state();

   if (cur != VOICE_STATE_SPEAKING && cur != VOICE_STATE_PROCESSING) {
      return;
   }
   if (cur == VOICE_STATE_PROCESSING) {
      voice_playback_buf_reset();
      voice_set_state(VOICE_STATE_SPEAKING, NULL);
   }

   /* #262: decode through voice_codec.  PCM is a memcpy-shaped passthrough
    * (out_samples == in_samples), keeping the existing upsample 16k->48k
    * path unchanged.  OPUS produces variable-length PCM (typ 320 samples
    * per 20 ms packet) which then upsamples the same way. */
   if (!s_upsample_buf) {
      ESP_LOGW(TAG, "handle_binary: upsample_buf not initialized");
      return;
   }

   /* Decode straight into a temp area in s_upsample_buf, low half;
    * upsample reads from there and writes to the high half.
    * Splitting avoids an extra malloc per chunk. */
   int16_t *dec_pcm = s_upsample_buf;                              /* low half */
   int16_t *up_out = s_upsample_buf + (UPSAMPLE_BUF_CAPACITY / 2); /* high half */
   size_t dec_cap = UPSAMPLE_BUF_CAPACITY / 2;
   size_t in_samples = 0;
   if (voice_codec_decode_downlink((const uint8_t *)data, (size_t)len, dec_pcm, dec_cap, &in_samples) != ESP_OK ||
       in_samples == 0) {
      ESP_LOGW(TAG, "handle_binary: codec decode failed (len=%d)", len);
      return;
   }
   size_t max_out = in_samples * UPSAMPLE_RATIO;
   if (max_out > dec_cap) {
      ESP_LOGW(TAG, "handle_binary: upsample would exceed half-buf — truncating");
      max_out = dec_cap;
   }
   size_t out_samples = voice_ws_proto_upsample_16k_to_48k(dec_pcm, in_samples, up_out, max_out);
   voice_playback_buf_write(up_out, out_samples);
}

// ---------------------------------------------------------------------------
// Task 1.6/1.8/1.9: WebSocket event handler + URI helper.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// WebSocket event handler (runs on esp_websocket_client's internal task)
// ---------------------------------------------------------------------------
/* TT #331 Wave 23 SRP-A1: was static void voice_ws_event_handler — promoted +
 * renamed for the voice_ws_proto layer.  Registered with
 * esp_websocket_register_events from voice_ws_start_client in voice.c. */
void voice_ws_proto_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
   esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

   switch ((esp_websocket_event_id_t)event_id) {
      case WEBSOCKET_EVENT_BEFORE_CONNECT:
         ESP_LOGI(TAG, "WS: BEFORE_CONNECT (host=%s%s)", s_dragon_host, s_using_ngrok ? " [ngrok]" : "");
         voice_set_state(VOICE_STATE_CONNECTING, s_dragon_host);
         break;

      case WEBSOCKET_EVENT_CONNECTED: {
         ESP_LOGI(TAG, "WS: CONNECTED — sending register frame");
         /* #293: emit obs event for e2e harness. */
         tab5_debug_obs_event("ws.connect", "");
         /* TT #317 Phase 4: stamp WS-alive so the K144 failover gate can
          * measure how long Dragon's been down before engaging. */
         s_ws_last_alive_us = esp_timer_get_time();
         /* TT #317 Phase 5: if a K144 failover engaged during the down
          * period, surface "Dragon reconnected" so the user knows the
          * next turn is back on the cloud/LAN brain.  Wave 4b: the flag
          * lives in voice_onboard now; consume_engagement_flag returns
          * true once and clears, so the toast fires exactly once per
          * disconnect cycle. */
         if (voice_onboard_consume_engagement_flag()) {
            ESP_LOGI(TAG, "WS reconnected after K144 failover — toast 'Dragon reconnected'");
            if (tab5_ui_try_lock(100)) {
               ui_home_show_toast("Dragon reconnected");
               tab5_ui_unlock();
            }
         }
         /* US-C02: bump session gen on every successful connect. */
         s_session_gen++;
         s_handshake_fail_cnt = 0;
         /* γ3-Tab5 (issue #198): clear the auth-fail counter on a
          * successful connect so a transient 401 (e.g. a token-rotation
          * race during Dragon restart) doesn't get sticky after Dragon
          * recovers.  Only a sustained run of 401s should trip the
          * stop-retry — recovery must self-heal. */
         s_auth_fail_cnt = 0;
         /* T1.1: reset backoff state on every successful connect so a
          * long-running healthy session doesn't get hit with a 30 s
          * delay on its first blip. */
         s_connect_attempt = 0;
         esp_websocket_client_set_reconnect_timeout(g_voice_ws, WS_CLIENT_BACKOFF_MIN_MS);

         esp_err_t reg_err = voice_ws_send_register();
         if (reg_err != ESP_OK) {
            ESP_LOGE(TAG, "WS: register send failed (%s)", esp_err_to_name(reg_err));
            /* Let auto-reconnect re-attempt. */
         }
         /* W3-C-d (cross-stack cohesion audit 2026-05-11): drain any
          * SOLO/K144 turns that were POSTed while Dragon was
          * unreachable.  Best-effort — re-queues per-entry failures
          * for the next reconnect cycle. */
         voice_messages_sync_drain();
         /* Don't set READY yet — wait for Dragon's session_start reply so
          * we know the backend pipeline is fully up. Keep state CONNECTING
          * until session_start arrives (~6s model load on first boot). */
         break;
      }

      case WEBSOCKET_EVENT_DISCONNECTED:
         ESP_LOGW(TAG, "WS: DISCONNECTED");
         { tab5_debug_obs_event("ws.disconnect", ""); }
         /* Flush playback so we don't keep speaking into a dead pipe. */
         voice_playback_buf_reset();
         tab5_audio_speaker_enable(false);
         /* PR 2 polish: WS drop during dictation pipeline.
          *
          * RECORDING / UPLOADING — the audio frames we were streaming
          * went into a dead pipe, no recovery is possible.  Fail
          * immediately so the user can retry.
          *
          * TRANSCRIBING — Dragon's blocking LLM summary call may have
          * starved the WS for >60 s and triggered an ngrok idle-close.
          * The WS auto-reconnects within ~600 ms (esp_websocket_client
          * built-in).  Don't flip the pipeline to FAILED right away —
          * instead arm a 45 s grace timer.  If `dictation_summary` or
          * `dictation_postprocessing_error` arrives on the reconnected
          * WS during that window, the pipeline reaches SAVED naturally
          * and the timer's pipeline-state check will skip the FAIL.
          * If the window expires with no resolution, then FAIL. */
         {
            dict_state_t cur_dict = voice_dictation_get().state;
            if (cur_dict == DICT_RECORDING || cur_dict == DICT_UPLOADING) {
               voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, (uint32_t)(esp_timer_get_time() / 1000));
            } else if (cur_dict == DICT_TRANSCRIBING) {
               extern void voice_ws_arm_transcribe_grace_timer(uint32_t timeout_ms);
               voice_ws_arm_transcribe_grace_timer(45000);
            }
         }
         /* Tab5 audit F5 (2026-04-20): if the drop landed MID-TURN (voice
          * state was PROCESSING or SPEAKING), surface a toast so the user
          * knows why the reply stopped.  Previously a mid-turn Dragon
          * crash was silent — just a frozen overlay + no answer.  The
          * home status-bar pill already picks up RECONNECTING via
          * voice_get_degraded_reason(), so the toast is a short-lived
          * orient-the-user signal, not the permanent indicator.
          *
          * Wave 7 F5 completion (2026-04-21): also pulse the halo orb
          * rose for ~2.5 s so the visual signal matches the audit spec
          * ("toast + rose orb pulse"). Toast alone was easy to miss if
          * the user was looking at the chat area rather than the home
          * card. ui_home_pulse_orb_alert reverts to the mode-default
          * orb paint via an LVGL one-shot timer. */
         {
            voice_state_t cur = voice_get_state();
            if ((cur == VOICE_STATE_PROCESSING || cur == VOICE_STATE_SPEAKING) && !s_disconnecting) {
               tab5_lv_async_call((lv_async_cb_t)ui_home_show_toast, (void *)"Dragon dropped mid-turn - reconnecting");
               tab5_lv_async_call((lv_async_cb_t)ui_home_pulse_orb_alert, NULL);
            }
         }
         /* T1.1: bump attempt counter + apply exponential-with-full-jitter
          * backoff to the client's reconnect timer.  Prevents thundering
          * herd if multiple Tab5s share the same Dragon + avoids 2 s
          * hammering against a server that's 20 s into its restart. */
         s_connect_attempt++;
         {
            int shift = s_connect_attempt - 1;
            if (shift < 0) shift = 0;
            if (shift > 5) shift = 5;
            uint32_t exp_ms = WS_CLIENT_BACKOFF_MIN_MS << shift;
            if (exp_ms > WS_CLIENT_BACKOFF_CAP_MS) exp_ms = WS_CLIENT_BACKOFF_CAP_MS;
            uint32_t half = exp_ms / 2;
            uint32_t jitter = half > 0 ? (esp_random() % half) : 0;
            uint32_t delay_ms = half + jitter; /* [exp/2, exp) */
            ESP_LOGI(TAG, "WS: reconnect attempt=%d backoff=%lums (exp=%lums)", s_connect_attempt,
                     (unsigned long)delay_ms, (unsigned long)exp_ms);
            esp_websocket_client_set_reconnect_timeout(g_voice_ws, delay_ms);
         }
/* #146: WS-level escalation.  If Wi-Fi probes claim we're fine
 * (probe task still sees lan=1/ngrok=1) but the voice WS keeps
 * failing to connect, the Wi-Fi chip's TCP-stack state is bad.
 * The probe is a one-shot SYN; the WS handshake needs more
 * buffers + TLS-path + full RX pipeline.  After WS_KICK_THRESHOLD
 * consecutive failed attempts (~2 min of retry), hard-kick the
 * stack.  Don't do this on attempt 1-4 — those are normal
 * transient failures (Dragon restart, brief AP hiccup). */
#define WS_KICK_THRESHOLD 5
         if (s_connect_attempt == WS_KICK_THRESHOLD) {
            ESP_LOGW(TAG,
                     "WS: %d failed attempts — escalating to hard-kick "
                     "(stack may be wedged despite probe success)",
                     s_connect_attempt);
            esp_err_t kr = tab5_wifi_hard_kick();
            if (kr != ESP_OK) {
               /* ESP-Hosted's Wi-Fi slave chip doesn't always recover
                * from stop/start without a host-side reboot (observed:
                * esp_wifi_start returns ESP_FAIL repeatedly).  Don't
                * burn another 5 attempts — reboot now. */
               ESP_LOGE(TAG, "WS: hard-kick failed (%s) — controlled reboot", esp_err_to_name(kr));
               vTaskDelay(pdMS_TO_TICKS(100));
               esp_restart();
               /* unreachable */
            }
            /* Reset the counter so we give it another 5 tries after the
             * successful hard kick before escalating again.  If 10
             * failures in a row, the link-probe zombie path catches it. */
            s_connect_attempt = 0;
         }
         /* T1.2: RECONNECTING while a backoff is queued + WiFi is up.
          * IDLE only when WiFi is genuinely down or user stopped voice. */
         {
            bool wifi_up = tab5_wifi_connected();
            if (s_initialized && !s_disconnecting && wifi_up) {
               voice_set_state(VOICE_STATE_RECONNECTING, "backoff");
            } else {
               voice_set_state(VOICE_STATE_IDLE, "disconnected");
            }
         }
         /* v4·D audit P0 fix: ngrok fallback was one-way.  Clear the flag
          * + reset the fail counter so the NEXT successful connection
          * gets a chance to land on LAN.  The actual URI swap is deferred
          * to voice_try_lan_probe() which runs off the WS event task. */
         if (s_using_ngrok && tab5_settings_get_connection_mode() == 0) {
            s_handshake_fail_cnt = 0;
            /* Leave s_using_ngrok alone here: we only reset it once the
             * external LAN probe succeeds (heap_watchdog periodic hook).
             * Clearing it here without swapping the client URI would be
             * a lie, and swapping the URI inside the event task has
             * shown to wedge the client.  Next sprint: add a proper
             * probe task that pings LAN and schedules the swap via
             * lv_async_call on success. */
         }
         break;

      case WEBSOCKET_EVENT_CLOSED:
         /* closes #110: was voice_set_state(IDLE, "closed") with NO
          * reconnect scheduled — if Dragon sends a graceful WS close
          * frame (restart, idle timeout, etc.) Tab5 parked in IDLE
          * forever until the user hit /voice/reconnect manually.
          * DISCONNECTED re-armed the reconnect timer; CLOSED did not.
          *
          * Mirror the DISCONNECTED path: bump attempt, apply
          * exponential-with-full-jitter backoff, transition to
          * RECONNECTING when Wi-Fi is up so the state bar reflects
          * that Tab5 is actively trying, not dead.  The managed WS
          * client's auto-reconnect picks up the new timeout. */
         ESP_LOGI(TAG, "WS: CLOSED — scheduling reconnect");
         if (g_voice_ws && !s_disconnecting) {
            s_connect_attempt++;
            int shift = s_connect_attempt - 1;
            if (shift < 0) shift = 0;
            if (shift > 5) shift = 5;
            uint32_t exp_ms = WS_CLIENT_BACKOFF_MIN_MS << shift;
            if (exp_ms > WS_CLIENT_BACKOFF_CAP_MS) exp_ms = WS_CLIENT_BACKOFF_CAP_MS;
            uint32_t half = exp_ms / 2;
            uint32_t jitter = half > 0 ? (esp_random() % half) : 0;
            uint32_t delay_ms = half + jitter;
            ESP_LOGI(TAG, "WS: CLOSED reconnect attempt=%d backoff=%lums", s_connect_attempt, (unsigned long)delay_ms);
            esp_websocket_client_set_reconnect_timeout(g_voice_ws, delay_ms);
            if (tab5_wifi_connected()) {
               voice_set_state(VOICE_STATE_RECONNECTING, "closed");
            } else {
               voice_set_state(VOICE_STATE_IDLE, "closed-nowifi");
            }
         } else {
            voice_set_state(VOICE_STATE_IDLE, "closed");
         }
         break;

      case WEBSOCKET_EVENT_DATA:
         if (!data) break;
         s_last_activity_us = esp_timer_get_time();
         if (data->op_code == WS_TRANSPORT_OPCODES_TEXT && data->data_len > 0) {
            ESP_LOGI(TAG, "WS recv text (%d bytes): %.*s", data->data_len, data->data_len > 200 ? 200 : data->data_len,
                     data->data_ptr);
            voice_ws_proto_handle_text(data->data_ptr, data->data_len);
         } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY && data->data_len > 0) {
            /* #268: large binary frames (e.g. Phase 3B video JPEGs >32 KB)
             * arrive in fragments — esp_websocket_client splits them at
             * WS_CLIENT_BUFFER_SIZE.  Reassemble using payload_len +
             * payload_offset before dispatching to handle_binary_message
             * so the magic-byte sniff sees the whole frame.  Audio
             * (PCM/OPUS) is single-fragment so the fast path skips
             * reassembly. */
            const int frag_total = data->payload_len;
            const int frag_off = data->payload_offset;
            const int frag_len = data->data_len;
            if (frag_total <= 0 || frag_total == frag_len) {
               /* Single-fragment frame — current behavior. */
               voice_ws_proto_handle_binary(data->data_ptr, frag_len);
            } else {
               /* Multi-fragment frame.  Lazy-init reassembly buffer
                * in PSRAM, sized to the same ceiling voice_video uses. */
               static uint8_t *s_rx_reasm_buf = NULL;
               static int s_rx_reasm_cap = 96 * 1024;
               if (!s_rx_reasm_buf) {
                  s_rx_reasm_buf = heap_caps_malloc(s_rx_reasm_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
               }
               if (!s_rx_reasm_buf) {
                  ESP_LOGW(TAG, "rx-reasm OOM (%d B); dropping frag", s_rx_reasm_cap);
                  break;
               }
               if (frag_total > s_rx_reasm_cap) {
                  ESP_LOGW(TAG, "rx-reasm: payload %d > cap %d; dropping", frag_total, s_rx_reasm_cap);
                  break;
               }
               if (frag_off + frag_len > frag_total) {
                  ESP_LOGW(TAG, "rx-reasm: bad frag off=%d len=%d total=%d", frag_off, frag_len, frag_total);
                  break;
               }
               memcpy(s_rx_reasm_buf + frag_off, data->data_ptr, frag_len);
               if (frag_off + frag_len == frag_total) {
                  voice_ws_proto_handle_binary((const char *)s_rx_reasm_buf, frag_total);
               }
            }
         } else if (data->op_code == WS_TRANSPORT_OPCODES_PING) {
            ESP_LOGD(TAG, "WS recv PING — client auto-pongs");
         } else if (data->op_code == WS_TRANSPORT_OPCODES_PONG) {
            ESP_LOGD(TAG, "WS recv PONG");
         } else if (data->op_code == WS_TRANSPORT_OPCODES_CLOSE) {
            ESP_LOGI(TAG, "WS recv CLOSE frame");
         }
         break;

      case WEBSOCKET_EVENT_ERROR: {
         int status = data ? data->error_handle.esp_ws_handshake_status_code : 0;
         esp_websocket_error_type_t et = data ? data->error_handle.error_type : WEBSOCKET_ERROR_TYPE_NONE;
         ESP_LOGW(TAG, "WS: ERROR (type=%d, handshake_status=%d, sock_errno=%d)", (int)et, status,
                  data ? data->error_handle.esp_transport_sock_errno : 0);
         /* T1.1: apply the same backoff on error as on disconnect so
          * a handshake-fail loop doesn't pin at 2 s forever. */
         {
            int shift = s_connect_attempt;
            if (shift < 0) shift = 0;
            if (shift > 5) shift = 5;
            uint32_t exp_ms = WS_CLIENT_BACKOFF_MIN_MS << shift;
            if (exp_ms > WS_CLIENT_BACKOFF_CAP_MS) exp_ms = WS_CLIENT_BACKOFF_CAP_MS;
            uint32_t half = exp_ms / 2;
            uint32_t jitter = half > 0 ? (esp_random() % half) : 0;
            esp_websocket_client_set_reconnect_timeout(g_voice_ws, half + jitter);
         }

         /* γ3-Tab5 (issue #198): 401 specifically means auth failed —
          * burning the auth-fail counter every retry will pin the
          * device against ngrok forever (wrong-token devices used to
          * loop 401s on both LAN and ngrok endpoints, eating battery
          * + ngrok bandwidth + log storage).  After WS_AUTH_FAIL_THRESHOLD
          * consecutive 401s, stop the WS client + show a toast pointing
          * at Settings.  s_auth_fail_cnt resets on a successful CONNECT
          * so a transient 401 (token-rotation race during Dragon
          * restart) doesn't get sticky after recovery.
          *
          * Filter on status alone — NOT on
          * `et == WEBSOCKET_ERROR_TYPE_HANDSHAKE`.  Empirically (live
          * test on real device against PROD :3502 with rotated token)
          * a clean 401 from Dragon arrives as `et=1`
          * (WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) because the underlying
          * TCP transport completed successfully — the 401 happens at
          * the application layer.  Gating on HANDSHAKE here meant the
          * counter never incremented and the loop ran forever. */
         if (status == 401) {
            s_auth_fail_cnt++;
            ESP_LOGW(TAG, "WS: 401 auth_failed (count=%d/%d)", s_auth_fail_cnt, WS_AUTH_FAIL_THRESHOLD);
            if (s_auth_fail_cnt >= WS_AUTH_FAIL_THRESHOLD && !s_disconnecting) {
               ESP_LOGE(TAG, "WS: %d consecutive 401s — stopping retry loop", s_auth_fail_cnt);
               s_disconnecting = true;
               /* Hop to worker task — esp_websocket_client_stop()
                * rejects calls from the WS task itself.  Same pattern
                * as the γ2-H8 device_evicted handler. */
               tab5_worker_enqueue(_voice_auth_fail_stop_worker_fn, NULL, "auth_failed_stop");
               /* TT #328 Wave 3 P0 #15 — pre-Wave-3 this was a 2.2 s toast.
                * After 5 silent retries the user got a single transient
                * notice they could easily miss.  Upgraded to a persistent
                * error banner that stays until they tap it (and presumably
                * jump to Settings to fix the token).  Also fires the
                * error.auth obs event so /events surfaces the lockout. */
               tab5_debug_obs_event("error.auth", "ws_401_stop");
               char *toast = strdup("Invalid Dragon token — check Settings");
               if (toast) {
                  voice_async_toast(toast);
               }
               voice_async_error_banner(
                   "Dragon rejected your token (401). Reconnect paused — "
                   "open Settings → Network to update.");
               break;
            }
         }

         /* Handshake failure → try ngrok fallback (only in conn_mode=auto,
          * only while still on local URI, after NGROK_FALLBACK_THRESHOLD fails). */
         if (et == WEBSOCKET_ERROR_TYPE_HANDSHAKE && status != 101) {
            s_handshake_fail_cnt++;
            uint8_t conn_mode = tab5_settings_get_connection_mode();
            if (conn_mode == 0 && !s_using_ngrok && s_handshake_fail_cnt >= NGROK_FALLBACK_THRESHOLD) {
               char ngrok_uri[128];
               snprintf(ngrok_uri, sizeof(ngrok_uri), "wss://%s:%d%s", TAB5_NGROK_HOST, TAB5_NGROK_PORT,
                        TAB5_VOICE_WS_PATH);
               ESP_LOGW(TAG, "WS: handshake failed %d× — falling back to %s", s_handshake_fail_cnt, ngrok_uri);
               s_using_ngrok = true;
               s_handshake_fail_cnt = 0;
               /* set_uri requires a stopped client. The client is
                * currently between reconnect attempts; stop+set+start
                * to force it onto the new URI immediately. */
               esp_websocket_client_stop(g_voice_ws);
               esp_websocket_client_set_uri(g_voice_ws, ngrok_uri);
               strncpy(s_dragon_host, TAB5_NGROK_HOST, sizeof(s_dragon_host) - 1);
               s_dragon_host[sizeof(s_dragon_host) - 1] = '\0';
               s_dragon_port = TAB5_NGROK_PORT;
               esp_websocket_client_start(g_voice_ws);
            }
         }
         break;
      }

      default:
         break;
   }
}

// ---------------------------------------------------------------------------
// URI helpers
// ---------------------------------------------------------------------------
/* TT #331 Wave 23 SRP-A1: was static void voice_build_local_uri. */
void voice_ws_proto_build_local_uri(char *out, size_t out_cap, const char *host, uint16_t port) {
   snprintf(out, out_cap, "ws://%s:%u%s", host, (unsigned)port, TAB5_VOICE_WS_PATH);
}
