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
#include "audio.h"
#include "config.h"
#include "ui_core.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
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
#define MIC_TDM_MIC1_OFF       0    // MIC1 (left mic) at slot 0
#define MIC_TDM_SAMPLES        (MIC_48K_FRAMES * MIC_TDM_CHANNELS)
// Downsample ratio: 48kHz -> 16kHz = 3:1
#define DOWNSAMPLE_RATIO       (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)
// Upsample ratio: 16kHz -> 48kHz = 1:3 (mirror of downsample)
#define UPSAMPLE_RATIO         (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)

// Reconnect parameters
#define VOICE_RECONNECT_BASE_MS  2000
#define VOICE_RECONNECT_MAX_MS   15000
#define VOICE_CONNECT_TIMEOUT_MS 5000
#define VOICE_RESPONSE_TIMEOUT_MS 120000

// Mic capture task (needs room for 3840-sample TDM buffer on stack)
#define MIC_TASK_STACK_SIZE    4096  /* Reduced: tdm_buf moved to PSRAM heap (#18) */
#define MIC_TASK_PRIORITY      5
#define MIC_TASK_CORE          1

// WS receive task
#define WS_TASK_STACK_SIZE     8192
#define WS_TASK_PRIORITY       4
#define WS_TASK_CORE           1

// Max transcript length
#define MAX_TRANSCRIPT_LEN     512

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

// Playback ring buffer (smooths network jitter)
static int16_t  s_play_buf[TAB5_VOICE_PLAYBACK_BUF / sizeof(int16_t)];
static size_t   s_play_wr = 0;  // write index (samples)
static size_t   s_play_rd = 0;  // read index (samples)
static size_t   s_play_count = 0; // samples available
static SemaphoreHandle_t s_play_mutex = NULL;
#define PLAY_BUF_CAPACITY  (TAB5_VOICE_PLAYBACK_BUF / sizeof(int16_t))

// Last transcript from Dragon STT
static char s_transcript[MAX_TRANSCRIPT_LEN] = {0};

