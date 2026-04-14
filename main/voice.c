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
 * Uses esp_transport_ws (same pattern as touch_ws.c) for the WebSocket layer.
 * Mic capture task is pinned to core 1 to keep core 0 free for UI/LVGL.
 */

#include "voice.h"
#include "afe.h"
#include "ui_notes.h"
#include "audio.h"
#include "config.h"
#include "settings.h"
#include "ui_core.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_crt_bundle.h"
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
// Adjust these if the slot verification test shows a different layout.
// TDM slot mapping — verified from SLOT_DIAG: slot 1 is near-silent (AEC ref from DAC)
// Slots 0 and 2 have similar RMS (both are mics). Slot 3 has low non-zero (noise/unused).
#define MIC_TDM_MIC1_OFF       0    // MIC1 at slot 0 (high RMS ~200)
#define MIC_TDM_REF_OFF        1    // AEC reference at slot 1 (near-zero RMS ~6 when speaker silent)
#define MIC_TDM_MIC2_OFF       2    // MIC2 at slot 2 (high RMS ~200)

// AFE feed: channels match the format string passed to afe_config_init().
// "MR" = 1 mic + 1 ref = 2 channels. "MMR" = 2 mic + 1 ref = 3 channels.
#define AFE_FEED_CHANNELS      2  /* "MR" mode — 1 mic + 1 ref */
#define AFE_FEED_MAX_SAMPLES   2048  /* max buffer — actual size from get_feed_chunksize() */
#define MIC_TDM_SAMPLES        (MIC_48K_FRAMES * MIC_TDM_CHANNELS)
// Downsample ratio: 48kHz -> 16kHz = 3:1
#define DOWNSAMPLE_RATIO       (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)
// Upsample ratio: 16kHz -> 48kHz = 1:3 (mirror of downsample)
#define UPSAMPLE_RATIO         (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)

// Reconnect parameters
#define VOICE_RECONNECT_BASE_MS  2000
#define VOICE_RECONNECT_MAX_MS   15000
#define VOICE_CONNECT_TIMEOUT_MS 5000
// Keep-alive ping interval: prevents TCP idle timeout during long LLM inference
#define VOICE_KEEPALIVE_MS       15000
// Dragon response timeout: auto-cancel if no STT/LLM response after stop
#define VOICE_RESPONSE_TIMEOUT_MS 35000  /* Must exceed Dragon's 30s TTS timeout */

// Mic capture task — needs room for AFE feed buffer (960 int16 = 1.9KB) + TDM diagnostics
#define MIC_TASK_STACK_SIZE    8192  /* Increased for AFE 3-channel extraction */
#define MIC_TASK_PRIORITY      5
#define MIC_TASK_CORE          1

// WS receive task
#define WS_TASK_STACK_SIZE     8192
#define WS_TASK_PRIORITY       4
#define WS_TASK_CORE           1

// Playback drain task — higher priority than WS receive so I2S stays fed
#define PLAY_TASK_STACK_SIZE   4096
#define PLAY_TASK_PRIORITY     6
#define PLAY_TASK_CORE         1

// Max transcript length
#define MAX_TRANSCRIPT_LEN     512

// Dictation mode constants
#define DICTATION_SILENCE_THRESHOLD  800   /* RMS below this = silence (Tab5 mic has higher noise floor) */
#define DICTATION_SILENCE_FRAMES     25    /* 25 × 20ms = 500ms silence before segment (was 800ms) */
#define DICTATION_AUTO_STOP_FRAMES   250   /* 250 × 20ms = 5s total silence → auto-stop */
#define DICTATION_TEXT_SIZE          65536  /* 64KB for accumulated transcript (~2hrs, PSRAM) */
#define MAX_RECORD_FRAMES_ASK        1500  /* 30s limit for Ask mode only */

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static voice_state_t     s_state = VOICE_STATE_IDLE;
static voice_state_cb_t  s_state_cb = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;

// WebSocket transport
static esp_transport_handle_t s_ws = NULL;
static volatile bool s_ws_connected = false;
static volatile bool s_stop_flag = false;
static SemaphoreHandle_t s_ws_mutex = NULL;  // protects s_ws send/recv from concurrent access

// Dragon connection info
static char     s_dragon_host[64] = {0};
static uint16_t s_dragon_port = TAB5_VOICE_PORT;

// Mic capture task
static TaskHandle_t  s_mic_task = NULL;
static volatile bool s_mic_running = false;

// WS receive task
static TaskHandle_t  s_ws_task = NULL;
static volatile bool s_ws_running = false;

// Playback drain task — pulls from ring buffer, blocks on i2s_channel_write
static TaskHandle_t  s_play_task = NULL;
static volatile bool s_play_running = false;
static volatile bool s_tts_done = false;     // set by tts_end, drain task transitions to READY
static SemaphoreHandle_t s_play_sem = NULL;  // signalled when data is written to ring buf

// Playback ring buffer (smooths network jitter) — allocated in PSRAM
static int16_t  *s_play_buf = NULL;
static size_t   s_play_capacity = 0;  // capacity in samples
static size_t   s_play_wr = 0;  // write index (samples)
static size_t   s_play_rd = 0;  // read index (samples)
static size_t   s_play_count = 0; // samples available
static SemaphoreHandle_t s_play_mutex = NULL;
#define PLAY_BUF_CAPACITY  s_play_capacity

// Last transcript from Dragon STT
static char s_transcript[MAX_TRANSCRIPT_LEN] = {0};
// Separated buffers: STT = what user said, LLM = what Tinker says
static char s_stt_text[MAX_TRANSCRIPT_LEN] = {0};
static char s_llm_text[MAX_TRANSCRIPT_LEN] = {0};

// Dictation mode
static voice_mode_t s_voice_mode = VOICE_MODE_ASK;
static char *s_dictation_text = NULL;  /* PSRAM-allocated, DICTATION_TEXT_SIZE */
static volatile float s_current_rms = 0.0f;  /* live mic RMS for UI waveform */
static char s_dictation_title[128] = {0};
static char s_dictation_summary[512] = {0};

// AFE (always-listening) mode
static volatile bool s_afe_enabled = false;     // true when AFE is init'd
static volatile bool s_afe_listening = false;    // true when always-listening active
static TaskHandle_t  s_afe_detect_task = NULL;   // fetch task handle

static bool s_initialized = false;
static volatile bool s_connect_in_progress = false;

// Reconnect watchdog
static TaskHandle_t  s_reconnect_task = NULL;
static volatile bool s_reconnect_enabled = false;
#define RECONNECT_CHECK_MS     5000   /* check connection every 5s */
#define RECONNECT_BACKOFF_MS   10000  /* wait 10s between reconnect attempts */
#define IDLE_PING_INTERVAL_MS  10000  /* ping every 10s when idle/ready */

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void voice_set_state(voice_state_t new_state, const char *detail);
static void mic_capture_task(void *arg);
static void ws_receive_task(void *arg);
static void playback_drain_task(void *arg);
static void handle_text_message(const char *data, int len);
static void playback_buf_reset(void);
static size_t playback_buf_write(const int16_t *data, size_t samples);
static size_t playback_buf_read(int16_t *data, size_t max_samples);
static esp_err_t ws_send_text(const char *msg);
static esp_err_t ws_send_binary(const void *data, size_t len);

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

    // Wake the playback drain task
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

// Activity timestamp for response timeout — shared between stop_listening and ws_receive_task
static volatile int64_t s_last_activity_us = 0;

