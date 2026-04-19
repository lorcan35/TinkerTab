/**
 * TinkerClaw Tab5 — Voice Streaming Module
 *
 * Streams mic audio (PCM 16-bit 16kHz mono) to Dragon voice server via
 * WebSocket, receives TTS audio and plays it through the speaker.
 *
 * Push-to-talk flow:
 *   1. voice_start_listening() -> sends {"type":"start"} -> starts mic capture
 *   2. voice_stop_listening()  -> stops mic capture -> sends {"type":"stop"}
 *   3. Dragon processes audio  -> sends STT text, then TTS audio
 *   4. Tab5 receives and plays TTS audio
 *   5. {"type":"tts_end"} received -> back to READY state
 *
 * Uses the Espressif `esp_websocket_client` managed component (issue #76).
 * The event-driven model eliminates the register-before-receive-task race
 * that caused the Tab5 voice WS to flap every 1–2 seconds: the `register`
 * frame is sent inside the WEBSOCKET_EVENT_CONNECTED callback, which fires
 * only after the client's event task is already running and ready to
 * process Dragon's `session_start` reply on the return path.
 *
 * Mic capture task is pinned to core 1 to keep core 0 free for UI/LVGL.
 */

#include "voice.h"
#include "afe.h"
#include "widget.h"
#include "ui_voice.h"
#include "ui_notes.h"
#include "ui_chat.h"
#include "audio.h"
#include "config.h"
#include "driver/i2s_common.h"  /* US-C13: i2s_channel_disable/enable for safe mic task shutdown */
#include "settings.h"
#include "ui_core.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

static const char *TAG = "tab5_voice";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Output: 20ms of 16kHz mono = 320 samples = 640 bytes (sent to Dragon)
#define VOICE_CHUNK_SAMPLES    (TAB5_VOICE_SAMPLE_RATE * TAB5_VOICE_CHUNK_MS / 1000)
#define VOICE_CHUNK_BYTES      (VOICE_CHUNK_SAMPLES * sizeof(int16_t))

// Input: 20ms of 48kHz, 4 TDM slots = 960 frames × 4 ch = 3840 samples
// TDM layout (M5Stack BSP): [MIC-L(0), AEC(1), MIC-R(2), MIC-HP(3)]
#define MIC_48K_FRAMES         (TAB5_AUDIO_SAMPLE_RATE * TAB5_VOICE_CHUNK_MS / 1000)
#define MIC_TDM_CHANNELS       4
// TDM slot offsets — verified at runtime via SLOT_DIAG logs.
// TDM slot mapping — verified from SLOT_DIAG: slot 1 is near-silent (AEC ref from DAC)
// Slots 0 and 2 have similar RMS (both are mics). Slot 3 has low non-zero (noise/unused).
#define MIC_TDM_MIC1_OFF       0    // MIC1 at slot 0 (high RMS ~200)
#define MIC_TDM_REF_OFF        1    // AEC reference at slot 1 (near-zero RMS ~6 when speaker silent)
#define MIC_TDM_MIC2_OFF       2    // MIC2 at slot 2 (high RMS ~200)

// AFE feed: channels match the format string passed to afe_config_init().
#define AFE_FEED_CHANNELS      2  /* "MR" mode — 1 mic + 1 ref */
#define AFE_FEED_MAX_SAMPLES   2048
#define MIC_TDM_SAMPLES        (MIC_48K_FRAMES * MIC_TDM_CHANNELS)
// Downsample ratio: 48kHz -> 16kHz = 3:1
#define DOWNSAMPLE_RATIO       (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)
// Upsample ratio: 16kHz -> 48kHz = 1:3
#define UPSAMPLE_RATIO         (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)

// esp_websocket_client tuning
#define WS_CLIENT_TASK_STACK   10240   /* client event/receive task stack */
#define WS_CLIENT_BUFFER_SIZE  4096    /* per-frame buffer; binary TTS comes in 640–1024 B chunks, headroom for text+media */
#define WS_CLIENT_RECONNECT_MS 2000    /* base reconnect delay — matches legacy RECONNECT_BACKOFF_MIN_MS */
#define WS_CLIENT_NETWORK_MS   10000   /* blocking send deadline */
#define WS_CLIENT_PING_SEC     15      /* built-in WS-level ping */
#define WS_CLIENT_PONG_SEC     45      /* disconnect if no pong in 3× ping */

// Ngrok fallback is attempted after this many consecutive failed handshakes
// against the local LAN URI (only in conn_mode=0 "auto").
#define NGROK_FALLBACK_THRESHOLD 2

// Mic capture task — needs room for AFE feed buffer (960 int16 = 1.9KB) + TDM diagnostics
#define MIC_TASK_STACK_SIZE    8192
#define MIC_TASK_PRIORITY      5
#define MIC_TASK_CORE          1

// Playback drain task — higher priority than WS receive so I2S stays fed
#define PLAY_TASK_STACK_SIZE   4096
#define PLAY_TASK_PRIORITY     6
#define PLAY_TASK_CORE         1

// Max transcript length
#define MAX_TRANSCRIPT_LEN     2048

// Dictation mode constants
#define DICTATION_SILENCE_THRESHOLD  800
#define DICTATION_SILENCE_FRAMES     25
#define DICTATION_AUTO_STOP_FRAMES   250
#define DICTATION_WARN_3S_FRAMES     150
#define DICTATION_WARN_4S_FRAMES     200
#define DICTATION_TEXT_SIZE          65536
#define MAX_RECORD_FRAMES_ASK        1500

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static voice_state_t     s_state = VOICE_STATE_IDLE;
static voice_state_cb_t  s_state_cb = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;

// US-C02: Session generation counter — monotonically increasing.
// Every lv_async_call from voice tasks captures this value; the callback
// checks it against the current generation and silently drops stale calls.
static volatile uint32_t s_session_gen = 0;

// WebSocket client (esp_websocket_client managed component)
static esp_websocket_client_handle_t s_ws = NULL;

// Dragon connection info (last-known, updated on each connect)
static char     s_dragon_host[64] = {0};
static uint16_t s_dragon_port     = TAB5_VOICE_PORT;

// Connection tracking
static volatile bool s_initialized        = false;
static volatile bool s_started            = false;  /* esp_websocket_client_start() has been called */
static volatile bool s_disconnecting      = false;  /* US-C21: guard against connect-during-disconnect race */
static volatile int  s_handshake_fail_cnt = 0;      /* consecutive handshake failures — trigger ngrok fallback in auto mode */
static volatile bool s_using_ngrok        = false;  /* true once we've switched to the ngrok URI */

// Mic capture task
static TaskHandle_t  s_mic_task    = NULL;
static volatile bool s_mic_running = false;

// Playback drain task — pulls from ring buffer, blocks on i2s_channel_write
static TaskHandle_t      s_play_task    = NULL;
static volatile bool     s_play_running = false;
static volatile bool     s_tts_done     = false;  // set by tts_end, drain task transitions to READY
static SemaphoreHandle_t s_play_sem     = NULL;   // signalled when data is written to ring buf

// Playback ring buffer — allocated in PSRAM
static int16_t          *s_play_buf      = NULL;
static size_t            s_play_capacity = 0;
static size_t            s_play_wr       = 0;
static size_t            s_play_rd       = 0;
static size_t            s_play_count    = 0;
static SemaphoreHandle_t s_play_mutex    = NULL;
#define PLAY_BUF_CAPACITY  s_play_capacity

// Last transcript from Dragon STT
static char s_transcript[MAX_TRANSCRIPT_LEN] = {0};
static char s_stt_text[MAX_TRANSCRIPT_LEN]   = {0};
static char s_llm_text[MAX_TRANSCRIPT_LEN]   = {0};

/* v4·D Phase 3b: cached per-turn receipt from Dragon. */
static int  s_last_receipt_mils       = 0;
static int  s_last_receipt_prompt_tok = 0;
static int  s_last_receipt_compl_tok  = 0;
static char s_last_receipt_model[64]  = {0};

// Dictation mode
static voice_mode_t   s_voice_mode        = VOICE_MODE_ASK;
static char          *s_dictation_text    = NULL;  /* PSRAM-allocated, DICTATION_TEXT_SIZE */
static volatile float s_current_rms       = 0.0f;
static char           s_dictation_title[128]   = {0};
static char           s_dictation_summary[512] = {0};

// AFE (always-listening) mode — PARKED, but `s_afe_enabled` is referenced
// by mic_capture_task (always compiled) via its non-parked code path, so
// it must exist even when WAKE_WORD_PARKED is defined. The detect-task
// handle and its live flag are only used inside the parked impl.
static volatile bool s_afe_enabled      = false;

// Drop counter for audio frames lost under back-pressure (US-C04)
static int s_audio_drop_count = 0;