static bool s_initialized = false;
static volatile bool s_connect_in_progress = false;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void voice_set_state(voice_state_t new_state, const char *detail);
static void mic_capture_task(void *arg);
static void ws_receive_task(void *arg);
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
            tab5_ui_lock();
            s_state_cb(new_state, detail);
            tab5_ui_unlock();
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
        ESP_LOGE(TAG, "WS text send failed: %d", ret);
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
        ESP_LOGE(TAG, "WS binary send failed (%zu bytes): %d", len, ret);
        return ESP_FAIL;
    }
    return ESP_OK;
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

    if (strcmp(type_str, "stt") == 0) {
        // Speech-to-text result
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            strncpy(s_transcript, text->valuestring, MAX_TRANSCRIPT_LEN - 1);
            s_transcript[MAX_TRANSCRIPT_LEN - 1] = '\0';
            if (s_state_mutex) xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "STT: \"%s\"", s_transcript);
            voice_set_state(VOICE_STATE_PROCESSING, s_transcript);
        }
    } else if (strcmp(type_str, "tts_start") == 0) {
        // Dragon is about to send TTS audio
        ESP_LOGI(TAG, "TTS start — preparing playback");
        tab5_audio_speaker_enable(true);
        playback_buf_reset();
        voice_set_state(VOICE_STATE_SPEAKING, NULL);
    } else if (strcmp(type_str, "tts_end") == 0) {
        // TTS audio stream complete — drain remaining buffer
        ESP_LOGI(TAG, "TTS end — draining playback buffer");

        // Flush any remaining samples in the ring buffer to the speaker
        int16_t drain_buf[VOICE_CHUNK_SAMPLES];
        size_t drained;
        while ((drained = playback_buf_read(drain_buf, VOICE_CHUNK_SAMPLES)) > 0) {
            tab5_audio_play_raw(drain_buf, drained);
        }

        tab5_audio_speaker_enable(false);
        voice_set_state(VOICE_STATE_READY, NULL);
    } else if (strcmp(type_str, "llm") == 0) {
        // Streaming LLM response token — append to transcript
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            size_t cur_len = strlen(s_transcript);
            size_t add_len = strlen(text->valuestring);
            if (cur_len + add_len < MAX_TRANSCRIPT_LEN - 1) {
                strcat(s_transcript, text->valuestring);
            }
            if (s_state_mutex) xSemaphoreGive(s_state_mutex);
            ESP_LOGD(TAG, "LLM token: \"%s\"", text->valuestring);
            // Update UI with streaming response
            voice_set_state(VOICE_STATE_PROCESSING, s_transcript);
        }
    } else if (strcmp(type_str, "error") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        const char *err_src = cJSON_IsString(msg) ? msg->valuestring : "unknown";
        ESP_LOGE(TAG, "Dragon error: %s", err_src);
        // Copy to local buffer — err_src points into cJSON tree that we free below.
        // voice_set_state callback must not hold a dangling pointer.
        char err_buf[128];
        strncpy(err_buf, err_src, sizeof(err_buf) - 1);
        err_buf[sizeof(err_buf) - 1] = '\0';
        voice_set_state(VOICE_STATE_READY, err_buf);
    } else if (strcmp(type_str, "session_start") == 0) {
        ESP_LOGI(TAG, "Dragon session_start received (state=%d)", s_state);
        /* Dragon is ready — no state change needed, voice_connect()
           already set READY when the WS connection succeeded. */
    } else {
        ESP_LOGW(TAG, "Unknown message type: %s (full: %.*s)", type_str, len, data);
    }

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Mic capture task — reads 4-ch TDM at 48kHz, extracts MIC1, downsamples
// to 16kHz mono, sends via WS to Dragon
// ---------------------------------------------------------------------------
static void mic_capture_task(void *arg)
{
    ESP_LOGI(TAG, "Mic capture task started (core %d)", xPortGetCoreID());

    // TDM buffer in PSRAM — saves 7.7KB internal RAM for SDIO DMA (#18)
    int16_t *tdm_buf = (int16_t *)heap_caps_malloc(
        MIC_TDM_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono_buf = (int16_t *)heap_caps_malloc(
        VOICE_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tdm_buf || !mono_buf) {
        ESP_LOGE(TAG, "Failed to allocate mic buffers");
        heap_caps_free(tdm_buf);
        heap_caps_free(mono_buf);
        s_mic_task = NULL;
        /* vTaskSuspend instead of vTaskDelete — P4 TLSP crash workaround (#20) */
    vTaskSuspend(NULL);
        return;
    }

    s_mic_running = true;
    int frames_sent = 0;

    while (s_mic_running && s_ws_connected) {
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

        // Extract selected slot and downsample 48kHz -> 16kHz with box filter.
        int out_idx = 0;
        for (int i = 0; i + DOWNSAMPLE_RATIO - 1 < MIC_48K_FRAMES && out_idx < VOICE_CHUNK_SAMPLES; i += DOWNSAMPLE_RATIO) {
            int32_t sum = 0;
            for (int j = 0; j < DOWNSAMPLE_RATIO; j++) {
                sum += tdm_buf[(i + j) * MIC_TDM_CHANNELS + MIC_TDM_MIC1_OFF];
            }
            mono_buf[out_idx++] = (int16_t)(sum / DOWNSAMPLE_RATIO);
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

        // Send 16kHz mono PCM as binary WS frame
        size_t send_bytes = out_idx * sizeof(int16_t);
        err = ws_send_binary(mono_buf, send_bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send audio chunk");
            break;
        }
        frames_sent++;
        #define MAX_RECORD_FRAMES 1500  /* 1500 * 20ms = 30s */
        if (frames_sent >= MAX_RECORD_FRAMES) {
            ESP_LOGI(TAG, "Max recording duration reached (%ds)", MAX_RECORD_FRAMES * TAB5_VOICE_CHUNK_MS / 1000);
            break;
        }
    }

    s_mic_running = false;
    heap_caps_free(tdm_buf);
    heap_caps_free(mono_buf);
    ESP_LOGI(TAG, "Mic capture task exiting");
    s_mic_task = NULL;
    /* vTaskSuspend instead of vTaskDelete — P4 TLSP crash workaround (#20) */
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
// WebSocket receive task — reads from Dragon, handles text + binary frames
// ---------------------------------------------------------------------------
static void ws_receive_task(void *arg)
{
    ESP_LOGI(TAG, "WS receive task started (core %d)", xPortGetCoreID());

    // Receive buffer — Dragon sends TTS audio in 4096-byte chunks
    char rx_buf[4100];
    int16_t play_chunk[VOICE_CHUNK_SAMPLES];

    // Static upsample buffer: max 2048 input samples × 3 = 6144 output samples
    // Static because only one ws_receive_task runs at a time (saves stack)
    static int16_t upsample_buf[2048 * UPSAMPLE_RATIO];

    s_ws_running = true;
    s_last_activity_us = esp_timer_get_time();

    while (s_ws_connected && !s_stop_flag) {
        int poll = esp_transport_poll_read(s_ws, 100);
        if (poll < 0) {
            ESP_LOGW(TAG, "WS poll error, connection lost");
            s_ws_connected = false;
            break;
        }
        if (poll == 0) {
            // No data available — check if we should drain playback buffer
            if (s_state == VOICE_STATE_SPEAKING) {
                size_t avail = playback_buf_read(play_chunk, VOICE_CHUNK_SAMPLES);
                if (avail > 0) {
                    tab5_audio_play_raw(play_chunk, avail);
                }
            }
            if (s_state == VOICE_STATE_PROCESSING) {
                int64_t elapsed_us = esp_timer_get_time() - s_last_activity_us;
                if (elapsed_us > (int64_t)VOICE_RESPONSE_TIMEOUT_MS * 1000) {
                    ESP_LOGW(TAG, "Response timeout (%d ms)", VOICE_RESPONSE_TIMEOUT_MS);
                    voice_set_state(VOICE_STATE_READY, "timeout");
                    s_last_activity_us = esp_timer_get_time();
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
                size_t in_samples = len / sizeof(int16_t);
                size_t out_samples = upsample_16k_to_48k(
                    (const int16_t *)rx_buf, in_samples,
                    upsample_buf, sizeof(upsample_buf) / sizeof(int16_t));
                playback_buf_write(upsample_buf, out_samples);

                // Immediately try to play what we have
                size_t avail = playback_buf_read(play_chunk, VOICE_CHUNK_SAMPLES);
                if (avail > 0) {
                    tab5_audio_play_raw(play_chunk, avail);
                }
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

    // Create WS transport (same pattern as touch_ws.c)
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    if (!tcp) {
        ESP_LOGE(TAG, "Failed to create TCP transport");
        voice_set_state(VOICE_STATE_IDLE, "transport error");
        return ESP_ERR_NO_MEM;
    }

    s_ws = esp_transport_ws_init(tcp);
    if (!s_ws) {
        ESP_LOGE(TAG, "Failed to create WS transport");
        esp_transport_destroy(tcp);
        voice_set_state(VOICE_STATE_IDLE, "transport error");
        return ESP_ERR_NO_MEM;
    }

    esp_transport_ws_set_path(s_ws, TAB5_VOICE_WS_PATH);

    ESP_LOGI(TAG, "Connecting to ws://%s:%d%s",
             s_dragon_host, s_dragon_port, TAB5_VOICE_WS_PATH);

    int err = esp_transport_connect(s_ws, s_dragon_host, s_dragon_port,
                                    VOICE_CONNECT_TIMEOUT_MS);
    if (err < 0) {
        ESP_LOGW(TAG, "WS connect failed");
        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = NULL;
        voice_set_state(VOICE_STATE_IDLE, "connect failed");
        return ESP_FAIL;
    }

    s_ws_connected = true;
    s_stop_flag = false;

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

    voice_set_state(VOICE_STATE_READY, NULL);
    ESP_LOGI(TAG, "Connected to Dragon voice server");
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

    ESP_LOGI(TAG, "Starting push-to-talk");

    // Send start signal to Dragon
    ESP_LOGI(TAG, "Sending {\"type\":\"start\"} to Dragon...");
    esp_err_t err = ws_send_text("{\"type\":\"start\"}");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send start signal");
        return err;
    }
    ESP_LOGI(TAG, "Start signal sent OK");

    // Clear previous transcript
    s_transcript[0] = '\0';

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
        ESP_LOGW(TAG, "Failed to send stop signal");
    }

    // Reset the activity timestamp so the response timeout starts NOW,
    // not from when the last WS frame was received (which could be 30s ago
    // during recording when Tab5 was only sending, not receiving).
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

    // 4. Wait for WS receive task to exit (it checks s_ws_connected + s_stop_flag)
    for (int i = 0; i < 150 && s_ws_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_ws_task != NULL) {
        ESP_LOGW(TAG, "WS receive task did not exit in time");
    }

    // 5. Destroy transport — safe now that all tasks have exited
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