void voice_reset_activity_timestamp(void)
{
    s_last_activity_us = esp_timer_get_time();
}

// ---------------------------------------------------------------------------
// WebSocket helpers
// ---------------------------------------------------------------------------
static esp_err_t ws_send_text(const char *msg)
{
    if (!s_ws || !s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    // Copy to mutable buffer — ESP-IDF WS transport masks frames in-place,
    // so passing a string literal (in flash/rodata) causes a store fault.
    size_t len = strlen(msg);
    char *buf = malloc(len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "WS send: OOM for %zu bytes", len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, msg, len + 1);

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    int ret;
    if (s_ws && s_ws_connected) {
        ret = esp_transport_ws_send_raw(s_ws,
            WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
            buf, len, 1000);
    } else {
        ret = -1;  // disconnected between check and mutex acquire
    }
    xSemaphoreGive(s_ws_mutex);

    free(buf);

    if (ret < 0) {
        ESP_LOGE(TAG, "WS text send failed: %d — marking disconnected", ret);
        s_ws_connected = false;
        playback_buf_reset();
        tab5_audio_speaker_enable(false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_send_binary(const void *data, size_t len)
{
    if (!s_ws || !s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    // Copy to mutable heap buffer — ESP-IDF WS transport masks in-place.
    // Callers may pass stack buffers (OK) but this future-proofs against
    // any caller passing const/flash data.
    void *buf = malloc(len);
    if (!buf) {
        ESP_LOGE(TAG, "WS binary send: OOM for %zu bytes", len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, data, len);

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    int ret;
    if (s_ws && s_ws_connected) {
        ret = esp_transport_ws_send_raw(s_ws,
            WS_TRANSPORT_OPCODES_BINARY | WS_TRANSPORT_OPCODES_FIN,
            buf, len, 1000);
    } else {
        ret = -1;  // disconnected between check and mutex acquire
    }
    xSemaphoreGive(s_ws_mutex);

    free(buf);

    if (ret < 0) {
        ESP_LOGW(TAG, "WS binary send failed — marking disconnected");
        s_ws_connected = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Device registration — sends register JSON as first frame after WS connect
// ---------------------------------------------------------------------------
static esp_err_t ws_send_register(void)
{
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

    /* Session resume: send last session_id if we have one */
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

    esp_err_t err = ws_send_text(json);
    cJSON_free(json);
    return err;
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
        /* Dictation mode: partial transcript from a segment */
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
            /* Update UI with running transcript */
            voice_set_state(VOICE_STATE_LISTENING, s_dictation_text);
        }
    } else if (strcmp(type_str, "stt") == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            if (s_voice_mode == VOICE_MODE_DICTATE) {
                /* Dictation: final combined transcript — store in stt_text,
                 * transition to READY (no LLM/TTS phase) */
                if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                strncpy(s_stt_text, text->valuestring, MAX_TRANSCRIPT_LEN - 1);
                s_stt_text[MAX_TRANSCRIPT_LEN - 1] = '\0';
                if (s_state_mutex) xSemaphoreGive(s_state_mutex);
                ESP_LOGI(TAG, "Dictation complete: %u chars", (unsigned)strlen(text->valuestring));
                voice_set_state(VOICE_STATE_READY, "dictation_done");
            } else {
                /* Ask mode: STT result, proceed to LLM */
                if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                strncpy(s_stt_text, text->valuestring, MAX_TRANSCRIPT_LEN - 1);
                s_stt_text[MAX_TRANSCRIPT_LEN - 1] = '\0';
                strncpy(s_transcript, text->valuestring, MAX_TRANSCRIPT_LEN - 1);
                s_transcript[MAX_TRANSCRIPT_LEN - 1] = '\0';
                s_llm_text[0] = '\0';
                if (s_state_mutex) xSemaphoreGive(s_state_mutex);
                ESP_LOGI(TAG, "STT: \"%s\"", s_stt_text);
                voice_set_state(VOICE_STATE_PROCESSING, s_stt_text);
            }
        }
    } else if (strcmp(type_str, "tts_start") == 0) {
        // Dragon is about to send TTS audio
        ESP_LOGI(TAG, "TTS start — preparing playback");
        tab5_audio_speaker_enable(true);
        playback_buf_reset();
        voice_set_state(VOICE_STATE_SPEAKING, NULL);
    } else if (strcmp(type_str, "tts_end") == 0) {
        // TTS audio stream complete — drain task will transition to READY
        // when the playback buffer empties. Don't block the WS receive task.
        ESP_LOGI(TAG, "TTS end — drain task will finish playback");
        s_tts_done = true;
        if (s_play_sem) xSemaphoreGive(s_play_sem);  // wake drain task
    } else if (strcmp(type_str, "llm") == 0) {
        // Streaming LLM response token — append to LLM buffer
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            size_t llm_len = strlen(s_llm_text);
            size_t add_len = strlen(text->valuestring);
            if (llm_len + add_len < MAX_TRANSCRIPT_LEN - 1) {
                strcat(s_llm_text, text->valuestring);
            }
            // Also keep s_transcript as full combined text for backward compat
            size_t cur_len = strlen(s_transcript);
            if (cur_len + add_len < MAX_TRANSCRIPT_LEN - 1) {
                strcat(s_transcript, text->valuestring);
            }
            if (s_state_mutex) xSemaphoreGive(s_state_mutex);
            ESP_LOGD(TAG, "LLM token: \"%s\"", text->valuestring);
            // Update UI with streaming LLM response
            voice_set_state(VOICE_STATE_PROCESSING, s_llm_text);
        }
    } else if (strcmp(type_str, "error") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        const char *err_src = cJSON_IsString(msg) ? msg->valuestring : "unknown";
        ESP_LOGE(TAG, "Dragon error: %s", err_src);
        char err_buf[128];
        strncpy(err_buf, err_src, sizeof(err_buf) - 1);
        err_buf[sizeof(err_buf) - 1] = '\0';
        // Stop any playback in progress
        playback_buf_reset();
        tab5_audio_speaker_enable(false);
        // Go to READY if WS still alive (transient error), IDLE if disconnected
        voice_set_state(s_ws_connected ? VOICE_STATE_READY : VOICE_STATE_IDLE, err_buf);
    } else if (strcmp(type_str, "session_start") == 0) {
        /* Store session_id in NVS for resume on reconnect */
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

        // Dragon pipeline is fully initialized — NOW safe to record.
        if (s_state == VOICE_STATE_CONNECTING) {
            voice_set_state(VOICE_STATE_READY, NULL);
            ESP_LOGI(TAG, "Connected to Dragon voice server");

            /* ALWAYS restore voice mode + model from NVS on reconnect.
             * Dragon resets to local defaults on register — Tab5 must resend
             * its saved config so Dragon matches the user's preference.
             * Even mode=0 needs resend if user selected a non-default local model. */
            uint8_t saved_mode = tab5_settings_get_voice_mode();
            char saved_model[64] = {0};
            tab5_settings_get_llm_model(saved_model, sizeof(saved_model));
            ESP_LOGI(TAG, "Restoring voice_mode=%d model='%s' on reconnect",
                     saved_mode, saved_model[0] ? saved_model : "(default)");
            voice_send_config_update((int)saved_mode,
                                     saved_model[0] ? saved_model : NULL);
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
        /* C6: Dragon note_created is the auto-save from dictation.
         * Tab5 already saves dictation text in ui_voice (dictation_done handler),
         * so we DON'T duplicate here. Just log it for awareness. */
    } else if (strcmp(type_str, "llm_done") == 0) {
        cJSON *ms = cJSON_GetObjectItem(root, "llm_ms");
        ESP_LOGI(TAG, "LLM done (%.0fms)", cJSON_IsNumber(ms) ? ms->valuedouble : 0.0);
    } else if (strcmp(type_str, "pong") == 0) {
        /* Dragon keepalive response — no action needed */
    } else if (strcmp(type_str, "config_update") == 0) {
        /* Check for error (auto-fallback notification from Dragon) */
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsString(error) && error->valuestring[0]) {
            ESP_LOGW(TAG, "Config update error from Dragon: %s", error->valuestring);
            /* Auto-disable: revert to local mode */
            tab5_settings_set_voice_mode(0);
            voice_set_state(VOICE_STATE_READY, error->valuestring);
        }
        /* Normal ACK: persist voice_mode (may be at top level or inside config) */
        cJSON *vmode = cJSON_GetObjectItem(root, "voice_mode");
        if (!cJSON_IsNumber(vmode)) {
            /* Try inside "config" object (Dragon success ACK nests it there) */
            cJSON *config_obj = cJSON_GetObjectItem(root, "config");
            if (config_obj) {
                vmode = cJSON_GetObjectItem(config_obj, "voice_mode");
            }
        }
        if (cJSON_IsNumber(vmode)) {
            uint8_t mode = (uint8_t)vmode->valueint;
            tab5_settings_set_voice_mode(mode);
            ESP_LOGI(TAG, "Config update: voice_mode=%d (persisted)", mode);
            /* Refresh home badge from LVGL thread */
            extern void ui_home_refresh_mode_badge(void);
            lv_async_call((void(*)(void*))ui_home_refresh_mode_badge, NULL);
        }
        /* Backward compat: cloud_mode bool */
        cJSON *config = cJSON_GetObjectItem(root, "config");
        if (config) {
            cJSON *cloud = cJSON_GetObjectItem(config, "cloud_mode");
            if (cJSON_IsBool(cloud)) {
                bool is_cloud = cJSON_IsTrue(cloud);
                if (!cJSON_IsNumber(vmode)) {
                    tab5_settings_set_voice_mode(is_cloud ? 1 : 0);
                }
            }
        }
    } else if (strcmp(type_str, "tool_call") == 0) {
        /* Dragon is invoking a tool — show status on voice overlay */
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
        /* Tool execution complete — log result, LLM response will follow */
        cJSON *tool = cJSON_GetObjectItem(root, "tool");
        cJSON *exec_ms = cJSON_GetObjectItem(root, "execution_ms");
        const char *tool_name = cJSON_IsString(tool) ? tool->valuestring : "unknown";
        double ms = cJSON_IsNumber(exec_ms) ? exec_ms->valuedouble : 0.0;
        ESP_LOGI(TAG, "Tool result: %s (%.0fms)", tool_name, ms);
        /* Clear tool status — LLM will continue generating shortly */
        voice_set_state(VOICE_STATE_PROCESSING, "Thinking...");
    } else {
        ESP_LOGW(TAG, "Unknown message type: %s (full: %.*s)", type_str, len, data);
    }

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Mic capture task — reads 4-ch TDM at 48kHz, extracts MIC1, downsamples
// to 16kHz mono, sends via WS to Dragon
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// AFE detect task — fetches processed audio from ESP-SR AFE, checks wake word
// ---------------------------------------------------------------------------
static void afe_detect_task(void *arg)
{
    ESP_LOGI(TAG, "AFE detect task started (core %d), afe_listening=%d", xPortGetCoreID(), s_afe_listening);
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

        // If we're in LISTENING state (after wake or PTT), stream clean audio to Dragon
        if (s_state == VOICE_STATE_LISTENING && clean_audio && clean_samples > 0 && s_ws_connected) {
            ws_send_binary(clean_audio, clean_samples * sizeof(int16_t));

            // Also write to SD card WAV (if recording active)
            ui_notes_write_audio(clean_audio, clean_samples);
        }
    }

    ESP_LOGI(TAG, "AFE detect task exiting");
    s_afe_detect_task = NULL;
    vTaskSuspend(NULL);
}

// ---------------------------------------------------------------------------
// Wake word handler — called from AFE detect task
// ---------------------------------------------------------------------------
void voice_on_wake(void)
{
    voice_state_t cur = s_state;

    if (cur == VOICE_STATE_IDLE || cur == VOICE_STATE_READY) {
        // Wake from idle — start listening
        ESP_LOGI(TAG, "Wake → LISTENING");
        voice_set_state(VOICE_STATE_LISTENING, NULL);

        // Send start command to Dragon
        if (s_ws_connected) {
            ws_send_text("{\"type\":\"start\",\"mode\":\"ask\",\"wake\":true}");
        }
    } else if (cur == VOICE_STATE_SPEAKING) {
        // Barge-in — interrupt TTS playback
        ESP_LOGI(TAG, "Wake → BARGE-IN (interrupting TTS)");

        // Flush playback buffer
        if (s_play_mutex) {
            xSemaphoreTake(s_play_mutex, portMAX_DELAY);
            s_play_wr = 0;
            s_play_rd = 0;
            s_play_count = 0;
            xSemaphoreGive(s_play_mutex);
        }

        // Cancel current processing on Dragon
        if (s_ws_connected) {
            ws_send_text("{\"type\":\"cancel\"}");
        }

        // Start new listening session
        voice_set_state(VOICE_STATE_LISTENING, NULL);
        if (s_ws_connected) {
            ws_send_text("{\"type\":\"start\",\"mode\":\"ask\",\"wake\":true}");
        }
    }
    // If already LISTENING or PROCESSING, ignore wake word
}

// ---------------------------------------------------------------------------
// Mic capture task — feeds AFE or sends raw audio (PTT fallback)
// ---------------------------------------------------------------------------
static void mic_capture_task(void *arg)
{
    ESP_LOGI(TAG, "Mic capture task started (core %d)", xPortGetCoreID());

    // TDM buffer in PSRAM — saves 7.7KB internal RAM for SDIO DMA (#18)
    int16_t *tdm_buf = (int16_t *)heap_caps_malloc(
        MIC_TDM_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono_buf = (int16_t *)heap_caps_malloc(
        VOICE_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // AFE feed buffer — size from get_feed_chunksize()
    // Allocate in PSRAM with cache-line alignment for esp_cache_msync
    int16_t *afe_buf = NULL;
    int afe_chunksize = 0;
    int afe_buf_pos = 0;
    if (s_afe_enabled) {
        afe_chunksize = tab5_afe_get_feed_chunksize();
        if (afe_chunksize <= 0) afe_chunksize = 512;
        // Align to 64-byte cache line for PSRAM coherency
        size_t alloc_size = ((afe_chunksize * sizeof(int16_t) + 63) / 64) * 64;
        afe_buf = (int16_t *)heap_caps_aligned_alloc(64, alloc_size,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "AFE feed buffer: %d samples (%zu bytes), buf=%p",
                 afe_chunksize, alloc_size, afe_buf);
    }
    // Temp extraction buffer — heap allocated to avoid PSRAM stack cache issues
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
        /* vTaskSuspend instead of vTaskDelete — P4 TLSP crash workaround (#20) */
    vTaskSuspend(NULL);
        return;
    }

    s_mic_running = true;
    int frames_sent = 0;

    /* Dictation VAD state (only used when s_voice_mode == VOICE_MODE_DICTATE) */
    int silence_frames = 0;
    int total_silence_frames = 0;
    bool had_speech = false;

    /* Adaptive VAD: calibrate threshold from first 500ms of ambient noise */
    #define CALIBRATION_FRAMES 25
    float noise_sum = 0;
    int cal_count = 0;
    float dictation_threshold = DICTATION_SILENCE_THRESHOLD;

    while (s_mic_running && (s_ws_connected || s_voice_mode == VOICE_MODE_DICTATE || s_afe_enabled)) {
        // Read 4-channel TDM data at 48kHz
        esp_err_t err = tab5_mic_read(tdm_buf, MIC_TDM_SAMPLES, 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Mic read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Log per-slot RMS for first 5 chunks to find the real mic channel
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
            // Also dump first 8 raw TDM frames as hex
            if (frames_sent == 0) {
                ESP_LOGI(TAG, "TDM raw [0..31]: %04X %04X %04X %04X | %04X %04X %04X %04X | %04X %04X %04X %04X | %04X %04X %04X %04X | %04X %04X %04X %04X | %04X %04X %04X %04X | %04X %04X %04X %04X | %04X %04X %04X %04X",
                    (uint16_t)tdm_buf[0],  (uint16_t)tdm_buf[1],  (uint16_t)tdm_buf[2],  (uint16_t)tdm_buf[3],
                    (uint16_t)tdm_buf[4],  (uint16_t)tdm_buf[5],  (uint16_t)tdm_buf[6],  (uint16_t)tdm_buf[7],
                    (uint16_t)tdm_buf[8],  (uint16_t)tdm_buf[9],  (uint16_t)tdm_buf[10], (uint16_t)tdm_buf[11],
                    (uint16_t)tdm_buf[12], (uint16_t)tdm_buf[13], (uint16_t)tdm_buf[14], (uint16_t)tdm_buf[15],
                    (uint16_t)tdm_buf[16], (uint16_t)tdm_buf[17], (uint16_t)tdm_buf[18], (uint16_t)tdm_buf[19],
                    (uint16_t)tdm_buf[20], (uint16_t)tdm_buf[21], (uint16_t)tdm_buf[22], (uint16_t)tdm_buf[23],
                    (uint16_t)tdm_buf[24], (uint16_t)tdm_buf[25], (uint16_t)tdm_buf[26], (uint16_t)tdm_buf[27],
                    (uint16_t)tdm_buf[28], (uint16_t)tdm_buf[29], (uint16_t)tdm_buf[30], (uint16_t)tdm_buf[31]);
            }
        }

        // Extract and downsample 48kHz -> 16kHz with box filter.
        int out_idx = 0;

        if (s_afe_enabled && afe_buf) {
            // AFE mode: extract mic1 + ref interleaved into heap-allocated afe_tmp
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
            // Accumulate into AFE feed buffer until we reach afe_chunksize
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
            // Legacy PTT mode: single channel only
            for (int i = 0; i + DOWNSAMPLE_RATIO - 1 < MIC_48K_FRAMES && out_idx < VOICE_CHUNK_SAMPLES; i += DOWNSAMPLE_RATIO) {
                int32_t sum = 0;
                for (int j = 0; j < DOWNSAMPLE_RATIO; j++) {
                    sum += tdm_buf[(i + j) * MIC_TDM_CHANNELS + MIC_TDM_MIC1_OFF];
                }
                mono_buf[out_idx++] = (int16_t)(sum / DOWNSAMPLE_RATIO);
            }
        }

        // Log audio levels every 50 chunks (~1 second) for debug
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

        // In AFE mode, the detect task handles WS streaming of clean audio.
        // Mic task only feeds the AFE and writes raw audio to SD.
        if (!s_afe_enabled) {
            // Always write to SD card WAV (if recording is active)
            ui_notes_write_audio(mono_buf, out_idx);

            // Stream to Dragon via WebSocket (if connected)
            size_t send_bytes = out_idx * sizeof(int16_t);
            if (s_ws_connected) {
                err = ws_send_binary(mono_buf, send_bytes);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "WS send failed — continuing SD recording");
                    /* Don't break — keep recording to SD even if WS dies */
                    if (s_voice_mode == VOICE_MODE_ASK) break;
                }
            }
        }
        frames_sent++;

        // Duration limit: 30s for Ask mode, unlimited for Dictate and AFE
        if (s_voice_mode == VOICE_MODE_ASK && !s_afe_enabled && frames_sent >= MAX_RECORD_FRAMES_ASK) {
            ESP_LOGI(TAG, "Max recording duration reached (%ds)",
                     MAX_RECORD_FRAMES_ASK * TAB5_VOICE_CHUNK_MS / 1000);
            voice_set_state(VOICE_STATE_LISTENING, "max_duration");
            break;
        }

        // Dictation VAD: detect pauses, send segment markers, auto-stop
        // Skip in AFE mode — AFE handles VAD internally
        if (s_voice_mode == VOICE_MODE_DICTATE && !s_afe_enabled) {
            int64_t sqsum = 0;
            for (int k = 0; k < out_idx; k++) {
                sqsum += (int64_t)mono_buf[k] * mono_buf[k];
            }
            float rms = sqrtf((float)(sqsum / (out_idx > 0 ? out_idx : 1)));
            s_current_rms = rms;  /* expose for UI waveform */

            /* Adaptive calibration: first 500ms measures ambient noise */
            if (cal_count < CALIBRATION_FRAMES) {
                noise_sum += rms;
                cal_count++;
                if (cal_count == CALIBRATION_FRAMES) {
                    float ambient = noise_sum / CALIBRATION_FRAMES;
                    dictation_threshold = fmaxf(400.0f, fminf(1500.0f, ambient * 2.0f));
                    ESP_LOGI(TAG, "VAD calibrated: ambient=%.0f, threshold=%.0f",
                             ambient, dictation_threshold);
                }
                continue;  /* skip VAD during calibration */
            }

            if (rms < dictation_threshold) {
                silence_frames++;
                total_silence_frames++;
            } else {
                if (had_speech && silence_frames >= DICTATION_SILENCE_FRAMES) {
                    ESP_LOGI(TAG, "Dictation: pause (%dms), sending segment",
                             silence_frames * TAB5_VOICE_CHUNK_MS);
                    ws_send_text("{\"type\":\"segment\"}");
                }
                silence_frames = 0;
                total_silence_frames = 0;
                had_speech = true;
            }

            // Auto-stop: 5s of continuous silence after speech
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

    /* Auto-stop: if we broke out of the loop due to dictation silence,
     * send stop to Dragon and transition to PROCESSING (same as manual stop) */
    if (s_voice_mode == VOICE_MODE_DICTATE && had_speech
        && total_silence_frames >= DICTATION_AUTO_STOP_FRAMES && s_ws_connected) {
        ws_send_text("{\"type\":\"stop\"}");
        voice_set_state(VOICE_STATE_PROCESSING, NULL);
    }

    ESP_LOGI(TAG, "Mic capture task exiting");
    s_mic_task = NULL;
    vTaskSuspend(NULL);
}

// ---------------------------------------------------------------------------
// Upsample 16kHz -> 48kHz with linear interpolation (mirror of downsample)
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

// ---------------------------------------------------------------------------
// Playback drain task — pulls samples from ring buffer, blocks on I2S write.
// Runs at higher priority than WS receive so I2S DMA stays fed.
// Producer (WS receive) fills the ring buffer; this task drains it at the
// hardware sample rate (48kHz), blocking on i2s_channel_write.
// ---------------------------------------------------------------------------
static void playback_drain_task(void *arg)
{
    ESP_LOGI(TAG, "Playback drain task started (core %d, prio %d)",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    // Chunk size: 20ms at 48kHz = 960 samples.  Matches VOICE_CHUNK_SAMPLES.
    int16_t *chunk = (int16_t *)heap_caps_malloc(
        VOICE_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        ESP_LOGE(TAG, "Playback drain: failed to allocate chunk buffer");
        s_play_task = NULL;
        vTaskSuspend(NULL);
        return;
    }

    s_play_running = true;

    while (s_play_running) {
        // Block until the producer signals new data (or timeout for state check)
        xSemaphoreTake(s_play_sem, pdMS_TO_TICKS(50));

        // Drain loop: keep pulling from ring buffer until empty
        while (s_play_running) {
            size_t avail = playback_buf_read(chunk, VOICE_CHUNK_SAMPLES);
            if (avail == 0) {
                // Buffer empty — if TTS is done, transition to READY
                if (s_tts_done) {
                    s_tts_done = false;
                    ESP_LOGI(TAG, "Playback drain complete — transitioning to READY");
                    tab5_audio_speaker_enable(false);
                    voice_set_state(VOICE_STATE_READY, NULL);
                }
                break;
            }

            // This blocks until I2S DMA has room — the backpressure we need.
            tab5_audio_play_raw(chunk, avail);
        }
    }

    heap_caps_free(chunk);
    ESP_LOGI(TAG, "Playback drain task exiting");
    s_play_task = NULL;
    vTaskSuspend(NULL);
}

// ---------------------------------------------------------------------------
// WebSocket receive task — reads from Dragon, handles text + binary frames
// ---------------------------------------------------------------------------
static void ws_receive_task(void *arg)
{
    ESP_LOGI(TAG, "WS receive task started (core %d)", xPortGetCoreID());

    // Receive buffer — Dragon sends TTS audio in 4096-byte chunks
    char rx_buf[4100];

    // Upsample buffer in PSRAM: max 2048 input samples × 3 = 6144 output samples (12KB)
    const size_t upsample_cap = 2048 * UPSAMPLE_RATIO;
    int16_t *upsample_buf = (int16_t *)heap_caps_malloc(
        upsample_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!upsample_buf) {
        ESP_LOGE(TAG, "Failed to allocate upsample buffer");
        s_ws_task = NULL;
        vTaskSuspend(NULL);
        return;
    }

    s_ws_running = true;
    s_last_activity_us = esp_timer_get_time();

    while (s_ws_connected && !s_stop_flag) {
        /* Yield every iteration to prevent starving lower-priority tasks on core 1 */
        vTaskDelay(pdMS_TO_TICKS(1));

        int poll = esp_transport_poll_read(s_ws, 100);
        if (poll < 0) {
            ESP_LOGW(TAG, "WS poll error, connection lost");
            s_ws_connected = false;
            break;
        }
        if (poll == 0) {
            // No data available — yield to prevent tight loop if poll returns immediately
            // (can happen when TCP socket is in degraded state, starving CPU1 idle task WDT)
            vTaskDelay(pdMS_TO_TICKS(10));
            int64_t now_us = esp_timer_get_time();
            static int64_t s_last_keepalive_us = 0;

            if (s_state == VOICE_STATE_PROCESSING || s_state == VOICE_STATE_SPEAKING) {
                int64_t elapsed_us = now_us - s_last_activity_us;

                // Send keep-alive heartbeat to prevent TCP idle timeout.
                if ((now_us - s_last_keepalive_us) > (int64_t)VOICE_KEEPALIVE_MS * 1000) {
                    esp_err_t ping_err = ws_send_text("{\"type\":\"ping\"}");
                    s_last_keepalive_us = now_us;
                    if (ping_err != ESP_OK) {
                        ESP_LOGW(TAG, "Keepalive ping failed — connection dead");
                        s_ws_connected = false;
                        break;
                    }
                }

                /* Response timeout: if Dragon hasn't sent anything, give up.
                 * Local mode (voice_mode=0): no timeout — Ollama + tool-calling can take 2+ minutes.
                 * Cloud mode (voice_mode 1/2): 35s timeout — cloud LLM is fast, timeout means real failure. */
                uint8_t cur_voice_mode = tab5_settings_get_voice_mode();
                int64_t timeout_us = (cur_voice_mode == 0)
                    ? (int64_t)300000 * 1000   /* 5 minutes for local (tool-calling + slow LLM) */
                    : (int64_t)VOICE_RESPONSE_TIMEOUT_MS * 1000;  /* 35s for cloud */
                if (elapsed_us > timeout_us) {
                    ESP_LOGW(TAG, "Dragon response timeout (%llds, mode=%d) — cancelling",
                             (long long)(timeout_us / 1000000), cur_voice_mode);
                    ws_send_text("{\"type\":\"cancel\"}");
                    playback_buf_reset();
                    tab5_audio_speaker_enable(false);
                    voice_set_state(VOICE_STATE_READY, "timeout");
                    s_last_activity_us = now_us;
                }
            } else if (s_state == VOICE_STATE_READY) {
                /* N1: Idle keepalive — detect dead TCP connections while READY.
                 * If Dragon restarts, TCP stays open but WS is dead. Periodic
                 * ping detects this and triggers reconnect via watchdog. */
                if ((now_us - s_last_keepalive_us) > (int64_t)IDLE_PING_INTERVAL_MS * 1000) {
                    esp_err_t ping_err = ws_send_text("{\"type\":\"ping\"}");
                    s_last_keepalive_us = now_us;
                    if (ping_err != ESP_OK) {
                        ESP_LOGW(TAG, "Idle ping failed — connection dead");
                        s_ws_connected = false;
                        break;
                    }
                }
            }
            continue;
        }

        // Data available — read it (mutex protects WS framing state shared with send)
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        int len = esp_transport_read(s_ws, rx_buf, sizeof(rx_buf), 1000);
        int opcode = (len > 0) ? esp_transport_ws_get_read_opcode(s_ws) : -1;
        xSemaphoreGive(s_ws_mutex);

        if (len <= 0) {
            ESP_LOGW(TAG, "WS read error (%d), disconnecting", len);
            s_ws_connected = false;
            break;
        }
        s_last_activity_us = esp_timer_get_time();

        if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
            // JSON text message from Dragon
            rx_buf[len] = '\0';
            ESP_LOGI(TAG, "WS recv text (%d bytes): %.*s", len,
                     len > 200 ? 200 : len, rx_buf);
            handle_text_message(rx_buf, len);
        } else if (opcode == WS_TRANSPORT_OPCODES_BINARY) {
            // PCM audio data from Dragon
            if (s_state == VOICE_STATE_SPEAKING ||
                s_state == VOICE_STATE_PROCESSING) {
                // If we get binary before tts_start, switch to speaking
                if (s_state == VOICE_STATE_PROCESSING) {
                    playback_buf_reset();
                    voice_set_state(VOICE_STATE_SPEAKING, NULL);
                }

                // Upsample 16kHz -> 48kHz before writing to playback buffer.
                // Dragon sends PCM int16 mono at 16kHz; I2S TX runs at 48kHz.
                // Playback drain task handles I2S writes from the ring buffer.
                size_t in_samples = len / sizeof(int16_t);
                size_t out_samples = upsample_16k_to_48k(
                    (const int16_t *)rx_buf, in_samples,
                    upsample_buf, upsample_cap);
                playback_buf_write(upsample_buf, out_samples);
            }
        } else if (opcode == WS_TRANSPORT_OPCODES_PING) {
            // Respond to ping with pong — rx_buf is stack (mutable), safe for masking.
            // But masking mutates it, so copy to heap to keep rx_buf clean for reuse.
            if (s_ws && s_ws_connected) {
                char *pong_buf = malloc(len);
                if (pong_buf) {
                    memcpy(pong_buf, rx_buf, len);
                    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
                    esp_transport_ws_send_raw(s_ws,
                        WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN,
                        pong_buf, len, 100);
                    xSemaphoreGive(s_ws_mutex);
                    free(pong_buf);
                }
            }
        } else if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
            ESP_LOGI(TAG, "WS close frame received");
            s_ws_connected = false;
            break;
        }
    }

    s_ws_running = false;
    s_ws_task = NULL;
    heap_caps_free(upsample_buf);

    // Clean up transport on unexpected disconnect.
    // Take WS mutex to ensure mic task isn't mid-send when we destroy.
    if (!s_stop_flag && s_ws) {
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        esp_transport_handle_t ws_tmp = s_ws;
        s_ws = NULL;  // prevent any new sends
        xSemaphoreGive(s_ws_mutex);

        esp_transport_close(ws_tmp);
        esp_transport_destroy(ws_tmp);
    }

    // If we disconnected unexpectedly, update state
    if (!s_stop_flag && s_state != VOICE_STATE_IDLE) {
        voice_set_state(VOICE_STATE_IDLE, "disconnected");
    }

    ESP_LOGI(TAG, "WS receive task exiting");
    /* vTaskSuspend instead of vTaskDelete — P4 TLSP crash workaround (#20) */
    vTaskSuspend(NULL);
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

    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex) {
        ESP_LOGE(TAG, "Failed to create WS mutex");
        vSemaphoreDelete(s_play_mutex);
        vSemaphoreDelete(s_state_mutex);
        s_play_mutex = NULL;
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_play_sem = xSemaphoreCreateBinary();
    if (!s_play_sem) {
        ESP_LOGE(TAG, "Failed to create playback semaphore");
        vSemaphoreDelete(s_ws_mutex);
        vSemaphoreDelete(s_play_mutex);
        vSemaphoreDelete(s_state_mutex);
        s_ws_mutex = NULL;
        s_play_mutex = NULL;
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate playback ring buffer in PSRAM (too large for internal SRAM)
    s_play_capacity = TAB5_VOICE_PLAYBACK_BUF / sizeof(int16_t);
    s_play_buf = (int16_t *)heap_caps_malloc(
        TAB5_VOICE_PLAYBACK_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_play_buf) {
        ESP_LOGE(TAG, "Failed to allocate playback buffer in PSRAM");
        vSemaphoreDelete(s_ws_mutex);
        vSemaphoreDelete(s_play_mutex);
        vSemaphoreDelete(s_state_mutex);
        s_ws_mutex = NULL;
        s_play_mutex = NULL;
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    playback_buf_reset();
    s_transcript[0] = '\0';
    s_initialized = true;

    voice_set_state(VOICE_STATE_IDLE, NULL);
    ESP_LOGI(TAG, "Voice module initialized");
    return ESP_OK;
}

esp_err_t voice_connect(const char *dragon_host, uint16_t dragon_port)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ws_connected) {
        ESP_LOGW(TAG, "Already connected");
        return ESP_OK;
    }

    // Save connection info
    strncpy(s_dragon_host, dragon_host, sizeof(s_dragon_host) - 1);
    s_dragon_host[sizeof(s_dragon_host) - 1] = '\0';
    s_dragon_port = dragon_port;

    // Only set CONNECTING if not already in that state (voice_connect_async
    // already sets it before spawning the task — avoid duplicate UI callback)
    if (s_state != VOICE_STATE_CONNECTING) {
        voice_set_state(VOICE_STATE_CONNECTING, dragon_host);
    }

    /* Connection mode: 0=auto (ngrok→local), 1=local only, 2=remote only */
    uint8_t conn_mode = tab5_settings_get_connection_mode();
    ESP_LOGI(TAG, "Connection mode: %d (%s)",
             conn_mode, conn_mode == 0 ? "auto" : conn_mode == 1 ? "local" : "remote");

    int err = -1;

    /* Try ngrok (remote) if mode is auto(0) or remote(2) */
    if (conn_mode != 1) {
        ESP_LOGI(TAG, "Connecting to wss://%s:%d%s (ngrok%s)",
                 TAB5_NGROK_HOST, TAB5_NGROK_PORT, TAB5_VOICE_WS_PATH,
                 conn_mode == 2 ? " only" : " primary");

        esp_transport_handle_t ssl = esp_transport_ssl_init();
        if (ssl) {
            esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
            s_ws = esp_transport_ws_init(ssl);
            if (s_ws) {
                esp_transport_ws_set_path(s_ws, TAB5_VOICE_WS_PATH);
                err = esp_transport_connect(s_ws, TAB5_NGROK_HOST, TAB5_NGROK_PORT,
                                            VOICE_CONNECT_TIMEOUT_MS);
                if (err >= 0) {
                    strncpy(s_dragon_host, TAB5_NGROK_HOST, sizeof(s_dragon_host) - 1);
                    s_dragon_port = TAB5_NGROK_PORT;
                    ESP_LOGI(TAG, "Connected via ngrok!");
                } else {
                    ESP_LOGW(TAG, "ngrok connect failed%s",
                             conn_mode == 2 ? " (remote-only mode)" : " — trying local...");
                    esp_transport_close(s_ws);
                    esp_transport_destroy(s_ws);
                    s_ws = NULL;
                }
            } else {
                esp_transport_destroy(ssl);
            }
        } else {
            ESP_LOGW(TAG, "SSL transport init failed");
        }
    }

    /* Try local LAN if mode is auto(0) or local(1), and ngrok didn't connect */
    if (err < 0 && conn_mode != 2) {
        esp_transport_handle_t tcp_fb = esp_transport_tcp_init();
        if (tcp_fb) {
            s_ws = esp_transport_ws_init(tcp_fb);
            if (s_ws) {
                esp_transport_ws_set_path(s_ws, TAB5_VOICE_WS_PATH);
                ESP_LOGI(TAG, "Trying local ws://%s:%d%s",
                         dragon_host, dragon_port, TAB5_VOICE_WS_PATH);
                err = esp_transport_connect(s_ws, dragon_host, dragon_port,
                                            VOICE_CONNECT_TIMEOUT_MS);
                if (err < 0) {
                    ESP_LOGE(TAG, "Local fallback also failed");
                    esp_transport_close(s_ws);
                    esp_transport_destroy(s_ws);
                    s_ws = NULL;
                } else {
                    ESP_LOGI(TAG, "Connected via local LAN!");
                }
            }
        }

        if (!s_ws || err < 0) {
            voice_set_state(VOICE_STATE_IDLE, "connect failed");
            return ESP_FAIL;
        }
    }

    s_ws_connected = true;
    s_stop_flag = false;

    // Send device registration as FIRST text frame (per protocol.md §1.1).
    // Must happen before spawning the receive task to guarantee ordering.
    esp_err_t reg_err = ws_send_register();
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send register message");
        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = NULL;
        s_ws_connected = false;
        voice_set_state(VOICE_STATE_IDLE, "register failed");
        return reg_err;
    }

    // Start WS receive task
    BaseType_t ret = xTaskCreatePinnedToCore(
        ws_receive_task, "voice_ws", WS_TASK_STACK_SIZE,
        NULL, WS_TASK_PRIORITY, &s_ws_task, WS_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WS receive task");
        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = NULL;
        s_ws_connected = false;
        voice_set_state(VOICE_STATE_IDLE, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    // Start playback drain task — stack in PSRAM to save internal SRAM for WiFi DMA
    ret = xTaskCreatePinnedToCoreWithCaps(
        playback_drain_task, "voice_play", PLAY_TASK_STACK_SIZE,
        NULL, PLAY_TASK_PRIORITY, &s_play_task, PLAY_TASK_CORE,
        MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create playback drain task — audio may stutter");
    }

    // Don't set READY yet — wait for Dragon's session_start message.
    // Pipeline init on Dragon takes ~6s (loading models). Setting READY now
    // causes the UI to allow recording before the server is ready, leading to
    // lost audio frames and Error transport_poll_write.
    ESP_LOGI(TAG, "WS connected, register sent, waiting for session_start...");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Async connect — runs voice_connect in a background FreeRTOS task
// ---------------------------------------------------------------------------
typedef struct {
    char     host[64];
    uint16_t port;
    bool     auto_listen;
} async_connect_args_t;

static void async_connect_task(void *arg)
{
    async_connect_args_t *args = (async_connect_args_t *)arg;

    ESP_LOGI(TAG, "async_connect_task: starting (stack free: %u, host=%s:%d, auto_listen=%d)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             args->host, args->port, args->auto_listen);

    esp_err_t ret = voice_connect(args->host, args->port);
    ESP_LOGI(TAG, "async_connect_task: voice_connect returned %s (stack free: %u)",
             esp_err_to_name(ret), (unsigned)uxTaskGetStackHighWaterMark(NULL));

    if (ret == ESP_OK && args->auto_listen) {
        ESP_LOGI(TAG, "async_connect_task: calling voice_start_listening (auto_listen)");
        esp_err_t listen_ret = voice_start_listening();
        ESP_LOGI(TAG, "async_connect_task: voice_start_listening returned %s (stack free: %u)",
                 esp_err_to_name(listen_ret), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }

    s_connect_in_progress = false;
    free(args);
    /* vTaskSuspend instead of vTaskDelete — P4 TLSP crash workaround (#20) */
    vTaskSuspend(NULL);
}

esp_err_t voice_connect_async(const char *dragon_host, uint16_t dragon_port,
                               bool auto_listen)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ws_connected) {
        ESP_LOGW(TAG, "Already connected");
        if (auto_listen) {
            return voice_start_listening();
        }
        return ESP_OK;
    }
    if (s_connect_in_progress) {
        ESP_LOGW(TAG, "Connect already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    /* Set CONNECTING state immediately so UI can react */
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

    s_connect_in_progress = true;

    BaseType_t xret = xTaskCreatePinnedToCore(
        async_connect_task, "voice_conn", 8192,
        args, 4, NULL, 1);
    if (xret != pdPASS) {
        s_connect_in_progress = false;
        free(args);
        voice_set_state(VOICE_STATE_IDLE, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t voice_start_listening(void)
{
    ESP_LOGI(TAG, "voice_start_listening: initialized=%d, ws_connected=%d, state=%d",
             s_initialized, s_ws_connected, s_state);

    if (!s_initialized || !s_ws_connected) {
        ESP_LOGE(TAG, "Not connected (initialized=%d, ws_connected=%d)",
                 s_initialized, s_ws_connected);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != VOICE_STATE_READY) {
        ESP_LOGW(TAG, "Cannot start listening in state %d", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting push-to-talk (ask mode)");
    s_voice_mode = VOICE_MODE_ASK;

    // Send start signal to Dragon
    ESP_LOGI(TAG, "Sending {\"type\":\"start\"} to Dragon...");
    esp_err_t err = ws_send_text("{\"type\":\"start\"}");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send start signal");
        return err;
    }
    ESP_LOGI(TAG, "Start signal sent OK");

    // Clear previous transcript and separated buffers
    s_transcript[0] = '\0';
    s_stt_text[0] = '\0';
    s_llm_text[0] = '\0';

    // Start mic capture task on core 1
    s_mic_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        mic_capture_task, "voice_mic", MIC_TASK_STACK_SIZE,
        NULL, MIC_TASK_PRIORITY, &s_mic_task, MIC_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mic capture task");
        ws_send_text("{\"type\":\"stop\"}");
        s_mic_running = false;
        return ESP_ERR_NO_MEM;
    }

    voice_set_state(VOICE_STATE_LISTENING, NULL);
    return ESP_OK;
}

esp_err_t voice_start_dictation(void)
{
    if (!s_initialized || !s_ws_connected) {
        ESP_LOGE(TAG, "Not connected for dictation");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != VOICE_STATE_READY) {
        ESP_LOGW(TAG, "Cannot start dictation in state %d", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting dictation mode (unlimited, STT-only)");
    s_voice_mode = VOICE_MODE_DICTATE;

    /* Allocate dictation text buffer in PSRAM if not already */
    if (!s_dictation_text) {
        s_dictation_text = heap_caps_malloc(DICTATION_TEXT_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_dictation_text) {
            ESP_LOGE(TAG, "Failed to allocate dictation buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    s_dictation_text[0] = '\0';

    /* Send start with dictate mode to Dragon */
    esp_err_t err = ws_send_text("{\"type\":\"start\",\"mode\":\"dictate\"}");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send dictation start");
        return err;
    }

    /* Clear previous transcripts */
    s_transcript[0] = '\0';
    s_stt_text[0] = '\0';
    s_llm_text[0] = '\0';

    /* Start mic capture task — same task, mode-aware behavior */
    s_mic_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        mic_capture_task, "voice_mic", MIC_TASK_STACK_SIZE,
        NULL, MIC_TASK_PRIORITY, &s_mic_task, MIC_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mic capture task");
        ws_send_text("{\"type\":\"stop\"}");
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
    if (!s_ws_connected) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Clearing conversation history on Dragon");
    return ws_send_text("{\"type\":\"clear\"}");
}

esp_err_t voice_stop_listening(void)
{
    if (s_state != VOICE_STATE_LISTENING) {
        ESP_LOGW(TAG, "Not listening (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping push-to-talk");

    // Stop mic capture task
    s_mic_running = false;
    // Wait briefly for task to finish (it checks s_mic_running each chunk)
    for (int i = 0; i < 10 && s_mic_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Send stop signal to Dragon
    esp_err_t err = ws_send_text("{\"type\":\"stop\"}");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Stop signal failed — connection lost");
        voice_set_state(VOICE_STATE_IDLE, "Connection lost");
        return ESP_FAIL;
    }

    // Reset the activity timestamp so the response timeout starts NOW
    extern void voice_reset_activity_timestamp(void);
    voice_reset_activity_timestamp();

    voice_set_state(VOICE_STATE_PROCESSING, NULL);
    return ESP_OK;
}

esp_err_t voice_cancel(void)
{
    if (s_state == VOICE_STATE_IDLE) {
        return ESP_OK;  /* Nothing to cancel */
    }
    ESP_LOGI(TAG, "Cancelling voice session");

    // Stop mic if running
    if (s_mic_running) {
        s_mic_running = false;
        for (int i = 0; i < 10 && s_mic_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Clear playback buffer
    playback_buf_reset();
    tab5_audio_speaker_enable(false);

    // Send cancel to Dragon if connected
    if (s_ws_connected) {
        ws_send_text("{\"type\":\"cancel\"}");
    }

    if (s_ws_connected) {
        voice_set_state(VOICE_STATE_READY, "cancelled");
    } else {
        voice_set_state(VOICE_STATE_IDLE, "cancelled");
    }

    return ESP_OK;
}

esp_err_t voice_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting from Dragon voice server");

    tab5_audio_speaker_enable(false);

    // Signal tasks to stop — order matters:
    // 1. Stop mic first (it uses ws_send_binary)
    s_stop_flag = true;
    s_mic_running = false;

    // Wait for mic task to exit so it's no longer sending on the WS
    for (int i = 0; i < 20 && s_mic_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 2. Mark disconnected so no new sends can start
    s_ws_connected = false;

    // 3. Close WS under mutex so we don't race with any in-flight send/recv
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    if (s_ws) {
        esp_transport_close(s_ws);
    }
    xSemaphoreGive(s_ws_mutex);

    // 4. Stop playback drain task
    s_play_running = false;
    if (s_play_sem) xSemaphoreGive(s_play_sem);  // wake it so it can exit
    for (int i = 0; i < 30 && s_play_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 5. Wait for WS receive task to exit (it checks s_ws_connected + s_stop_flag)
    for (int i = 0; i < 150 && s_ws_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_ws_task != NULL) {
        ESP_LOGW(TAG, "WS receive task did not exit in time");
    }

    // 6. Destroy transport — safe now that all tasks have exited
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    if (s_ws) {
        esp_transport_destroy(s_ws);
        s_ws = NULL;
    }
    xSemaphoreGive(s_ws_mutex);

    playback_buf_reset();
    voice_set_state(VOICE_STATE_IDLE, NULL);

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
    /* Note: caller must not hold reference across calls — copy if needed.
       Protected by s_state_mutex in handle_text_message(). */
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
    if (!s_ws_connected) return ESP_ERR_INVALID_STATE;

    /* Build JSON: {"type":"text","content":"..."} */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "text");
    cJSON_AddStringToObject(msg, "content", text);
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Sending text to Dragon: %s", text);

    /* Store as user STT text for UI display */
    if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strncpy(s_stt_text, text, MAX_TRANSCRIPT_LEN - 1);
    s_stt_text[MAX_TRANSCRIPT_LEN - 1] = '\0';
    strncpy(s_transcript, text, MAX_TRANSCRIPT_LEN - 1);
    s_transcript[MAX_TRANSCRIPT_LEN - 1] = '\0';
    s_llm_text[0] = '\0';
    if (s_state_mutex) xSemaphoreGive(s_state_mutex);

    esp_err_t ret = ws_send_text(json);
    free(json);

    if (ret == ESP_OK) {
        voice_set_state(VOICE_STATE_PROCESSING, text);
    }
    return ret;
}

esp_err_t voice_send_config_update(int voice_mode, const char *llm_model)
{
    if (!s_ws_connected) return ESP_ERR_INVALID_STATE;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "config_update");
    cJSON_AddNumberToObject(msg, "voice_mode", voice_mode);
    /* Backward compat: Dragon still checks cloud_mode bool */
    cJSON_AddBoolToObject(msg, "cloud_mode", voice_mode >= 1);
    if (voice_mode == 2 && llm_model && llm_model[0]) {
        cJSON_AddStringToObject(msg, "llm_model", llm_model);
    }
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Sending config_update: voice_mode=%d llm_model=%s",
             voice_mode, (llm_model && llm_model[0]) ? llm_model : "(local)");
    esp_err_t ret = ws_send_text(json);
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
// N1: Reconnect watchdog — auto-reconnects voice WS after disconnect
// ---------------------------------------------------------------------------

static void reconnect_watchdog_task(void *arg)
{
    ESP_LOGI(TAG, "Reconnect watchdog started");
    int backoff_ms = RECONNECT_BACKOFF_MS;

    while (s_reconnect_enabled) {
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_CHECK_MS));

        if (!s_reconnect_enabled) break;

        /* Only reconnect if: initialized, not connected, not already connecting,
         * and not intentionally stopped (s_stop_flag) */
        if (s_initialized && !s_ws_connected && !s_connect_in_progress && !s_stop_flag) {
            ESP_LOGI(TAG, "Watchdog: voice WS disconnected — reconnecting (backoff %dms)", backoff_ms);

            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (!s_reconnect_enabled || s_ws_connected) continue;

            char dhost[64];
            tab5_settings_get_dragon_host(dhost, sizeof(dhost));
            if (dhost[0]) {
                /* Silent reconnect — don't show overlay */
                extern void ui_voice_set_boot_connect(bool silent);
                ui_voice_set_boot_connect(true);
                voice_connect_async(dhost, TAB5_VOICE_PORT, false);

                /* Wait for connect result */
                for (int i = 0; i < 60 && s_connect_in_progress; i++) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }

                if (s_ws_connected) {
                    ESP_LOGI(TAG, "Watchdog: reconnected successfully!");
                    backoff_ms = RECONNECT_BACKOFF_MS;  /* reset backoff */
                    /* S6: Sync pending notes that were created while offline */
                    extern void ui_notes_sync_pending(void);
                    ui_notes_sync_pending();
                } else {
                    ESP_LOGW(TAG, "Watchdog: reconnect failed");
                    backoff_ms = (backoff_ms < 60000) ? backoff_ms * 2 : 60000;
                }
            }
        } else if (s_ws_connected) {
            backoff_ms = RECONNECT_BACKOFF_MS;  /* reset when connected */
        }
    }

    ESP_LOGI(TAG, "Reconnect watchdog stopped");
    s_reconnect_task = NULL;
    vTaskSuspend(NULL);
}

esp_err_t voice_start_reconnect_watchdog(void)
{
    if (s_reconnect_task) return ESP_OK;  /* already running */

    s_reconnect_enabled = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        reconnect_watchdog_task, "voice_recon", 4096,
        NULL, 3, &s_reconnect_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reconnect watchdog task");
        s_reconnect_enabled = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void voice_stop_reconnect_watchdog(void)
{
    s_reconnect_enabled = false;
    /* Task will exit on next check */
}

bool voice_is_connected(void)
{
    return s_ws_connected && s_ws_running;
}

// ---------------------------------------------------------------------------
// Always-listening mode (AFE + wake word)
// ---------------------------------------------------------------------------

esp_err_t voice_start_always_listening(void)
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

    // Start mic capture task — runs continuously to feed AFE
    // Use PSRAM stack to avoid exhausting limited internal SRAM (only 44KB free)
    s_mic_running = true;
    s_mic_task = NULL;  // Force fresh creation
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

    // Wait for mic to start feeding before launching detect task
    vTaskDelay(pdMS_TO_TICKS(500));

    // Start AFE detect task — also PSRAM stack
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

esp_err_t voice_stop_always_listening(void)
{
    if (!s_afe_enabled) return ESP_OK;

    s_afe_listening = false;
    s_afe_enabled = false;

    // Wait for detect task to exit
    vTaskDelay(pdMS_TO_TICKS(100));

    tab5_afe_deinit();
    ESP_LOGI(TAG, "Always-listening mode stopped");
    return ESP_OK;
}

bool voice_is_always_listening(void)
{
    return s_afe_enabled && s_afe_listening;
}