// Activity timestamp shared with stop_listening for response timeout
static volatile int64_t s_last_activity_us = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void voice_set_state(voice_state_t new_state, const char *detail);
static void mic_capture_task(void *arg);
static void playback_drain_task(void *arg);
static void handle_text_message(const char *data, int len);
static void playback_buf_reset(void);
static size_t playback_buf_write(const int16_t *data, size_t samples);
static size_t playback_buf_read(int16_t *data, size_t max_samples);
static esp_err_t voice_ws_send_text(const char *msg);
static esp_err_t voice_ws_send_register(void);
static void voice_ws_event_handler(void *arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data);
static void voice_build_local_uri(char *out, size_t out_cap,
                                   const char *host, uint16_t port);

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
static void voice_set_state(voice_state_t new_state, const char *detail)
{
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }
    voice_state_t old = s_state;
    s_state = new_state;
    if (s_state_mutex) {
        xSemaphoreGive(s_state_mutex);
    }

    if (old != new_state || (detail != NULL && detail[0] != '\0')) {
        ESP_LOGI(TAG, "State: %d -> %d (%s)", old, new_state,
                 detail ? detail : "");
        if (s_state_cb) {
            if (tab5_ui_try_lock(200)) {
                s_state_cb(new_state, detail);
                tab5_ui_unlock();
            } else {
                ESP_LOGW(TAG, "State %d->%d: LVGL lock busy, UI callback skipped",
                         old, new_state);
            }
        }
        /* Always poke the home screen — ui_home_refresh_sys_label() uses
         * lv_async_call(), lock-free and thread-safe. */
        extern void ui_home_refresh_sys_label(void);
        ui_home_refresh_sys_label();
    }
}

// ---------------------------------------------------------------------------
// Playback ring buffer
// ---------------------------------------------------------------------------
static void playback_buf_reset(void)
{
    xSemaphoreTake(s_play_mutex, portMAX_DELAY);
    s_play_wr = 0;
    s_play_rd = 0;
    s_play_count = 0;
    xSemaphoreGive(s_play_mutex);
}

static size_t playback_buf_write(const int16_t *data, size_t samples)
{
    xSemaphoreTake(s_play_mutex, portMAX_DELAY);

    size_t written = 0;
    while (written < samples && s_play_count < PLAY_BUF_CAPACITY) {
        s_play_buf[s_play_wr] = data[written];
        s_play_wr = (s_play_wr + 1) % PLAY_BUF_CAPACITY;
        s_play_count++;
        written++;
    }

    xSemaphoreGive(s_play_mutex);

    if (written < samples) {
        ESP_LOGW(TAG, "Playback buffer overflow, dropped %zu samples",
                 samples - written);
    }

    if (written > 0 && s_play_sem) {
        xSemaphoreGive(s_play_sem);
    }
    return written;
}

static size_t playback_buf_read(int16_t *data, size_t max_samples)
{
    xSemaphoreTake(s_play_mutex, portMAX_DELAY);

    size_t read = 0;
    while (read < max_samples && s_play_count > 0) {
        data[read] = s_play_buf[s_play_rd];
        s_play_rd = (s_play_rd + 1) % PLAY_BUF_CAPACITY;
        s_play_count--;
        read++;
    }

    xSemaphoreGive(s_play_mutex);
    return read;
}

void voice_reset_activity_timestamp(void)
{
    s_last_activity_us = esp_timer_get_time();
}

// ---------------------------------------------------------------------------
// WebSocket send helpers (wrapping esp_websocket_client_send_*)
// ---------------------------------------------------------------------------
static esp_err_t voice_ws_send_text(const char *msg)
{
    if (!s_ws) return ESP_ERR_INVALID_STATE;
    if (!esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;

    size_t len = strlen(msg);
    int w = esp_websocket_client_send_text(s_ws, msg, (int)len, pdMS_TO_TICKS(1000));
    if (w < 0) {
        ESP_LOGW(TAG, "WS text send failed (%zu bytes)", len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t voice_ws_send_binary(const void *data, size_t len)
{
    if (!s_ws) return ESP_ERR_INVALID_STATE;
    if (!esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;

    /* Short 100 ms timeout — we drop frames under pressure rather than
     * block the mic task. If WiFi stalls, I2S DMA would overflow. */
    int w = esp_websocket_client_send_bin(s_ws, (const char *)data, (int)len,
                                          pdMS_TO_TICKS(100));
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

// ---------------------------------------------------------------------------
// Device registration — sent as FIRST text frame from the CONNECTED handler.
// This is the #76 fix: register is sent from inside the event handler, AFTER
// the client's event task is running and ready to dispatch the server's
// session_start reply. The legacy code sent register synchronously before
// spawning the receive task, which opened a 50–500ms window where Dragon's
// reply arrived while nothing was draining the SDIO RX buffer.
// ---------------------------------------------------------------------------
static esp_err_t voice_ws_send_register(void)
{
    char device_id[16]   = {0};
    char hardware_id[20] = {0};
    char session_id[64]  = {0};

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

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Sending register: device=%s hw=%s session=%s",
             device_id, hardware_id, session_id[0] ? session_id : "(new)");

    esp_err_t err = voice_ws_send_text(json);
    cJSON_free(json);
    return err;
}

// ---------------------------------------------------------------------------
// US-C02: Generation-guarded async call wrappers.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t gen;
    char    *text;
} voice_async_toast_t;

typedef struct {
    uint32_t gen;
} voice_async_badge_t;

static void async_show_toast_cb(void *arg)
{
    voice_async_toast_t *t = (voice_async_toast_t *)arg;
    if (!t) return;
    if (t->gen == s_session_gen && t->text) {
        extern void ui_home_show_toast(const char *text);
        ui_home_show_toast(t->text);
    } else if (t->text) {
        ESP_LOGD(TAG, "Stale async toast dropped (gen %lu vs %lu)",
                 (unsigned long)t->gen, (unsigned long)s_session_gen);
    }
    free(t->text);
    free(t);
}

static void async_refresh_badge_cb(void *arg)
{
    voice_async_badge_t *b = (voice_async_badge_t *)arg;
    if (!b) return;
    if (b->gen == s_session_gen) {
        extern void ui_home_refresh_mode_badge(void);
        ui_home_refresh_mode_badge();
    } else {
        ESP_LOGD(TAG, "Stale async badge refresh dropped (gen %lu vs %lu)",
                 (unsigned long)b->gen, (unsigned long)s_session_gen);
    }
    free(b);
}

static void voice_async_toast(char *text)
{
    voice_async_toast_t *t = malloc(sizeof(voice_async_toast_t));
    if (!t) { free(text); return; }
    t->gen = s_session_gen;
    t->text = text;
    lv_async_call(async_show_toast_cb, t);
}

static void voice_async_refresh_badge(void)
{
    voice_async_badge_t *b = malloc(sizeof(voice_async_badge_t));
    if (!b) return;
    b->gen = s_session_gen;
    lv_async_call(async_refresh_badge_cb, b);
}

// ---------------------------------------------------------------------------
// JSON message handling (Dragon -> Tab5)
// ---------------------------------------------------------------------------
static void handle_text_message(const char *data, int len)
{
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
        if (cJSON_IsString(text) && text->valuestring && s_voice_mode == VOICE_MODE_DICTATE
            && s_dictation_text) {
            size_t cur_len = strlen(s_dictation_text);
            size_t add_len = strlen(text->valuestring);
            if (cur_len + add_len + 2 < DICTATION_TEXT_SIZE) {
                if (cur_len > 0) strcat(s_dictation_text, " ");
                strcat(s_dictation_text, text->valuestring);
            }
            ESP_LOGI(TAG, "STT partial: \"%s\" (total %u chars)",
                     text->valuestring, (unsigned)strlen(s_dictation_text));
            voice_set_state(VOICE_STATE_LISTENING, s_dictation_text);
        }
    } else if (strcmp(type_str, "stt") == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            if (s_voice_mode == VOICE_MODE_DICTATE) {
                if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                strncpy(s_stt_text, text->valuestring, MAX_TRANSCRIPT_LEN - 1);
                s_stt_text[MAX_TRANSCRIPT_LEN - 1] = '\0';
                if (s_state_mutex) xSemaphoreGive(s_state_mutex);
                ESP_LOGI(TAG, "Dictation complete: %u chars", (unsigned)strlen(text->valuestring));
                voice_set_state(VOICE_STATE_READY, "dictation_done");
            } else {
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
        ESP_LOGI(TAG, "TTS start — preparing playback");
        tab5_audio_speaker_enable(true);
        playback_buf_reset();
        voice_set_state(VOICE_STATE_SPEAKING, NULL);
    } else if (strcmp(type_str, "tts_end") == 0) {
        ESP_LOGI(TAG, "TTS end — drain task will finish playback");
        s_tts_done = true;
        if (s_play_sem) xSemaphoreGive(s_play_sem);
    } else if (strcmp(type_str, "llm") == 0) {
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
    } else if (strcmp(type_str, "error") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        const char *err_src = cJSON_IsString(msg) ? msg->valuestring : "unknown";
        ESP_LOGE(TAG, "Dragon error: %s", err_src);
        char err_buf[128];
        strncpy(err_buf, err_src, sizeof(err_buf) - 1);
        err_buf[sizeof(err_buf) - 1] = '\0';
        playback_buf_reset();
        tab5_audio_speaker_enable(false);
        bool connected = (s_ws != NULL) && esp_websocket_client_is_connected(s_ws);
        voice_set_state(connected ? VOICE_STATE_READY : VOICE_STATE_IDLE, err_buf);
    } else if (strcmp(type_str, "session_start") == 0) {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid) && sid->valuestring) {
            tab5_settings_set_session_id(sid->valuestring);
            ESP_LOGI(TAG, "Session: %s", sid->valuestring);
        }
        cJSON *resumed = cJSON_GetObjectItem(root, "resumed");
        cJSON *msg_cnt = cJSON_GetObjectItem(root, "message_count");
        ESP_LOGI(TAG, "session_start: resumed=%s messages=%d",
                 (cJSON_IsTrue(resumed) ? "yes" : "no"),
                 cJSON_IsNumber(msg_cnt) ? msg_cnt->valueint : 0);

        if (s_state == VOICE_STATE_CONNECTING) {
            voice_set_state(VOICE_STATE_READY, NULL);
            ESP_LOGI(TAG, "Connected to Dragon voice server");

            uint8_t saved_mode = tab5_settings_get_voice_mode();
            char saved_model[64] = {0};
            tab5_settings_get_llm_model(saved_model, sizeof(saved_model));
            ESP_LOGI(TAG, "Restoring voice_mode=%d model='%s' on reconnect",
                     saved_mode, saved_model[0] ? saved_model : "(default)");
            voice_send_config_update((int)saved_mode,
                                     saved_model[0] ? saved_model : NULL);

            ui_notes_sync_pending();
        }
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
    } else if (strcmp(type_str, "note_created") == 0) {
        cJSON *nid = cJSON_GetObjectItem(root, "note_id");
        cJSON *ntitle = cJSON_GetObjectItem(root, "title");
        ESP_LOGI(TAG, "Dragon auto-created note: id=%s title=\"%s\"",
                 cJSON_IsString(nid) ? nid->valuestring : "?",
                 cJSON_IsString(ntitle) ? ntitle->valuestring : "?");
    } else if (strcmp(type_str, "llm_done") == 0) {
        cJSON *ms = cJSON_GetObjectItem(root, "llm_ms");
        ESP_LOGI(TAG, "LLM done (%.0fms)", cJSON_IsNumber(ms) ? ms->valuedouble : 0.0);
        if (s_llm_text[0]) {
            ui_chat_push_message("assistant", s_llm_text);
        }
    } else if (strcmp(type_str, "receipt") == 0) {
        /* v4·D Phase 3b: Dragon emits a receipt after each LLM turn on
         * the cloud path.  Logged here for now; rendering the stamp
         * beneath the chat bubble is a follow-up that lives in
         * chat_msg_view.c when the store grows a receipt field.  Cost
         * is in mils (1/1000 USD cent) so divide by 100000 for dollars
         * or 1000 for cents. */
        const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(root, "model"));
        cJSON *ptok = cJSON_GetObjectItem(root, "prompt_tokens");
        cJSON *ctok = cJSON_GetObjectItem(root, "completion_tokens");
        cJSON *mils = cJSON_GetObjectItem(root, "cost_mils");
        int pt = cJSON_IsNumber(ptok) ? (int)ptok->valuedouble : 0;
        int ct = cJSON_IsNumber(ctok) ? (int)ctok->valuedouble : 0;
        int m  = cJSON_IsNumber(mils) ? (int)mils->valuedouble : 0;
        ESP_LOGI(TAG, "Receipt: model=%s tok=%d+%d cost=%d mils ($%d.%05d)",
                 model ? model : "?", pt, ct, m, m / 100000, m % 100000);
        /* Cache for UI consumption. Accessed via voice_get_last_receipt(). */
        s_last_receipt_mils       = m;
        s_last_receipt_prompt_tok = pt;
        s_last_receipt_compl_tok  = ct;
        snprintf(s_last_receipt_model, sizeof(s_last_receipt_model),
                 "%s", model ? model : "");
    } else if (strcmp(type_str, "text_update") == 0) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        if (text) {
            ESP_LOGI(TAG, "Text update: replacing last AI message");
            ui_chat_update_last_message(text);
        }
    } else if (strcmp(type_str, "pong") == 0) {
        /* Dragon-level JSON pong — logged only. WS-level ping/pong is
         * handled automatically by esp_websocket_client (pingpong_timeout_sec). */
        ESP_LOGD(TAG, "App-level pong");
    } else if (strcmp(type_str, "config_update") == 0) {
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsString(error) && error->valuestring[0]) {
            ESP_LOGW(TAG, "Config update error from Dragon: %s", error->valuestring);
            tab5_settings_set_voice_mode(0);
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
            tab5_settings_set_voice_mode(mode);
            ESP_LOGI(TAG, "Config update: voice_mode=%d (persisted)", mode);
            voice_async_refresh_badge();
        }
        cJSON *config = cJSON_GetObjectItem(root, "config");
        if (config) {
            cJSON *cloud = cJSON_GetObjectItem(config, "cloud_mode");
            if (cJSON_IsBool(cloud)) {
                bool is_cloud = cJSON_IsTrue(cloud);
                if (!cJSON_IsNumber(vmode)) {
                    tab5_settings_set_voice_mode(is_cloud ? 1 : 0);
                    voice_async_refresh_badge();
                }
            }
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
    } else if (strcmp(type_str, "tool_result") == 0) {
        cJSON *tool = cJSON_GetObjectItem(root, "tool");
        cJSON *exec_ms = cJSON_GetObjectItem(root, "execution_ms");
        const char *tool_name = cJSON_IsString(tool) ? tool->valuestring : "unknown";
        double ms = cJSON_IsNumber(exec_ms) ? exec_ms->valuedouble : 0.0;
        ESP_LOGI(TAG, "Tool result: %s (%.0fms)", tool_name, ms);
        voice_set_state(VOICE_STATE_PROCESSING, "Thinking...");
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
    } else if (strcmp(type_str, "audio_clip") == 0) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
        cJSON *dur_item = cJSON_GetObjectItem(root, "duration_s");
        float dur = cJSON_IsNumber(dur_item) ? (float)dur_item->valuedouble : 0.0f;
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(root, "label"));
        if (url) {
            ESP_LOGI(TAG, "Audio clip: %s (%.1fs)", label ? label : "", dur);
            ui_chat_push_audio_clip(url, dur, label);
        }
    } else if (strcmp(type_str, "widget_live") == 0 ||
               strcmp(type_str, "widget_live_update") == 0) {
        extern widget_t *widget_store_upsert(const widget_t *in);
        extern widget_t *widget_store_update(const char *card_id,
                                             const char *body,
                                             widget_tone_t tone,
                                             float progress,
                                             const char *action_label,
                                             const char *action_event);
        extern widget_tone_t widget_tone_from_str(const char *s);
        extern void ui_home_update_status(void);

        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
        if (!cid) {
            ESP_LOGW(TAG, "widget_live missing card_id");
        } else if (strcmp(type_str, "widget_live_update") == 0) {
            const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
            const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
            cJSON *prog_j = cJSON_GetObjectItem(root, "progress");
            float progress = cJSON_IsNumber(prog_j) ? (float)prog_j->valuedouble : -1.0f;
            cJSON *act = cJSON_GetObjectItem(root, "action");
            const char *al = act ? cJSON_GetStringValue(cJSON_GetObjectItem(act, "label")) : NULL;
            const char *ae = act ? cJSON_GetStringValue(cJSON_GetObjectItem(act, "event")) : NULL;
            widget_store_update(cid, body,
                                tone_s ? widget_tone_from_str(tone_s) : WIDGET_TONE_CALM,
                                progress, al, ae);
            lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
        } else {
            widget_t w = {0};
            strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
            const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "skill_id"));
            if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
            const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
            if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
            const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
            if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
            const char *icn = cJSON_GetStringValue(cJSON_GetObjectItem(root, "icon"));
            if (icn) strncpy(w.icon, icn, WIDGET_ICON_LEN - 1);
            const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
            w.tone = widget_tone_from_str(tone_s);
            cJSON *prog_j = cJSON_GetObjectItem(root, "progress");
            w.progress = cJSON_IsNumber(prog_j) ? (float)prog_j->valuedouble : 0.0f;
            cJSON *pri = cJSON_GetObjectItem(root, "priority");
            w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
            cJSON *act = cJSON_GetObjectItem(root, "action");
            if (cJSON_IsObject(act)) {
                const char *al = cJSON_GetStringValue(cJSON_GetObjectItem(act, "label"));
                const char *ae = cJSON_GetStringValue(cJSON_GetObjectItem(act, "event"));
                if (al) strncpy(w.action_label, al, WIDGET_ACTION_LBL_LEN - 1);
                if (ae) strncpy(w.action_event, ae, WIDGET_ACTION_EVT_LEN - 1);
            }
            w.type = WIDGET_TYPE_LIVE;
            widget_store_upsert(&w);
            lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
            ESP_LOGI(TAG, "widget_live upsert: %s/%s tone=%s", w.skill_id, cid,
                     tone_s ? tone_s : "calm");
        }
    } else if (strcmp(type_str, "widget_live_dismiss") == 0 ||
               strcmp(type_str, "widget_dismiss") == 0) {
        extern void widget_store_dismiss(const char *card_id);
        extern void ui_home_update_status(void);
        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
        if (cid) {
            widget_store_dismiss(cid);
            lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
            ESP_LOGI(TAG, "widget_live_dismiss: %s", cid);
        }
    } else {
        ESP_LOGW(TAG, "Unknown message type: %s (full: %.*s)", type_str, len, data);
    }

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Binary frame handling (TTS audio from Dragon) — called from event handler.
// Dragon sends PCM int16 mono at 16kHz; I2S TX runs at 48kHz, so we upsample
// 1:3 before writing to the playback ring buffer. Playback drain task
// handles I2S writes.
// ---------------------------------------------------------------------------
static size_t upsample_16k_to_48k(const int16_t *in, size_t in_samples,
                                    int16_t *out, size_t out_capacity)
{
    size_t out_idx = 0;
    for (size_t i = 0; i < in_samples && out_idx + UPSAMPLE_RATIO <= out_capacity; i++) {
        int16_t curr = in[i];
        int16_t next = (i + 1 < in_samples) ? in[i + 1] : curr;
        for (int j = 0; j < UPSAMPLE_RATIO; j++) {
            out[out_idx++] = (int16_t)(curr + (int32_t)(next - curr) * j / UPSAMPLE_RATIO);
        }
    }
    return out_idx;
}

static void handle_binary_message(const char *data, int len)
{
    if (s_state != VOICE_STATE_SPEAKING && s_state != VOICE_STATE_PROCESSING) {
        return;
    }
    if (s_state == VOICE_STATE_PROCESSING) {
        playback_buf_reset();
        voice_set_state(VOICE_STATE_SPEAKING, NULL);
    }

    /* Upsample into a stack-adjacent but reasonably-sized temp buffer. A single
     * Dragon TTS chunk is typically 640–1024 B (320–512 samples), so
     * worst-case output is 512 × 3 = 1536 samples = 3072 B. Cap at 2048
     * input samples to stay well under the managed-client buffer. */
    const size_t in_samples   = len / sizeof(int16_t);
    if (in_samples == 0) return;
    const size_t max_out      = in_samples * UPSAMPLE_RATIO;
    int16_t *upsample_buf = (int16_t *)heap_caps_malloc(
        max_out * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!upsample_buf) {
        ESP_LOGW(TAG, "handle_binary: OOM for %zu samples", max_out);
        return;
    }
    size_t out_samples = upsample_16k_to_48k((const int16_t *)data, in_samples,
                                              upsample_buf, max_out);
    playback_buf_write(upsample_buf, out_samples);
    heap_caps_free(upsample_buf);
}

// ---------------------------------------------------------------------------
// WebSocket event handler (runs on esp_websocket_client's internal task)
// ---------------------------------------------------------------------------
static void voice_ws_event_handler(void *arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "WS: BEFORE_CONNECT (host=%s%s)",
                 s_dragon_host, s_using_ngrok ? " [ngrok]" : "");
        voice_set_state(VOICE_STATE_CONNECTING, s_dragon_host);
        break;

    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "WS: CONNECTED — sending register frame");
        /* US-C02: bump session gen on every successful connect. */
        s_session_gen++;
        s_handshake_fail_cnt = 0;

        esp_err_t reg_err = voice_ws_send_register();
        if (reg_err != ESP_OK) {
            ESP_LOGE(TAG, "WS: register send failed (%s)", esp_err_to_name(reg_err));
            /* Let auto-reconnect re-attempt. */
        }
        /* Don't set READY yet — wait for Dragon's session_start reply so
         * we know the backend pipeline is fully up. Keep state CONNECTING
         * until session_start arrives (~6s model load on first boot). */
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS: DISCONNECTED");
        /* Flush playback so we don't keep speaking into a dead pipe. */
        playback_buf_reset();
        tab5_audio_speaker_enable(false);
        voice_set_state(VOICE_STATE_IDLE, "disconnected");
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WS: CLOSED");
        voice_set_state(VOICE_STATE_IDLE, "closed");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (!data) break;
        s_last_activity_us = esp_timer_get_time();
        if (data->op_code == WS_TRANSPORT_OPCODES_TEXT && data->data_len > 0) {
            ESP_LOGI(TAG, "WS recv text (%d bytes): %.*s", data->data_len,
                     data->data_len > 200 ? 200 : data->data_len, data->data_ptr);
            handle_text_message(data->data_ptr, data->data_len);
        } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY && data->data_len > 0) {
            handle_binary_message(data->data_ptr, data->data_len);
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
        ESP_LOGW(TAG, "WS: ERROR (type=%d, handshake_status=%d, sock_errno=%d)",
                 (int)et, status,
                 data ? data->error_handle.esp_transport_sock_errno : 0);

        /* Handshake failure → try ngrok fallback (only in conn_mode=auto,
         * only while still on local URI, after NGROK_FALLBACK_THRESHOLD fails). */
        if (et == WEBSOCKET_ERROR_TYPE_HANDSHAKE && status != 101) {
            s_handshake_fail_cnt++;
            uint8_t conn_mode = tab5_settings_get_connection_mode();
            if (conn_mode == 0 && !s_using_ngrok
                && s_handshake_fail_cnt >= NGROK_FALLBACK_THRESHOLD) {
                char ngrok_uri[128];
                snprintf(ngrok_uri, sizeof(ngrok_uri),
                         "wss://%s:%d%s",
                         TAB5_NGROK_HOST, TAB5_NGROK_PORT, TAB5_VOICE_WS_PATH);
                ESP_LOGW(TAG, "WS: handshake failed %d× — falling back to %s",
                         s_handshake_fail_cnt, ngrok_uri);
                s_using_ngrok = true;
                s_handshake_fail_cnt = 0;
                /* set_uri requires a stopped client. The client is
                 * currently between reconnect attempts; stop+set+start
                 * to force it onto the new URI immediately. */
                esp_websocket_client_stop(s_ws);
                esp_websocket_client_set_uri(s_ws, ngrok_uri);
                strncpy(s_dragon_host, TAB5_NGROK_HOST, sizeof(s_dragon_host) - 1);
                s_dragon_host[sizeof(s_dragon_host) - 1] = '\0';
                s_dragon_port = TAB5_NGROK_PORT;
                esp_websocket_client_start(s_ws);
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
static void voice_build_local_uri(char *out, size_t out_cap,
                                   const char *host, uint16_t port)
{
    snprintf(out, out_cap, "ws://%s:%u%s", host, (unsigned)port, TAB5_VOICE_WS_PATH);
}

// ---------------------------------------------------------------------------
// Wake word handler — called from AFE detect task (parked)
// ---------------------------------------------------------------------------
void voice_on_wake(void)
{
    voice_state_t cur = s_state;

    if (cur == VOICE_STATE_IDLE || cur == VOICE_STATE_READY) {
        ESP_LOGI(TAG, "Wake → LISTENING");
        voice_set_state(VOICE_STATE_LISTENING, NULL);

        if (s_ws && esp_websocket_client_is_connected(s_ws)) {
            voice_ws_send_text("{\"type\":\"start\",\"mode\":\"ask\",\"wake\":true}");
        }
    } else if (cur == VOICE_STATE_SPEAKING) {
        ESP_LOGI(TAG, "Wake → BARGE-IN (interrupting TTS)");

        if (s_play_mutex) {
            xSemaphoreTake(s_play_mutex, portMAX_DELAY);
            s_play_wr = 0;
            s_play_rd = 0;
            s_play_count = 0;
            xSemaphoreGive(s_play_mutex);
        }

        if (s_ws && esp_websocket_client_is_connected(s_ws)) {
            voice_ws_send_text("{\"type\":\"cancel\"}");
        }

        voice_set_state(VOICE_STATE_LISTENING, NULL);
        if (s_ws && esp_websocket_client_is_connected(s_ws)) {
            voice_ws_send_text("{\"type\":\"start\",\"mode\":\"ask\",\"wake\":true}");
        }
    }
}

// ---------------------------------------------------------------------------
// Mic capture task — reads 4-ch TDM at 48kHz, extracts MIC1, downsamples
// to 16kHz mono, sends via WS to Dragon.
// ---------------------------------------------------------------------------
static void mic_capture_task(void *arg)
{
    ESP_LOGI(TAG, "Mic capture task started (core %d)", xPortGetCoreID());

    int16_t *tdm_buf = (int16_t *)heap_caps_malloc(
        MIC_TDM_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono_buf = (int16_t *)heap_caps_malloc(
        VOICE_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *afe_buf = NULL;
    int afe_chunksize = 0;
    int afe_buf_pos = 0;
    if (s_afe_enabled) {
        afe_chunksize = tab5_afe_get_feed_chunksize();
        if (afe_chunksize <= 0) afe_chunksize = 512;
        size_t alloc_size = ((afe_chunksize * sizeof(int16_t) + 63) / 64) * 64;
        afe_buf = (int16_t *)heap_caps_aligned_alloc(64, alloc_size,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "AFE feed buffer: %d samples (%zu bytes), buf=%p",
                 afe_chunksize, alloc_size, afe_buf);
    }
    int16_t *afe_tmp = NULL;
    if (s_afe_enabled) {
        afe_tmp = (int16_t *)heap_caps_malloc(
            VOICE_CHUNK_SAMPLES * AFE_FEED_CHANNELS * sizeof(int16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!tdm_buf || !mono_buf || (s_afe_enabled && !afe_buf)) {
        ESP_LOGE(TAG, "Failed to allocate mic buffers");
        heap_caps_free(tdm_buf);
        heap_caps_free(mono_buf);
        heap_caps_free(afe_buf);
        s_mic_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_mic_running = true;
    int frames_sent = 0;

    int silence_frames = 0;
    int total_silence_frames = 0;
    bool had_speech = false;
    bool auto_stop_warning_shown = false;

    #define CALIBRATION_FRAMES 25
    float noise_sum = 0;
    int cal_count = 0;
    float dictation_threshold = DICTATION_SILENCE_THRESHOLD;

    bool ws_live = (s_ws != NULL) && esp_websocket_client_is_connected(s_ws);

    while (s_mic_running && (ws_live || s_voice_mode == VOICE_MODE_DICTATE || s_afe_enabled)) {
        esp_err_t err = tab5_mic_read(tdm_buf, MIC_TDM_SAMPLES, 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Mic read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (frames_sent < 5) {
            for (int ch = 0; ch < MIC_TDM_CHANNELS; ch++) {
                int64_t sqsum = 0;
                int16_t mn = 0, mx = 0;
                int nz = 0;
                for (int i = ch; i < MIC_TDM_SAMPLES; i += MIC_TDM_CHANNELS) {
                    int16_t v = tdm_buf[i];
                    sqsum += (int64_t)v * v;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                    if (v != 0) nz++;
                }
                int count = MIC_48K_FRAMES;
                float rms = sqrtf((float)(sqsum / (count > 0 ? count : 1)));
                ESP_LOGI(TAG, "SLOT_DIAG #%d CH%d: min=%d max=%d rms=%.0f nz=%d/%d",
                         frames_sent, ch, mn, mx, rms, nz, count);
            }
        }

        int out_idx = 0;

        if (s_afe_enabled && afe_buf) {
            for (int i = 0; i + DOWNSAMPLE_RATIO - 1 < MIC_48K_FRAMES && out_idx < VOICE_CHUNK_SAMPLES; i += DOWNSAMPLE_RATIO) {
                int32_t m1 = 0, ref = 0;
                for (int j = 0; j < DOWNSAMPLE_RATIO; j++) {
                    int base = (i + j) * MIC_TDM_CHANNELS;
                    m1  += tdm_buf[base + MIC_TDM_MIC1_OFF];
                    ref += tdm_buf[base + MIC_TDM_REF_OFF];
                }
                afe_tmp[out_idx * AFE_FEED_CHANNELS + 0] = (int16_t)(m1 / DOWNSAMPLE_RATIO);
                afe_tmp[out_idx * AFE_FEED_CHANNELS + 1] = (int16_t)(ref / DOWNSAMPLE_RATIO);
                mono_buf[out_idx] = (int16_t)(m1 / DOWNSAMPLE_RATIO);
                out_idx++;
            }
            int produced = out_idx * AFE_FEED_CHANNELS;
            int src_pos = 0;
            while (src_pos < produced) {
                int space = afe_chunksize - afe_buf_pos;
                int avail = produced - src_pos;
                int to_copy = (avail < space) ? avail : space;
                memcpy(&afe_buf[afe_buf_pos], &afe_tmp[src_pos], to_copy * sizeof(int16_t));
                afe_buf_pos += to_copy;
                src_pos += to_copy;

                if (afe_buf_pos >= afe_chunksize) {
                    tab5_afe_feed(afe_buf, afe_chunksize);
                    afe_buf_pos = 0;
                    static int feed_count = 0;
                    if (++feed_count % 100 == 0) {
                        ESP_LOGI(TAG, "AFE fed %d chunks", feed_count);
                    }
                }
            }
        } else {
            for (int i = 0; i + DOWNSAMPLE_RATIO - 1 < MIC_48K_FRAMES && out_idx < VOICE_CHUNK_SAMPLES; i += DOWNSAMPLE_RATIO) {
                int32_t sum = 0;
                for (int j = 0; j < DOWNSAMPLE_RATIO; j++) {
                    sum += tdm_buf[(i + j) * MIC_TDM_CHANNELS + MIC_TDM_MIC1_OFF];
                }
                mono_buf[out_idx++] = (int16_t)(sum / DOWNSAMPLE_RATIO);
            }
        }

        if (frames_sent % 50 == 0) {
            int16_t mn = 0, mx = 0;
            int64_t sqsum = 0;
            for (int k = 0; k < out_idx; k++) {
                int16_t v = mono_buf[k];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
                sqsum += (int64_t)v * v;
            }
            float rms = sqrtf((float)(sqsum / (out_idx > 0 ? out_idx : 1)));
            ESP_LOGI(TAG, "Mic chunk #%d (slot %d): min=%d max=%d rms=%.0f samples=%d",
                     frames_sent, MIC_TDM_MIC1_OFF, mn, mx, rms, out_idx);
        }

        /* Re-check connection each loop — esp_websocket_client may
         * have reconnected (or gone away) in the background. */
        ws_live = (s_ws != NULL) && esp_websocket_client_is_connected(s_ws);

        if (!s_afe_enabled) {
            ui_notes_write_audio(mono_buf, out_idx);

            size_t send_bytes = out_idx * sizeof(int16_t);
            if (ws_live) {
                err = voice_ws_send_binary(mono_buf, send_bytes);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "WS send failed — continuing SD recording");
                    if (s_voice_mode == VOICE_MODE_ASK) break;
                }
            }
        }
        frames_sent++;

        if (s_voice_mode == VOICE_MODE_ASK && !s_afe_enabled && frames_sent >= MAX_RECORD_FRAMES_ASK) {
            ESP_LOGI(TAG, "Max recording duration reached (%ds)",
                     MAX_RECORD_FRAMES_ASK * TAB5_VOICE_CHUNK_MS / 1000);
            voice_set_state(VOICE_STATE_LISTENING, "max_duration");
            break;
        }

        if (s_voice_mode == VOICE_MODE_DICTATE && !s_afe_enabled) {
            int64_t sqsum = 0;
            for (int k = 0; k < out_idx; k++) {
                sqsum += (int64_t)mono_buf[k] * mono_buf[k];
            }
            float rms = sqrtf((float)(sqsum / (out_idx > 0 ? out_idx : 1)));
            s_current_rms = rms;

            if (cal_count < CALIBRATION_FRAMES) {
                noise_sum += rms;
                cal_count++;
                if (cal_count == CALIBRATION_FRAMES) {
                    float ambient = noise_sum / CALIBRATION_FRAMES;
                    dictation_threshold = fmaxf(400.0f, fminf(1500.0f, ambient * 2.0f));
                    ESP_LOGI(TAG, "VAD calibrated: ambient=%.0f, threshold=%.0f",
                             ambient, dictation_threshold);
                }
                continue;
            }

            if (rms < dictation_threshold) {
                silence_frames++;
                total_silence_frames++;

                if (had_speech && total_silence_frames == DICTATION_WARN_3S_FRAMES) {
                    ui_voice_show_auto_stop_warning(2);
                    auto_stop_warning_shown = true;
                } else if (had_speech && total_silence_frames == DICTATION_WARN_4S_FRAMES) {
                    ui_voice_show_auto_stop_warning(1);
                }
            } else {
                if (had_speech && silence_frames >= DICTATION_SILENCE_FRAMES) {
                    ESP_LOGI(TAG, "Dictation: pause (%dms), sending segment",
                             silence_frames * TAB5_VOICE_CHUNK_MS);
                    voice_ws_send_text("{\"type\":\"segment\"}");
                }
                if (auto_stop_warning_shown) {
                    ui_voice_show_auto_stop_warning(0);
                    auto_stop_warning_shown = false;
                }
                silence_frames = 0;
                total_silence_frames = 0;
                had_speech = true;
            }

            if (had_speech && total_silence_frames >= DICTATION_AUTO_STOP_FRAMES) {
                ESP_LOGI(TAG, "Dictation auto-stop: %ds silence",
                         DICTATION_AUTO_STOP_FRAMES * TAB5_VOICE_CHUNK_MS / 1000);
                break;
            }
        }
    }

    s_mic_running = false;
    s_current_rms = 0;
    heap_caps_free(tdm_buf);
    heap_caps_free(mono_buf);
    heap_caps_free(afe_buf);
    heap_caps_free(afe_tmp);

    if (s_voice_mode == VOICE_MODE_DICTATE && had_speech
        && total_silence_frames >= DICTATION_AUTO_STOP_FRAMES
        && s_ws && esp_websocket_client_is_connected(s_ws)) {
        voice_ws_send_text("{\"type\":\"stop\"}");
        voice_set_state(VOICE_STATE_PROCESSING, NULL);
    }

    ESP_LOGI(TAG, "Mic capture task exiting");
    s_mic_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Playback drain task
// ---------------------------------------------------------------------------
static void playback_drain_task(void *arg)
{
    ESP_LOGI(TAG, "Playback drain task started (core %d, prio %d)",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    int16_t *chunk = (int16_t *)heap_caps_malloc(
        VOICE_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        ESP_LOGE(TAG, "Playback drain: failed to allocate chunk buffer");
        s_play_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_play_running = true;

    while (s_play_running) {
        xSemaphoreTake(s_play_sem, pdMS_TO_TICKS(50));

        while (s_play_running) {
            size_t avail = playback_buf_read(chunk, VOICE_CHUNK_SAMPLES);
            if (avail == 0) {
                if (s_tts_done) {
                    s_tts_done = false;
                    ESP_LOGI(TAG, "Playback drain complete — transitioning to READY");
                    tab5_audio_speaker_enable(false);
                    voice_set_state(VOICE_STATE_READY, NULL);
                }
                break;
            }
            tab5_audio_play_raw(chunk, avail);
        }
    }

    heap_caps_free(chunk);
    ESP_LOGI(TAG, "Playback drain task exiting");
    s_play_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t voice_init(voice_state_cb_t state_cb)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing voice module");

    s_state = VOICE_STATE_IDLE;
    s_disconnecting = false;
    s_started = false;
    s_handshake_fail_cnt = 0;
    s_using_ngrok = false;

    s_state_cb = state_cb;

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    s_play_mutex = xSemaphoreCreateMutex();
    if (!s_play_mutex) {
        ESP_LOGE(TAG, "Failed to create playback mutex");
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_play_sem = xSemaphoreCreateBinary();
    if (!s_play_sem) {
        ESP_LOGE(TAG, "Failed to create playback semaphore");
        vSemaphoreDelete(s_play_mutex);
        vSemaphoreDelete(s_state_mutex);
        s_play_mutex = NULL;
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_play_capacity = TAB5_VOICE_PLAYBACK_BUF / sizeof(int16_t);
    s_play_buf = (int16_t *)heap_caps_malloc(
        TAB5_VOICE_PLAYBACK_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_play_buf) {
        ESP_LOGE(TAG, "Failed to allocate playback buffer in PSRAM");
        vSemaphoreDelete(s_play_sem);
        vSemaphoreDelete(s_play_mutex);
        vSemaphoreDelete(s_state_mutex);
        s_play_sem = NULL;
        s_play_mutex = NULL;
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    playback_buf_reset();
    s_transcript[0] = '\0';

    /* Spawn the playback drain task once, at init — it sleeps on s_play_sem
     * until TTS audio arrives, and persists across reconnects. */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        playback_drain_task, "voice_play", PLAY_TASK_STACK_SIZE,
        NULL, PLAY_TASK_PRIORITY, &s_play_task, PLAY_TASK_CORE,
        MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create playback drain task — audio may stutter");
    }

    s_initialized = true;
    voice_set_state(VOICE_STATE_IDLE, NULL);
    ESP_LOGI(TAG, "Voice module initialized");
    return ESP_OK;
}

/* Initialize and start the esp_websocket_client for the given host/port.
 * Called from voice_connect (sync) and voice_connect_async. Safe to call
 * multiple times — if the client already exists, just updates the URI
 * and re-starts. */
static esp_err_t voice_ws_start_client(const char *dragon_host, uint16_t dragon_port)
{
    uint8_t conn_mode = tab5_settings_get_connection_mode();
    ESP_LOGI(TAG, "Connection mode: %d (%s)",
             conn_mode, conn_mode == 0 ? "auto" : conn_mode == 1 ? "local" : "remote");

    /* Choose initial URI.
     * - conn_mode 0 (auto)  : start local, let error handler fall back to ngrok.
     * - conn_mode 1 (local) : always local; fallback disabled.
     * - conn_mode 2 (remote): straight to ngrok. */
    char uri[128];
    bool start_ngrok = (conn_mode == 2);
    if (start_ngrok) {
        snprintf(uri, sizeof(uri), "wss://%s:%d%s",
                 TAB5_NGROK_HOST, TAB5_NGROK_PORT, TAB5_VOICE_WS_PATH);
        strncpy(s_dragon_host, TAB5_NGROK_HOST, sizeof(s_dragon_host) - 1);
        s_dragon_host[sizeof(s_dragon_host) - 1] = '\0';
        s_dragon_port = TAB5_NGROK_PORT;
        s_using_ngrok = true;
    } else {
        voice_build_local_uri(uri, sizeof(uri), dragon_host, dragon_port);
        strncpy(s_dragon_host, dragon_host, sizeof(s_dragon_host) - 1);
        s_dragon_host[sizeof(s_dragon_host) - 1] = '\0';
        s_dragon_port = dragon_port;
        s_using_ngrok = false;
    }
    s_handshake_fail_cnt = 0;

    /* If client already exists, just re-target the URI.  The managed
     * component requires stop() before set_uri() when started. */
    if (s_ws) {
        ESP_LOGI(TAG, "Re-using existing WS client; set_uri=%s", uri);
        if (s_started) {
            esp_websocket_client_stop(s_ws);
        }
        esp_err_t ur = esp_websocket_client_set_uri(s_ws, uri);
        if (ur != ESP_OK) {
            ESP_LOGE(TAG, "set_uri failed: %s", esp_err_to_name(ur));
            return ur;
        }
        esp_err_t sr = esp_websocket_client_start(s_ws);
        if (sr != ESP_OK) {
            ESP_LOGE(TAG, "client_start failed: %s", esp_err_to_name(sr));
            return sr;
        }
        s_started = true;
        return ESP_OK;
    }

    esp_websocket_client_config_t cfg = {
        .uri                    = uri,
        .task_stack             = WS_CLIENT_TASK_STACK,
        .task_prio              = tskIDLE_PRIORITY + 5,
        .buffer_size            = WS_CLIENT_BUFFER_SIZE,
        .reconnect_timeout_ms   = WS_CLIENT_RECONNECT_MS,
        .network_timeout_ms     = WS_CLIENT_NETWORK_MS,
        .ping_interval_sec      = WS_CLIENT_PING_SEC,
        .pingpong_timeout_sec   = WS_CLIENT_PONG_SEC,
        .disable_auto_reconnect = false,
        /* Attach cert bundle unconditionally — harmless on ws:// (plain TCP),
         * required for wss:// ngrok.  This sidesteps the need to tear down
         * and recreate the client when falling back from local to ngrok. */
        .crt_bundle_attach      = esp_crt_bundle_attach,
    };

    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_FAIL;
    }

    esp_err_t er = esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                  voice_ws_event_handler, NULL);
    if (er != ESP_OK) {
        ESP_LOGE(TAG, "register_events failed: %s", esp_err_to_name(er));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return er;
    }

    esp_err_t sr = esp_websocket_client_start(s_ws);
    if (sr != ESP_OK) {
        ESP_LOGE(TAG, "client_start failed: %s", esp_err_to_name(sr));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return sr;
    }
    s_started = true;
    ESP_LOGI(TAG, "WS client started: %s", uri);
    return ESP_OK;
}

esp_err_t voice_connect(const char *dragon_host, uint16_t dragon_port)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_disconnecting) {
        ESP_LOGW(TAG, "Disconnect in progress — refusing connect");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "Already connected");
        return ESP_OK;
    }

    if (s_state != VOICE_STATE_IDLE && s_state != VOICE_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Stale voice state %d on connect — forcing IDLE", s_state);
        s_state = VOICE_STATE_IDLE;
    }

    voice_set_state(VOICE_STATE_CONNECTING, dragon_host);

    return voice_ws_start_client(dragon_host, dragon_port);
}

typedef struct {
    char     host[64];
    uint16_t port;
    bool     auto_listen;
} async_connect_args_t;

static void async_connect_task(void *arg)
{
    async_connect_args_t *args = (async_connect_args_t *)arg;

    ESP_LOGI(TAG, "async_connect_task: starting (host=%s:%d, auto_listen=%d)",
             args->host, args->port, args->auto_listen);

    esp_err_t ret = voice_connect(args->host, args->port);
    ESP_LOGI(TAG, "async_connect_task: voice_connect returned %s",
             esp_err_to_name(ret));

    if (ret == ESP_OK && args->auto_listen) {
        /* Wait for WS + session_start → READY (up to 15s). */
        for (int i = 0; i < 150 && s_state != VOICE_STATE_READY; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_state == VOICE_STATE_READY) {
            ESP_LOGI(TAG, "async_connect_task: calling voice_start_listening (auto_listen)");
            voice_start_listening();
        } else {
            ESP_LOGW(TAG, "async_connect_task: never reached READY, skipping auto_listen");
        }
    }

    free(args);
    vTaskDelete(NULL);
}

esp_err_t voice_connect_async(const char *dragon_host, uint16_t dragon_port,
                               bool auto_listen)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_disconnecting) {
        ESP_LOGW(TAG, "Disconnect in progress — refusing async connect");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "Already connected");
        if (auto_listen) {
            return voice_start_listening();
        }
        return ESP_OK;
    }

    voice_set_state(VOICE_STATE_CONNECTING, dragon_host);

    async_connect_args_t *args = malloc(sizeof(async_connect_args_t));
    if (!args) {
        voice_set_state(VOICE_STATE_IDLE, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    strncpy(args->host, dragon_host, sizeof(args->host) - 1);
    args->host[sizeof(args->host) - 1] = '\0';
    args->port = dragon_port;
    args->auto_listen = auto_listen;

    BaseType_t xret = xTaskCreatePinnedToCore(
        async_connect_task, "voice_conn", 8192,
        args, 4, NULL, 1);
    if (xret != pdPASS) {
        free(args);
        voice_set_state(VOICE_STATE_IDLE, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voice_start_listening(void)
{
    bool ws_live = s_ws && esp_websocket_client_is_connected(s_ws);
    ESP_LOGI(TAG, "voice_start_listening: initialized=%d, ws_live=%d, state=%d",
             s_initialized, ws_live, s_state);

    if (tab5_settings_get_mic_mute()) {
        ESP_LOGW(TAG, "voice_start_listening: mic is muted, refusing");
        extern void ui_home_show_toast(const char *text);
        ui_home_show_toast("Mic is muted -- unmute in Settings");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_initialized || !ws_live) {
        ESP_LOGE(TAG, "Not connected (initialized=%d, ws_live=%d)",
                 s_initialized, ws_live);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != VOICE_STATE_READY) {
        ESP_LOGW(TAG, "Cannot start listening in state %d", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting push-to-talk (ask mode)");
    s_voice_mode = VOICE_MODE_ASK;

    ESP_LOGI(TAG, "Sending {\"type\":\"start\"} to Dragon...");
    esp_err_t err = voice_ws_send_text("{\"type\":\"start\"}");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send start signal");
        return err;
    }
    ESP_LOGI(TAG, "Start signal sent OK");

    s_transcript[0] = '\0';
    s_stt_text[0] = '\0';
    s_llm_text[0] = '\0';

    s_mic_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        mic_capture_task, "voice_mic", MIC_TASK_STACK_SIZE,
        NULL, MIC_TASK_PRIORITY, &s_mic_task, MIC_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mic capture task");
        voice_ws_send_text("{\"type\":\"stop\"}");
        s_mic_running = false;
        return ESP_ERR_NO_MEM;
    }

    voice_set_state(VOICE_STATE_LISTENING, NULL);
    return ESP_OK;
}

esp_err_t voice_start_dictation(void)
{
    bool ws_live = s_ws && esp_websocket_client_is_connected(s_ws);
    if (!s_initialized || !ws_live) {
        ESP_LOGE(TAG, "Not connected for dictation");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != VOICE_STATE_READY) {
        ESP_LOGW(TAG, "Cannot start dictation in state %d", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting dictation mode (unlimited, STT-only)");
    s_voice_mode = VOICE_MODE_DICTATE;

    if (!s_dictation_text) {
        s_dictation_text = heap_caps_malloc(DICTATION_TEXT_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_dictation_text) {
            ESP_LOGE(TAG, "Failed to allocate dictation buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    s_dictation_text[0] = '\0';

    esp_err_t err = voice_ws_send_text("{\"type\":\"start\",\"mode\":\"dictate\"}");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send dictation start");
        return err;
    }

    s_transcript[0] = '\0';
    s_stt_text[0] = '\0';
    s_llm_text[0] = '\0';

    s_mic_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        mic_capture_task, "voice_mic", MIC_TASK_STACK_SIZE,
        NULL, MIC_TASK_PRIORITY, &s_mic_task, MIC_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mic capture task");
        voice_ws_send_text("{\"type\":\"stop\"}");
        s_mic_running = false;
        return ESP_ERR_NO_MEM;
    }

    voice_set_state(VOICE_STATE_LISTENING, NULL);
    return ESP_OK;
}

voice_mode_t voice_get_mode(void)
{
    return s_voice_mode;
}

const char *voice_get_dictation_text(void)
{
    return s_dictation_text ? s_dictation_text : "";
}

esp_err_t voice_clear_history(void)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Clearing conversation history on Dragon");
    return voice_ws_send_text("{\"type\":\"clear\"}");
}

esp_err_t voice_stop_listening(void)
{
    if (s_state != VOICE_STATE_LISTENING) {
        ESP_LOGW(TAG, "Not listening (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping push-to-talk");

    s_mic_running = false;

    i2s_chan_handle_t rx_h = tab5_audio_get_i2s_rx();
    if (rx_h) {
        i2s_channel_disable(rx_h);
    }

    for (int i = 0; i < 120 && s_mic_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (rx_h) {
        i2s_channel_enable(rx_h);
    }

    esp_err_t err = voice_ws_send_text("{\"type\":\"stop\"}");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Stop signal failed — connection lost");
        voice_set_state(VOICE_STATE_IDLE, "Connection lost");
        return ESP_FAIL;
    }

    voice_reset_activity_timestamp();

    voice_set_state(VOICE_STATE_PROCESSING, NULL);
    return ESP_OK;
}

esp_err_t voice_cancel(void)
{
    if (s_state == VOICE_STATE_IDLE) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Cancelling voice session");

    if (s_mic_running) {
        s_mic_running = false;
        i2s_chan_handle_t rx_h = tab5_audio_get_i2s_rx();
        if (rx_h) {
            i2s_channel_disable(rx_h);
        }
        for (int i = 0; i < 120 && s_mic_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (rx_h) {
            i2s_channel_enable(rx_h);
        }
    }

    playback_buf_reset();
    tab5_audio_speaker_enable(false);

    bool ws_live = s_ws && esp_websocket_client_is_connected(s_ws);
    if (ws_live) {
        voice_ws_send_text("{\"type\":\"cancel\"}");
    }

    if (ws_live) {
        voice_set_state(VOICE_STATE_READY, "cancelled");
    } else {
        voice_set_state(VOICE_STATE_IDLE, "cancelled");
    }

    return ESP_OK;
}

esp_err_t voice_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting from Dragon voice server");

    /* US-C02: Bump session gen FIRST so any already-enqueued async calls
     * are quietly dropped once they reach LVGL. */
    s_session_gen++;

    s_disconnecting = true;

    tab5_audio_speaker_enable(false);

    /* Stop mic first — it uses voice_ws_send_binary(). */
    s_mic_running = false;

    i2s_chan_handle_t rx_h = tab5_audio_get_i2s_rx();
    if (rx_h) {
        i2s_channel_disable(rx_h);
    }

    for (int i = 0; i < 120 && s_mic_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_mic_task != NULL) {
        ESP_LOGW(TAG, "Mic task did not exit in 1200ms — forcing handle NULL");
    }
    s_mic_task = NULL;

    if (rx_h) {
        i2s_channel_enable(rx_h);
    }

    /* Stop the WS client. We keep the handle around (don't destroy) so
     * future voice_connect/_async calls can set_uri + start without
     * re-creating everything. Destroy happens never in practice — the
     * client lives for the lifetime of the process. */
    if (s_ws && s_started) {
        esp_websocket_client_stop(s_ws);
        s_started = false;
    }

    playback_buf_reset();
    voice_set_state(VOICE_STATE_IDLE, NULL);

    s_disconnecting = false;

    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

voice_state_t voice_get_state(void)
{
    voice_state_t state;
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        state = s_state;
        xSemaphoreGive(s_state_mutex);
    } else {
        state = s_state;
    }
    return state;
}

const char *voice_get_last_transcript(void)
{
    return s_transcript;
}

const char *voice_get_stt_text(void)
{
    return s_stt_text;
}

const char *voice_get_llm_text(void)
{
    return s_llm_text;
}

esp_err_t voice_send_text(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;

    if (s_state == VOICE_STATE_LISTENING) {
        ESP_LOGI(TAG, "voice_send_text: cancelling active LISTENING to allow text send");
        voice_cancel();
    }
    if (s_state == VOICE_STATE_PROCESSING || s_state == VOICE_STATE_SPEAKING) {
        ESP_LOGW(TAG, "voice_send_text: rejected — voice pipeline active (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "text");
    cJSON_AddStringToObject(msg, "content", text);
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Sending text to Dragon: %s", text);

    if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strncpy(s_stt_text, text, MAX_TRANSCRIPT_LEN - 1);
    s_stt_text[MAX_TRANSCRIPT_LEN - 1] = '\0';
    strncpy(s_transcript, text, MAX_TRANSCRIPT_LEN - 1);
    s_transcript[MAX_TRANSCRIPT_LEN - 1] = '\0';
    s_llm_text[0] = '\0';
    if (s_state_mutex) xSemaphoreGive(s_state_mutex);

    esp_err_t ret = voice_ws_send_text(json);
    cJSON_free(json);

    if (ret == ESP_OK) {
        voice_set_state(VOICE_STATE_PROCESSING, text);
    }
    return ret;
}

esp_err_t voice_send_widget_action(const char *card_id, const char *event,
                                   const char *payload_json)
{
    if (!card_id || !event) return ESP_ERR_INVALID_ARG;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;

    /* Rate limit: 4/sec max (see docs/WIDGETS.md §11). */
    static uint32_t s_action_bucket = 4;
    static uint32_t s_action_tick   = 0;
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

esp_err_t voice_send_config_update(int voice_mode, const char *llm_model)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "config_update");
    cJSON_AddNumberToObject(msg, "voice_mode", voice_mode);
    cJSON_AddBoolToObject(msg, "cloud_mode", voice_mode >= 1);
    if (voice_mode == 2 && llm_model && llm_model[0]) {
        cJSON_AddStringToObject(msg, "llm_model", llm_model);
    }
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Sending config_update: voice_mode=%d llm_model=%s",
             voice_mode, (llm_model && llm_model[0]) ? llm_model : "(local)");
    esp_err_t ret = voice_ws_send_text(json);
    cJSON_free(json);
    return ret;
}

float voice_get_current_rms(void)
{
    return s_current_rms;
}

const char *voice_get_dictation_title(void)
{
    return s_dictation_title;
}

const char *voice_get_dictation_summary(void)
{
    return s_dictation_summary;
}

// ---------------------------------------------------------------------------
// Reconnect watchdog — superseded by esp_websocket_client's built-in
// auto-reconnect. Public API preserved as no-ops for backward compat so
// callers in main.c / debug_server.c still link. See issue #76.
// ---------------------------------------------------------------------------
esp_err_t voice_start_reconnect_watchdog(void)
{
    ESP_LOGI(TAG, "reconnect watchdog superseded by esp_websocket_client built-in auto-reconnect");
    return ESP_OK;
}

void voice_stop_reconnect_watchdog(void)
{
    /* No-op — the managed client's auto-reconnect loop runs until
     * esp_websocket_client_stop() is called, which voice_disconnect()
     * handles on the teardown path. */
}

void voice_force_reconnect(void)
{
    /* Stop + start: the managed client will immediately re-attempt the
     * connect handshake, bypassing any pending reconnect_timeout_ms delay. */
    if (!s_initialized || !s_ws) {
        ESP_LOGW(TAG, "voice_force_reconnect: client not ready");
        return;
    }
    ESP_LOGI(TAG, "Force reconnect");
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_start(s_ws);
    s_started = true;
}

bool voice_is_connected(void)
{
    return s_ws && esp_websocket_client_is_connected(s_ws);
}

// ---------------------------------------------------------------------------
// Always-listening mode (AFE + wake word) — PARKED.
//
// Wake-word always-listening is parked because the AEC reference channel
// lands on the wrong TDM slot and cancels the real voice. To un-park:
//   1. Remove #define WAKE_WORD_PARKED below and the #ifndef/#endif gate
//      around _voice_start_always_listening_impl.
//   2. Restore the Wake Word row + cb_wake_word in ui_settings.c.
//   3. Unhide wake_listening / wake_word fields in debug_server.c /info.
// ---------------------------------------------------------------------------
#define WAKE_WORD_PARKED 1

esp_err_t voice_start_always_listening(void)
{
    ESP_LOGW(TAG, "Wake-word mode is parked — start request ignored");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_stop_always_listening(void)
{
    return ESP_OK;
}

bool voice_is_always_listening(void)
{
    return false;
}

#ifndef WAKE_WORD_PARKED
/* Original implementation preserved verbatim for easy revival. The AFE feed
 * audio (clean_audio) is streamed to Dragon via voice_ws_send_binary() now. */
static volatile bool s_afe_listening   = false;
static TaskHandle_t  s_afe_detect_task = NULL;

static void afe_detect_task(void *arg)
{
    ESP_LOGI(TAG, "AFE detect task started (core %d), afe_listening=%d",
             xPortGetCoreID(), s_afe_listening);
    int fetch_count = 0;

    while (s_afe_listening) {
        int16_t *clean_audio = NULL;
        int clean_samples = 0;
        bool wake_detected = false;
        bool vad_speech = false;

        if (fetch_count == 0) {
            ESP_LOGI(TAG, "AFE detect: calling first fetch()...");
        }
        esp_err_t err = tab5_afe_fetch(&clean_audio, &clean_samples, &wake_detected, &vad_speech);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AFE fetch failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        fetch_count++;
        if (fetch_count % 100 == 0 || fetch_count <= 3) {
            ESP_LOGI(TAG, "AFE fetch #%d: wake=%d vad=%d samples=%d",
                     fetch_count, wake_detected, vad_speech, clean_samples);
        }

        if (wake_detected) {
            ESP_LOGI(TAG, "*** WAKE WORD DETECTED ***");
            voice_on_wake();
        }

        if (s_state == VOICE_STATE_LISTENING && clean_audio && clean_samples > 0
            && s_ws && esp_websocket_client_is_connected(s_ws)) {
            voice_ws_send_binary(clean_audio, clean_samples * sizeof(int16_t));
            ui_notes_write_audio(clean_audio, clean_samples);
        }
    }

    ESP_LOGI(TAG, "AFE detect task exiting");
    s_afe_detect_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t _voice_start_always_listening_impl(void)
{
    if (s_afe_enabled) {
        ESP_LOGW(TAG, "AFE already running");
        return ESP_OK;
    }

    esp_err_t ret = tab5_afe_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AFE init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_afe_enabled = true;
    s_afe_listening = true;

    s_mic_running = true;
    s_mic_task = NULL;
    BaseType_t mic_ret = xTaskCreatePinnedToCoreWithCaps(
        mic_capture_task, "afe_mic", MIC_TASK_STACK_SIZE,
        NULL, MIC_TASK_PRIORITY, &s_mic_task, MIC_TASK_CORE,
        MALLOC_CAP_SPIRAM);
    if (mic_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE mic task (ret=%d)", mic_ret);
        tab5_afe_deinit();
        s_afe_enabled = false;
        s_afe_listening = false;
        s_mic_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AFE mic task created on core %d", MIC_TASK_CORE);

    vTaskDelay(pdMS_TO_TICKS(500));

    BaseType_t det_ret = xTaskCreatePinnedToCoreWithCaps(
        afe_detect_task, "afe_detect", 8192, NULL,
        MIC_TASK_PRIORITY, &s_afe_detect_task, MIC_TASK_CORE,
        MALLOC_CAP_SPIRAM);
    if (det_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE detect task");
    }

    ESP_LOGI(TAG, "Always-listening mode started (wake word: %s)",
             tab5_afe_wake_word_name());
    return ESP_OK;
}

esp_err_t _voice_stop_always_listening_impl(void)
{
    if (!s_afe_enabled) return ESP_OK;
    s_afe_listening = false;
    s_afe_enabled = false;
    vTaskDelay(pdMS_TO_TICKS(100));
    tab5_afe_deinit();
    ESP_LOGI(TAG, "Always-listening mode stopped");
    return ESP_OK;
}
#endif  /* WAKE_WORD_PARKED */
