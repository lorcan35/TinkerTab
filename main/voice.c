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
#include "task_worker.h"   /* #133: defer queue-drain to avoid stack blow */
#include <limits.h>  /* W14-L02: INT_MAX for WS size_t->int bound */
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
#include "wifi.h"

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

#define MIC_TDM_SAMPLES        (MIC_48K_FRAMES * MIC_TDM_CHANNELS)
// Downsample ratio: 48kHz -> 16kHz = 3:1
#define DOWNSAMPLE_RATIO       (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)
// Upsample ratio: 16kHz -> 48kHz = 1:3
#define UPSAMPLE_RATIO         (TAB5_AUDIO_SAMPLE_RATE / TAB5_VOICE_SAMPLE_RATE)

// esp_websocket_client tuning
#define WS_CLIENT_TASK_STACK   10240   /* client event/receive task stack */
#define WS_CLIENT_BUFFER_SIZE  4096    /* per-frame buffer; binary TTS comes in 640–1024 B chunks, headroom for text+media */
#define WS_CLIENT_RECONNECT_MS 1000    /* base reconnect delay -- exponential ramp takes over from here */
#define WS_CLIENT_NETWORK_MS   10000   /* blocking send deadline */
#define WS_CLIENT_PING_SEC     15      /* built-in WS-level ping */
/* Pong budget widened to 180 s (12x ping interval) so a long-running
 * local LLM turn on Dragon's ARM64 CPU doesn't tear the WS down before
 * the first token streams back.  Dragon's aiohttp keepalive answers
 * pings on its own asyncio loop independent of the LLM coroutine,
 * so bumping this doesn't hide real network failures -- it only stops
 * us from killing the session while Ollama is genuinely thinking. */
#define WS_CLIENT_PONG_SEC     180

/* v4·D connectivity audit T1.1: exponential backoff + full jitter on
 * WS reconnect.  Formula per the audit:
 *   exp = min(BACKOFF_CAP_MS, BACKOFF_MIN_MS << min(attempt, 5))
 *   delay = esp_random() % (exp/2) + exp/2   // full jitter [exp/2, exp)
 * attempt resets to 0 on WEBSOCKET_EVENT_CONNECTED.
 * Before: fixed 2 s reconnect -> thundering herd on Dragon reboot. */
#define WS_CLIENT_BACKOFF_MIN_MS 1000
#define WS_CLIENT_BACKOFF_CAP_MS 30000

// Ngrok fallback is attempted after this many consecutive failed handshakes
// against the local LAN URI (only in conn_mode=0 "auto").
// Raised from 2 to 4 per connectivity audit: co-boot with Dragon almost
// always fails the first 2 LAN handshakes while Dragon is still loading
// Moonshine/Piper/Ollama -- raising the threshold stops us from
// immediately pinning to ngrok on a simple server restart.
#define NGROK_FALLBACK_THRESHOLD 4

/* γ3-Tab5 (issue #198): stop retrying after this many consecutive
 * 401 handshake failures.  3 is tight enough that a misconfigured
 * device doesn't burn battery / log noise / ngrok bandwidth, but
 * loose enough to absorb a transient 401 during a token-rotation
 * race on Dragon restart. */
#define WS_AUTH_FAIL_THRESHOLD 3

// Mic capture task — needs room for AFE feed buffer (960 int16 = 1.9KB) + TDM diagnostics
#define MIC_TASK_STACK_SIZE    8192
#define MIC_TASK_PRIORITY      5
#define MIC_TASK_CORE          1

// Playback drain task — higher priority than WS receive so I2S stays fed
/* Wave 9 audit #80 fix: raise playback drain priority so TTS audio
 * gets consumed faster than it arrives. At priority 6 the WS rx task
 * (priority ~7-8 from esp_websocket_client) can out-run the drain,
 * lwIP recv buffers pile up against DMA-capable internal SRAM, and
 * WiFi Rx ring starves at ~15KB free (device goes one-way dead).
 * Bumping to priority 9 preempts WS rx so the ring drains with the
 * playback cadence.
 *
 * Stack bumped 4 KB -> 6 KB at the same time: the longer-TTS regression
 * test caught a "Stack protection fault" at task priority 9 because the
 * deeper preemption stored more context per entry. */
#define PLAY_TASK_STACK_SIZE   6144
#define PLAY_TASK_PRIORITY     9
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
/* Wave 14 W14-M03: volatile because s_ws is written on Core 1's WS
 * task (connect/disconnect) and read on Core 0's LVGL thread
 * (voice_send_text, voice_start_listening, async_connect_task poll).
 * Without volatile the compiler may cache the load across a
 * disconnect and use-after-free the freed WS client. */
static esp_websocket_client_handle_t volatile s_ws = NULL;

// Dragon connection info (last-known, updated on each connect)
static char     s_dragon_host[64] = {0};
static uint16_t s_dragon_port     = TAB5_VOICE_PORT;

// Connection tracking
static volatile bool s_initialized        = false;
static volatile bool s_started            = false;  /* esp_websocket_client_start() has been called */
static volatile bool s_disconnecting      = false;  /* US-C21: guard against connect-during-disconnect race */
static volatile int  s_handshake_fail_cnt = 0;      /* consecutive handshake failures — trigger ngrok fallback in auto mode */
static volatile int  s_auth_fail_cnt      = 0;      /* γ3-Tab5 (issue #198): consecutive 401s — trigger stop-retry after WS_AUTH_FAIL_THRESHOLD */
/* v4·D connectivity audit T1.1: reconnect backoff state.  Counts
 * attempts since the last successful CONNECTED event.  Applied via
 * esp_websocket_client_set_reconnect_timeout inside the ERROR /
 * DISCONNECTED handlers; reset on CONNECTED.  */
static volatile int  s_connect_attempt   = 0;
/* v4·D connectivity audit T1.3: link health published by probe task. */
static volatile bool s_lan_tcp_ok        = false;
static volatile bool s_ngrok_tcp_ok      = false;
static volatile uint32_t s_last_probe_ms = 0;
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

/* Wave 9 audit #80 + AUDIT-stability P0: pre-allocated upsample buffer
 * reused across every TTS chunk. Dragon TTS chunks are <=1024 bytes in
 * (512 samples), so the worst-case upsample output is 512 * UPSAMPLE_RATIO
 * samples (~3 KB for the typical 16k->48k ratio). Allocating 4096 samples
 * gives headroom for edge cases. The old per-chunk heap_caps_malloc +
 * heap_caps_free in handle_binary_message caused PSRAM fragmentation
 * over long sessions and — combined with DMA pressure from the WS rx
 * backlog — was a contributor to the one-way WiFi death at ~15 KB DMA. */
#define UPSAMPLE_BUF_CAPACITY  (4096 * 2)  /* samples — 8 KB PSRAM */
static int16_t          *s_upsample_buf  = NULL;

// Last transcript from Dragon STT
static char s_transcript[MAX_TRANSCRIPT_LEN] = {0};
static char s_stt_text[MAX_TRANSCRIPT_LEN]   = {0};
static char s_llm_text[MAX_TRANSCRIPT_LEN]   = {0};

/* v4·D Gauntlet G1: single-slot queued-turn buffer.  When voice_send_text
 * is invoked while the pipeline is busy, the new text is stashed here
 * instead of rejected.  It drains on the next transition back to READY. */
static char s_queued_text[MAX_TRANSCRIPT_LEN] = {0};
static bool s_queue_pending = false;

/* v4·D Phase 3b: cached per-turn receipt from Dragon. */
static int  s_last_receipt_mils       = 0;
static int  s_last_receipt_prompt_tok = 0;
static int  s_last_receipt_compl_tok  = 0;
static char s_last_receipt_model[64]  = {0};

/* v4·D Phase 4b: cached vision capability from Dragon (camera screen). */
static bool s_vision_capable   = false;
static int  s_vision_per_frame_mils = 0;
static char s_vision_model[64] = {0};

// Dictation mode
static voice_mode_t   s_voice_mode        = VOICE_MODE_ASK;
static char          *s_dictation_text    = NULL;  /* PSRAM-allocated, DICTATION_TEXT_SIZE */
static volatile float s_current_rms       = 0.0f;
static char           s_dictation_title[128]   = {0};
static char           s_dictation_summary[512] = {0};

// Drop counter for audio frames lost under back-pressure (US-C04)
static int s_audio_drop_count = 0;

// Activity timestamp shared with stop_listening for response timeout
static volatile int64_t s_last_activity_us = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void voice_set_state(voice_state_t new_state, const char *detail);
static void _drain_queued_text_job(void *arg);   /* #133 */
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
// Deferred receipt attach — hops from voice WS rx task onto LVGL thread so
// it queues AFTER ui_chat_push_message("assistant", …) from llm_done.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t mils;
    uint16_t ptok;
    uint16_t ctok;
    bool     retried;
    char     model_short[16];
} receipt_attach_async_t;

static void receipt_attach_async_cb(void *arg)
{
    receipt_attach_async_t *r = (receipt_attach_async_t *)arg;
    if (!r) return;
    extern int  chat_store_attach_receipt_ex(uint32_t, uint16_t, uint16_t,
                                              const char *, bool);
    extern void ui_chat_refresh_receipts(void);
    chat_store_attach_receipt_ex(r->mils, r->ptok, r->ctok,
                                  r->model_short, r->retried);
    ui_chat_refresh_receipts();
    free(r);
}

void voice_defer_receipt_attach(uint32_t mils,
                                uint16_t ptok, uint16_t ctok,
                                const char *model_short, bool retried)
{
    receipt_attach_async_t *r = calloc(1, sizeof(*r));
    if (!r) {
        /* Wave 14 W14-M08: log the drop so "my cost tracking is off"
         * bug reports have a trail. */
        ESP_LOGW(TAG, "voice_defer_receipt_attach OOM — dropped receipt (mils=%lu)",
                 (unsigned long)mils);
        return;
    }
    r->mils    = mils;
    r->ptok    = ptok;
    r->ctok    = ctok;
    r->retried = retried;
    if (model_short) {
        snprintf(r->model_short, sizeof(r->model_short), "%s", model_short);
    }
    lv_async_call(receipt_attach_async_cb, r);
}

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

    /* v4·D Gauntlet G1: drain the queued turn on transition to READY.
     *
     * closes #133: was `voice_send_text(pending)` called inline here.
     * That recursed voice_set_state → ui_voice_on_state_change → ESP_LOGI
     * on the CURRENT task's stack.  When the current task was voice_play
     * (6 KB stack) and the log chain went deep into newlib's vfprintf,
     * the stack blew up and the task panicked.  Captured coredump:
     * voice_play → voice_set_state(READY) → voice_send_text('And 5 times
     * 3?') → voice_set_state(PROCESSING) → ui_voice_on_state_change →
     * _vfprintf_r → fault.
     *
     * Defer the drain via tab5_worker_enqueue so it runs on the shared
     * worker task (16 KB stack in PSRAM) instead of whatever task
     * triggered the state change.  */
    if (new_state == VOICE_STATE_READY && s_queue_pending) {
        char *pending = malloc(MAX_TRANSCRIPT_LEN);
        if (!pending) {
            ESP_LOGW(TAG, "queue drain: malloc failed; dropping queued text");
            s_queued_text[0] = '\0';
            s_queue_pending = false;
            return;
        }
        if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        strncpy(pending, s_queued_text, MAX_TRANSCRIPT_LEN - 1);
        pending[MAX_TRANSCRIPT_LEN - 1] = '\0';
        s_queued_text[0] = '\0';
        s_queue_pending = false;
        if (s_state_mutex) xSemaphoreGive(s_state_mutex);
        if (pending[0]) {
            ESP_LOGI(TAG, "G1 drain: deferring queued text -> '%s'", pending);
            if (tab5_worker_enqueue(_drain_queued_text_job, pending,
                                    "voice-drain-queue") != ESP_OK) {
                ESP_LOGW(TAG, "queue drain: enqueue failed; dropping '%s'",
                         pending);
                free(pending);
            }
        } else {
            free(pending);
        }
    }
}

/* closes #133: runs on the shared worker (16 KB PSRAM stack), safe for
 * the deep voice_send_text → voice_set_state → ESP_LOGI call chain. */
static void _drain_queued_text_job(void *arg)
{
    char *text = (char *)arg;
    if (text && text[0]) {
        voice_send_text(text);
    }
    free(text);
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
    if (!msg) return ESP_ERR_INVALID_ARG;

    size_t len = strlen(msg);
    /* Wave 14 W14-L02: bound the size_t→int cast.  Realistic messages
     * are <256 bytes but an unchecked cast would silently truncate on
     * an accidental >2 GB string (e.g. a corrupted LLM response
     * flowing through text_update). */
    if (len > INT_MAX) return ESP_ERR_INVALID_ARG;
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
    if (!data) return ESP_ERR_INVALID_ARG;
    /* W14-L02: same bound as text. */
    if (len > INT_MAX) return ESP_ERR_INVALID_ARG;

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
static void _voice_stop_ws_worker_fn(void *arg)
{
    (void)arg;
    if (s_ws) {
        ESP_LOGW(TAG, "device_evicted worker: stopping WS client now");
        esp_websocket_client_stop(s_ws);
    }
}

/* γ3-Tab5 (issue #198): worker for the auth-failed stop path.
 * Same task-hop pattern as _voice_stop_ws_worker_fn — must run
 * off the WS task or esp_websocket_client_stop() no-ops.
 * Separate function (not shared with the eviction worker) so the
 * ESP_LOG line clearly identifies WHICH path triggered the stop —
 * makes ops triage on a misconfigured-token device much easier
 * than a generic "stop_ws" trace. */
static void _voice_auth_fail_stop_worker_fn(void *arg)
{
    (void)arg;
    if (s_ws) {
        ESP_LOGW(TAG, "auth_failed worker: stopping WS after %d consecutive 401s",
                 s_auth_fail_cnt);
        esp_websocket_client_stop(s_ws);
    }
}

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
    if (!t) {
        /* Wave 14 W14-M08: log the silent-drop so ops can see we
         * squeezed internal SRAM and a user-facing toast never reached
         * LVGL. */
        ESP_LOGW(TAG, "voice_async_toast OOM — dropping toast (text=%.40s)",
                 text ? text : "");
        free(text);
        return;
    }
    t->gen = s_session_gen;
    t->text = text;
    lv_async_call(async_show_toast_cb, t);
}

static void voice_async_refresh_badge(void)
{
    voice_async_badge_t *b = malloc(sizeof(voice_async_badge_t));
    if (!b) {
        ESP_LOGW(TAG, "voice_async_refresh_badge OOM — badge will lag a tick");
        return;
    }
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
            /* v4·D audit P1 fix: use bounded copy instead of unchecked
             * strcat.  Previously the guard compared cur+add+2 against
             * the buffer size, but under mutex-less concurrent writes
             * cur_len could change between the check and the strcat.
             * Take the state mutex and re-bound with snprintf so an
             * overflow is impossible. */
            if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            size_t cur_len = strlen(s_dictation_text);
            size_t remaining = (cur_len + 1 < DICTATION_TEXT_SIZE)
                               ? (DICTATION_TEXT_SIZE - cur_len - 1) : 0;
            if (remaining > 1) {
                const char *sep = (cur_len > 0) ? " " : "";
                snprintf(s_dictation_text + cur_len, remaining,
                         "%s%s", sep, text->valuestring);
            }
            if (s_state_mutex) xSemaphoreGive(s_state_mutex);
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
        /* Audit #80 DMA leak hunt (wave 9): log heap state at the 5
         * interesting boundaries of a chat turn (llm_done, tts_start,
         * tts_end, media, text_update) so we can diff which stage leaks.
         * Heap caps are DMA-capable internal SRAM — same pool the WiFi
         * Rx ring needs to stay alive. */
        ESP_LOGI(TAG, "TTS start — preparing playback | heap_dma_free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        tab5_audio_speaker_enable(true);
        playback_buf_reset();
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
            ESP_LOGD(TAG, "llm token ignored — state=%d (post-cancel late arrival)",
                     cur_for_llm);
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
        cJSON *msg       = cJSON_GetObjectItem(root, "message");
        cJSON *severity  = cJSON_GetObjectItem(root, "severity");
        cJSON *code      = cJSON_GetObjectItem(root, "code");
        const char *err_src  = cJSON_IsString(msg) ? msg->valuestring : "unknown";
        /* Default to "fatal" for unknown / pre-γ1 frames — safer to
         * surface in caption (more visible) than to silently toast.
         * Tab5 is forward-compatible with future severity values too:
         * anything not "transient" routes to caption. */
        const char *sev_src  = cJSON_IsString(severity) ? severity->valuestring : "fatal";
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
            playback_buf_reset();
            tab5_audio_speaker_enable(false);
            bool connected = (s_ws != NULL) && esp_websocket_client_is_connected(s_ws);
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
            if (strcmp(code_src, "device_evicted") == 0 && s_ws) {
                ESP_LOGW(TAG, "device_evicted: scheduling WS stop to prevent reconnect loop");
                s_disconnecting = true;
                tab5_worker_enqueue(_voice_stop_ws_worker_fn, NULL,
                                    "device_evicted_stop");
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
                const char *role = cJSON_GetStringValue(
                    cJSON_GetObjectItem(m, "role"));
                const char *content = cJSON_GetStringValue(
                    cJSON_GetObjectItem(m, "content"));
                if (!role || !content || !content[0]) continue;
                /* Skip system/tool rows — chat UI only renders
                 * user/assistant today. */
                if (strcmp(role, "user") != 0 &&
                    strcmp(role, "assistant") != 0) continue;
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
        const char *m = cJSON_IsString(msg) ? msg->valuestring
                                            : "Note saved — summary unavailable";
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
         * here so we don't fight the new caption. */
        ESP_LOGI(TAG, "Dictation post-process cancelled (superseded by new dictation)");
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
        ESP_LOGI(TAG, "LLM done (%.0fms) | heap_dma_free=%u largest=%u",
                 cJSON_IsNumber(ms) ? ms->valuedouble : 0.0,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        /* Prefer the full text field in llm_done (TC bypass uses it)
         * falling back to the accumulated streamed tokens. */
        cJSON *full = cJSON_GetObjectItem(root, "text");
        const char *bubble_text = s_llm_text;
        if (cJSON_IsString(full) && full->valuestring && full->valuestring[0]) {
            bubble_text = full->valuestring;
        }
        if (bubble_text && bubble_text[0]) {
            ui_chat_push_message("assistant", bubble_text);
        }
        /* v4·D TC polish: if no TTS is coming (TC bypass never sends
         * tts_start -- gateway is text-only), transition to READY
         * directly.  Without this, state sits in PROCESSING forever
         * after a TC text turn, chat input pill stays on "Thinking...",
         * voice overlay stays blocked. */
        if (s_state == VOICE_STATE_PROCESSING) {
            voice_set_state(VOICE_STATE_READY, "llm_done");
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
        /* Phase 3c: accumulate into the daily spend counter. NVS write is
         * serialised by the settings mutex; this runs in the voice WS rx
         * task so the LVGL thread won't block. */
        if (m > 0) {
            tab5_budget_accumulate((uint32_t)m);
            uint32_t spent = tab5_budget_get_today_mils();
            uint32_t cap   = tab5_budget_get_cap_mils();
            ESP_LOGI(TAG, "Budget: today=%lu mils / cap=%lu mils",
                     (unsigned long)spent, (unsigned long)cap);
            /* Nudge home to redraw its live-line spend readout. */
            extern void ui_home_update_status(void);
            lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);

            /* Phase 3e auto-downgrade: once spent >= cap AND we're in a
             * cloud-cost-bearing mode (1=Hybrid or 2=Full Cloud), flip
             * back to Local (voice_mode=0) + notify Dragon + surface a
             * toast so the user knows why their next turn is free.
             * Agent (mode 3) goes via TinkerClaw which has its own
             * billing -- we don't auto-change it here. */
            if (cap > 0 && spent >= cap) {
                uint8_t cur = tab5_settings_get_voice_mode();
                if (cur == 1 || cur == 2) {
                    ESP_LOGW(TAG, "Budget cap reached -- auto-downgrading %d -> 0 (Local)", cur);
                    tab5_settings_set_voice_mode(0);
                    /* Clear llm_model override so Dragon reverts to local.
                     * Leaving the NVS llm_model alone keeps the user's
                     * cloud preference for when they raise the cap. */
                    char lm[64] = {0};
                    tab5_settings_get_llm_model(lm, sizeof(lm));
                    /* G7-F: tag this downgrade so Dragon speaks a short TTS
                     * alert -- the user hears the switch even with the screen
                     * off. */
                    voice_send_config_update_ex(0, lm, "cap_downgrade");
                    /* Also reset the three Sovereign tiers so the mode
                     * sheet visually reflects the downgrade. */
                    tab5_settings_set_int_tier(0);
                    tab5_settings_set_voi_tier(0);
                    extern void ui_home_show_toast(const char *text);
                    lv_async_call((lv_async_cb_t)ui_home_show_toast,
                                  (void *)"Budget cap hit — Local mode");
                }
            }
        }
        /* Phase 3d + 4a: attach the receipt to the most-recent assistant
         * bubble in the chat store so chat_msg_view can render a per-turn
         * stamp.  Attach regardless of cost_mils so LOCAL turns (Ollama
         * qwen3, cost=0) also get a "qwen3 · FREE" stamp -- engine-used
         * transparency on every bubble, not just billable ones. */
        cJSON *retried_j = cJSON_GetObjectItem(root, "retried");
        bool retried = cJSON_IsTrue(retried_j);
        /* v4·D audit P1 fix: always stamp the bubble, even when model is
         * missing -- a blank model still tells the user the turn finished
         * and the cost was zero.  Previously the null/empty-model path
         * silently swallowed the receipt and the user never saw any
         * stamp on a local turn that Dragon forgot to name. */
        {
            char short_model[16] = {0};
            const char *mp = (model && model[0]) ? model : "local";
            const char *slash = strchr(mp, '/');
            const char *tail = slash ? slash + 1 : mp;
            const char *hyphen = strchr(tail, '-');  /* skip vendor prefix "claude-" */
            const char *start = hyphen ? hyphen + 1 : tail;
            snprintf(short_model, sizeof(short_model), "%s", start);
            /* Defer the attach onto the LVGL thread.  ui_chat_push_message
             * ("assistant", …) from llm_done enqueues via lv_async_call a
             * few ms before this handler runs; if we attach synchronously
             * here (on the voice WS rx task) the assistant bubble is not
             * yet in chat_store, attach returns -1, and the stamp is
             * silently dropped.  Queuing via lv_async_call after the push
             * preserves FIFO order — push lands first, attach lands
             * second — and both run on the LVGL thread. */
            extern void voice_defer_receipt_attach(uint32_t mils,
                                                    uint16_t ptok,
                                                    uint16_t ctok,
                                                    const char *model_short,
                                                    bool retried);
            voice_defer_receipt_attach((uint32_t)m, (uint16_t)pt,
                                        (uint16_t)ct, short_model, retried);
        }
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
        ESP_LOGI(TAG, "Vision capability: %s (model=%s, per_frame=%d mils)",
                 s_vision_capable ? "YES" : "no",
                 s_vision_model[0] ? s_vision_model : "none",
                 s_vision_per_frame_mils);
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
        /* Wave 14 W14-L03: if neither voice_mode nor cloud_mode was
         * present, the handler used to exit silently and Tab5 would
         * disagree with Dragon about the active mode with no trace.
         * Log at DEBUG so it's visible via /log without spamming INFO. */
        if (!cJSON_IsNumber(vmode) && !applied_cloud &&
            !cJSON_IsString(error)) {
            ESP_LOGD(TAG, "config_update received with no voice_mode/"
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
        snprintf(done_buf, sizeof(done_buf), "%s done (%.0fms)",
                 tool_name, ms);
        ui_chat_push_system(done_buf);
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
    } else if (strcmp(type_str, "widget_card") == 0) {
        /* Audit B2 (2026-04-20): widget_card from Tab5Surface.card()
         * is a different shape than the legacy "card" message used by
         * rich-media turns — title + body + tone + optional image_url.
         * Route it to the same chat bubble renderer so skills can push
         * context cards into the conversation. */
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
        const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
        const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(root, "image_url"));
        if (title) {
            ESP_LOGI(TAG, "widget_card: %s", title);
            /* chat_push_card takes (title, subtitle, image_url, description).
             * Map body → description for read-order; subtitle stays NULL. */
            ui_chat_push_card(title, NULL, img, body);
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
    } else if (strcmp(type_str, "widget_list") == 0) {
        /* v4·D Phase 4c: ranked list widget.  Same upsert shape as
         * widget_live but with an "items" array carrying up to 5 rows
         * of {text, value} displayed stacked on the home live slot.
         * Skills like web_search, memory recall, daily agenda, etc.
         * should emit this instead of cramming rows into a body blob. */
        extern widget_t *widget_store_upsert(const widget_t *in);
        extern widget_tone_t widget_tone_from_str(const char *s);
        extern void ui_home_update_status(void);
        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
        if (!cid) {
            ESP_LOGW(TAG, "widget_list missing card_id");
        } else {
            widget_t w = {0};
            strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
            const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "skill_id"));
            if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
            const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
            if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
            const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
            if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
            const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
            w.tone = widget_tone_from_str(tone_s);
            cJSON *pri = cJSON_GetObjectItem(root, "priority");
            w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
            cJSON *items = cJSON_GetObjectItem(root, "items");
            if (cJSON_IsArray(items)) {
                int cnt = cJSON_GetArraySize(items);
                if (cnt > WIDGET_LIST_MAX_ITEMS) cnt = WIDGET_LIST_MAX_ITEMS;
                for (int i = 0; i < cnt; i++) {
                    cJSON *it = cJSON_GetArrayItem(items, i);
                    if (!cJSON_IsObject(it)) continue;
                    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
                    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "value"));
                    if (t) strncpy(w.items[i].text,  t, WIDGET_LIST_ITEM_TEXT_LEN - 1);
                    if (v) strncpy(w.items[i].value, v, WIDGET_LIST_ITEM_VALUE_LEN - 1);
                }
                w.items_count = (uint8_t)cnt;
            }
            w.type = WIDGET_TYPE_LIST;
            widget_store_upsert(&w);
            lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
            ESP_LOGI(TAG, "widget_list upsert: %s/%s items=%u",
                     w.skill_id, cid, w.items_count);
        }
    } else if (strcmp(type_str, "widget_chart") == 0) {
        /* v4·D Phase 4f: mini bar chart widget.  Same upsert shape as
         * widget_list; "values" array carries up to 12 floats, optional
         * "max" bound for normalization, optional "body" for a summary
         * line below the bars. */
        extern widget_t *widget_store_upsert(const widget_t *in);
        extern widget_tone_t widget_tone_from_str(const char *s);
        extern void ui_home_update_status(void);
        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
        if (!cid) {
            ESP_LOGW(TAG, "widget_chart missing card_id");
        } else {
            widget_t w = {0};
            strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
            const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "skill_id"));
            if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
            const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
            if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
            const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
            if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
            const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
            w.tone = widget_tone_from_str(tone_s);
            cJSON *pri = cJSON_GetObjectItem(root, "priority");
            w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
            cJSON *mx = cJSON_GetObjectItem(root, "max");
            w.chart_max = cJSON_IsNumber(mx) ? (float)mx->valuedouble : 0.0f;
            cJSON *vals = cJSON_GetObjectItem(root, "values");
            if (cJSON_IsArray(vals)) {
                int cnt = cJSON_GetArraySize(vals);
                if (cnt > WIDGET_CHART_MAX_POINTS) cnt = WIDGET_CHART_MAX_POINTS;
                for (int i = 0; i < cnt; i++) {
                    cJSON *v = cJSON_GetArrayItem(vals, i);
                    w.chart_values[i] = cJSON_IsNumber(v)
                                        ? (float)v->valuedouble : 0.0f;
                }
                w.chart_count = (uint8_t)cnt;
            }
            w.type = WIDGET_TYPE_CHART;
            widget_store_upsert(&w);
            lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
            ESP_LOGI(TAG, "widget_chart upsert: %s/%s pts=%u max=%.2f",
                     w.skill_id, cid, w.chart_count, w.chart_max);
        }
    } else if (strcmp(type_str, "widget_media") == 0) {
        /* v4·D Phase 4g: media widget (image + caption in the live slot). */
        extern widget_t *widget_store_upsert(const widget_t *in);
        extern widget_tone_t widget_tone_from_str(const char *s);
        extern void ui_home_update_status(void);
        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
        if (!cid) {
            ESP_LOGW(TAG, "widget_media missing card_id");
        } else {
            widget_t w = {0};
            strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
            const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "skill_id"));
            if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
            const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
            if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
            const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
            if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
            if (url) strncpy(w.media_url, url, WIDGET_MEDIA_URL_LEN - 1);
            const char *alt = cJSON_GetStringValue(cJSON_GetObjectItem(root, "alt"));
            if (alt) strncpy(w.media_alt, alt, WIDGET_MEDIA_ALT_LEN - 1);
            const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
            w.tone = widget_tone_from_str(tone_s);
            cJSON *pri = cJSON_GetObjectItem(root, "priority");
            w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
            w.type = WIDGET_TYPE_MEDIA;
            widget_store_upsert(&w);
            lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
            ESP_LOGI(TAG, "widget_media upsert: %s/%s alt=%s",
                     w.skill_id, cid, w.media_alt);
        }
    } else if (strcmp(type_str, "widget_prompt") == 0) {
        /* v4·D Phase 4g: multi-choice prompt widget.  Up to 3 choices;
         * Tab5 renders each as a button.  Tap fires widget_action. */
        extern widget_t *widget_store_upsert(const widget_t *in);
        extern widget_tone_t widget_tone_from_str(const char *s);
        extern void ui_home_update_status(void);
        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
        if (!cid) {
            ESP_LOGW(TAG, "widget_prompt missing card_id");
        } else {
            widget_t w = {0};
            strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
            const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "skill_id"));
            if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
            const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
            if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
            const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
            if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
            const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
            w.tone = widget_tone_from_str(tone_s);
            cJSON *pri = cJSON_GetObjectItem(root, "priority");
            w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 60;
            cJSON *choices = cJSON_GetObjectItem(root, "choices");
            if (cJSON_IsArray(choices)) {
                int cnt = cJSON_GetArraySize(choices);
                if (cnt > WIDGET_PROMPT_MAX_CHOICES) cnt = WIDGET_PROMPT_MAX_CHOICES;
                for (int i = 0; i < cnt; i++) {
                    cJSON *it = cJSON_GetArrayItem(choices, i);
                    if (!cJSON_IsObject(it)) continue;
                    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
                    const char *ev = cJSON_GetStringValue(cJSON_GetObjectItem(it, "event"));
                    if (t)  strncpy(w.choices[i].text,  t,  WIDGET_PROMPT_CHOICE_LEN - 1);
                    if (ev) strncpy(w.choices[i].event, ev, WIDGET_PROMPT_EVENT_LEN  - 1);
                }
                w.choices_count = (uint8_t)cnt;
            }
            w.type = WIDGET_TYPE_PROMPT;
            widget_store_upsert(&w);
            lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
            ESP_LOGI(TAG, "widget_prompt upsert: %s/%s choices=%u",
                     w.skill_id, cid, w.choices_count);
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
    /* v4·D audit P0 fix: snapshot s_state under the mutex so we never
     * race with voice_set_state on another task.  The checks below
     * formerly read s_state twice without locking -- second read could
     * see a newer value mid-TTS-frame. */
    voice_state_t cur;
    if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    cur = s_state;
    if (s_state_mutex) xSemaphoreGive(s_state_mutex);


    if (cur != VOICE_STATE_SPEAKING && cur != VOICE_STATE_PROCESSING) {
        return;
    }
    if (cur == VOICE_STATE_PROCESSING) {
        playback_buf_reset();
        voice_set_state(VOICE_STATE_SPEAKING, NULL);
    }

    /* Upsample into a stack-adjacent but reasonably-sized temp buffer. A single
     * Dragon TTS chunk is typically 640–1024 B (320–512 samples), so
     * worst-case output is 512 × 3 = 1536 samples = 3072 B. Cap at 2048
     * input samples to stay well under the managed-client buffer. */
    const size_t in_samples   = len / sizeof(int16_t);
    if (in_samples == 0) return;
    size_t max_out = in_samples * UPSAMPLE_RATIO;
    /* Wave 9 audit #80 + stability P0: use the pre-allocated upsample
     * buffer from voice_init instead of malloc/free on every chunk. */
    if (!s_upsample_buf) {
        ESP_LOGW(TAG, "handle_binary: upsample_buf not initialized");
        return;
    }
    if (max_out > UPSAMPLE_BUF_CAPACITY) {
        ESP_LOGW(TAG, "handle_binary: chunk too large (in=%zu out=%zu cap=%d) — truncating",
                 in_samples, max_out, UPSAMPLE_BUF_CAPACITY);
        max_out = UPSAMPLE_BUF_CAPACITY;
    }
    size_t out_samples = upsample_16k_to_48k((const int16_t *)data, in_samples,
                                              s_upsample_buf, max_out);
    playback_buf_write(s_upsample_buf, out_samples);
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
        esp_websocket_client_set_reconnect_timeout(s_ws, WS_CLIENT_BACKOFF_MIN_MS);

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
            voice_state_t cur = s_state;
            if ((cur == VOICE_STATE_PROCESSING || cur == VOICE_STATE_SPEAKING)
                && !s_disconnecting) {
                extern void ui_home_show_toast(const char *text);
                extern void ui_home_pulse_orb_alert(void);
                lv_async_call((lv_async_cb_t)ui_home_show_toast,
                              (void *)"Dragon dropped mid-turn - reconnecting");
                lv_async_call((lv_async_cb_t)ui_home_pulse_orb_alert, NULL);
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
            uint32_t delay_ms = half + jitter;    /* [exp/2, exp) */
            ESP_LOGI(TAG, "WS: reconnect attempt=%d backoff=%lums (exp=%lums)",
                     s_connect_attempt, (unsigned long)delay_ms,
                     (unsigned long)exp_ms);
            esp_websocket_client_set_reconnect_timeout(s_ws, delay_ms);
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
            ESP_LOGW(TAG, "WS: %d failed attempts — escalating to hard-kick "
                          "(stack may be wedged despite probe success)",
                     s_connect_attempt);
            esp_err_t kr = tab5_wifi_hard_kick();
            if (kr != ESP_OK) {
                /* ESP-Hosted's Wi-Fi slave chip doesn't always recover
                 * from stop/start without a host-side reboot (observed:
                 * esp_wifi_start returns ESP_FAIL repeatedly).  Don't
                 * burn another 5 attempts — reboot now. */
                ESP_LOGE(TAG, "WS: hard-kick failed (%s) — controlled reboot",
                         esp_err_to_name(kr));
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
        if (s_ws && !s_disconnecting) {
            s_connect_attempt++;
            int shift = s_connect_attempt - 1;
            if (shift < 0) shift = 0;
            if (shift > 5) shift = 5;
            uint32_t exp_ms = WS_CLIENT_BACKOFF_MIN_MS << shift;
            if (exp_ms > WS_CLIENT_BACKOFF_CAP_MS) exp_ms = WS_CLIENT_BACKOFF_CAP_MS;
            uint32_t half = exp_ms / 2;
            uint32_t jitter = half > 0 ? (esp_random() % half) : 0;
            uint32_t delay_ms = half + jitter;
            ESP_LOGI(TAG, "WS: CLOSED reconnect attempt=%d backoff=%lums",
                     s_connect_attempt, (unsigned long)delay_ms);
            esp_websocket_client_set_reconnect_timeout(s_ws, delay_ms);
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
            esp_websocket_client_set_reconnect_timeout(s_ws, half + jitter);
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
            ESP_LOGW(TAG, "WS: 401 auth_failed (count=%d/%d)",
                     s_auth_fail_cnt, WS_AUTH_FAIL_THRESHOLD);
            if (s_auth_fail_cnt >= WS_AUTH_FAIL_THRESHOLD && !s_disconnecting) {
                ESP_LOGE(TAG, "WS: %d consecutive 401s — stopping retry loop",
                         s_auth_fail_cnt);
                s_disconnecting = true;
                /* Hop to worker task — esp_websocket_client_stop()
                 * rejects calls from the WS task itself.  Same pattern
                 * as the γ2-H8 device_evicted handler. */
                tab5_worker_enqueue(_voice_auth_fail_stop_worker_fn, NULL,
                                    "auth_failed_stop");
                /* Toast routes to LVGL via voice_async_toast (existing
                 * helper used elsewhere in the file).  strdup so the
                 * buffer outlives this stack frame. */
                char *toast = strdup("Invalid Dragon token — check Settings");
                if (toast) {
                    voice_async_toast(toast);
                }
                break;
            }
        }

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
    if (!tdm_buf || !mono_buf) {
        ESP_LOGE(TAG, "Failed to allocate mic buffers");
        heap_caps_free(tdm_buf);
        heap_caps_free(mono_buf);
        s_mic_task = NULL;
        vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
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

    while (s_mic_running && (ws_live || s_voice_mode == VOICE_MODE_DICTATE)) {
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

        for (int i = 0; i + DOWNSAMPLE_RATIO - 1 < MIC_48K_FRAMES && out_idx < VOICE_CHUNK_SAMPLES; i += DOWNSAMPLE_RATIO) {
            int32_t sum = 0;
            for (int j = 0; j < DOWNSAMPLE_RATIO; j++) {
                sum += tdm_buf[(i + j) * MIC_TDM_CHANNELS + MIC_TDM_MIC1_OFF];
            }
            mono_buf[out_idx++] = (int16_t)(sum / DOWNSAMPLE_RATIO);
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

        ui_notes_write_audio(mono_buf, out_idx);

        size_t send_bytes = out_idx * sizeof(int16_t);
        if (ws_live) {
            err = voice_ws_send_binary(mono_buf, send_bytes);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed — continuing SD recording");
                if (s_voice_mode == VOICE_MODE_ASK) break;
            }
        }
        frames_sent++;

        if (s_voice_mode == VOICE_MODE_ASK && frames_sent >= MAX_RECORD_FRAMES_ASK) {
            ESP_LOGI(TAG, "Max recording duration reached (%ds)",
                     MAX_RECORD_FRAMES_ASK * TAB5_VOICE_CHUNK_MS / 1000);
            voice_set_state(VOICE_STATE_LISTENING, "max_duration");
            break;
        }

        /* Wave 15 W15-P02: compute RMS for BOTH Ask and Dictate modes
         * so the voice overlay can render a live "listening back" glow
         * on the orb driven by voice_get_current_rms().  Previously RMS
         * was gated to DICTATE and the UI orb stayed static during
         * normal Ask turns, which made the overlay feel unresponsive.
         * The expensive VAD-driven auto-stop logic below is still
         * scoped to DICTATE (the only mode that uses it). */
        if (out_idx > 0) {
            int64_t sqsum = 0;
            for (int k = 0; k < out_idx; k++) {
                sqsum += (int64_t)mono_buf[k] * mono_buf[k];
            }
            float rms = sqrtf((float)(sqsum / out_idx));
            s_current_rms = rms;
        }

        if (s_voice_mode == VOICE_MODE_DICTATE) {
            /* Re-read the just-computed RMS rather than recomputing. */
            float rms = s_current_rms;

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

    if (s_voice_mode == VOICE_MODE_DICTATE && had_speech
        && total_silence_frames >= DICTATION_AUTO_STOP_FRAMES
        && s_ws && esp_websocket_client_is_connected(s_ws)) {
        voice_ws_send_text("{\"type\":\"stop\"}");
        voice_set_state(VOICE_STATE_PROCESSING, NULL);
    }

    ESP_LOGI(TAG, "Mic capture task exiting");
    s_mic_task = NULL;
    vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
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
        vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
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
    vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
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

    /* Wave 9 #80 fix: pre-allocate the TTS upsample buffer once. See
     * handle_binary_message for why — per-chunk malloc churn was a PSRAM
     * fragmentation source and contributed to the DMA-starve spiral. */
    s_upsample_buf = (int16_t *)heap_caps_malloc(
        UPSAMPLE_BUF_CAPACITY * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_upsample_buf) {
        ESP_LOGE(TAG, "Failed to allocate upsample buffer in PSRAM");
        heap_caps_free(s_play_buf);
        s_play_buf = NULL;
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

    /* v4·D "fix once and for all": ngrok→LAN recovery probe task.
     *
     * The flakiness chain:
     *   1. Tab5 + Dragon co-boot; Dragon WS isn't listening yet
     *   2. Tab5 handshake fails 2x -> flip s_using_ngrok=true, swap URI
     *   3. Ngrok reaches Dragon (eventually) but stays on 400ms RTT
     *   4. Previous fix cleared s_handshake_fail_cnt on disconnect, but
     *      never cleared s_using_ngrok or swapped back to LAN -- so any
     *      Dragon hiccup that dumps us onto ngrok was permanent until
     *      reboot.
     *
     * The task opens a raw TCP connect to the configured LAN host
     * every 30 s.  If it succeeds (Dragon reachable), we stop the WS
     * client, swap the URI back to LAN, and restart it.  Scheduling
     * stop/start from a non-event task avoids the documented "client
     * stops itself from its own callback" race.
     */
    {
        extern void voice_lan_probe_task(void *arg);
        BaseType_t ok = xTaskCreatePinnedToCore(
            voice_lan_probe_task, "voice_lan_probe",
            4096, NULL, 2, NULL, 1);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "Failed to start LAN probe task");
        }
    }

    s_initialized = true;
    voice_set_state(VOICE_STATE_IDLE, NULL);
    ESP_LOGI(TAG, "Voice module initialized");
    return ESP_OK;
}

/* v4·D connectivity audit T1.3: link-health probe.
 *
 * Runs on Core 1 at priority 2, sleeps 30 s between passes.  Probes
 * BOTH LAN and ngrok every time so voice_get_link_health() + the home
 * pill always reflect reality ("Dragon unreachable" vs "just
 * reconnecting" vs "remote mode").  When we're on ngrok in auto mode
 * and the LAN probe succeeds, hot-swaps the WS client back to LAN.
 * Doing the stop/set_uri/start from this task (not the WS event
 * handler) avoids the documented "client stops itself from its own
 * callback" race.  Uses a raw TCP socket with 1500 ms connect timeout
 * so we don't block under WiFi weather. */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <fcntl.h>
#include <errno.h>

static bool probe_tcp_connect(const char *host, uint16_t port)
{
    /* #146: SO_RCVTIMEO / SO_SNDTIMEO do NOT affect connect() on lwIP.
     * A blocking connect to a wedged Wi-Fi stack hangs ~75 s before the
     * TCP SYN timeout fires, which starved the probe task and prevented
     * the zombie-kick escalation from running on schedule.  Use
     * non-blocking connect + select() so each probe returns in ≤ 1.5 s
     * regardless of stack state. */
    if (!host || !host[0] || port == 0) return false;
    struct addrinfo hints = { .ai_family = AF_INET,
                               .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_s[8]; snprintf(port_s, sizeof(port_s), "%u", (unsigned)port);
    int gai = getaddrinfo(host, port_s, &hints, &res);
    if (gai != 0 || !res) {
        if (res) freeaddrinfo(res);
        return false;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return false; }

    /* Non-blocking connect via fcntl + select. */
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);

    bool ok = false;
    int cr = connect(s, res->ai_addr, res->ai_addrlen);
    if (cr == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
        int n = select(s + 1, NULL, &wfds, NULL, &tv);
        if (n > 0 && FD_ISSET(s, &wfds)) {
            int soerr = 0; socklen_t lerr = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &lerr);
            ok = (soerr == 0);
        }
    }
    close(s);
    freeaddrinfo(res);
    return ok;
}

void voice_lan_probe_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Link probe task started (both LAN + ngrok every 30 s)");
    /* #146: progressive-escalation zombie detector.  Previously we only
     * had one lever (soft kick = esp_wifi_disconnect+connect) which
     * didn't recover when the ESP-Hosted SDIO driver itself was wedged.
     * Now we escalate:
     *   Round 2 (60 s):  soft kick — deauth + reconnect, recovers
     *                     AP-side black-holing.
     *   Round 4 (120 s): hard kick — esp_wifi_stop()/start(), recovers
     *                     dead host driver + frees internal SRAM buffers.
     *   Round 6 (180 s): controlled esp_restart() — full clean boot.
     * The largest-free internal SRAM block is logged every probe so
     * we can correlate Wi-Fi flap with allocator pressure. */
    int zombie_rounds = 0;
    const int ZOMBIE_SOFT   = 2;
    const int ZOMBIE_HARD   = 4;
    const int ZOMBIE_REBOOT = 6;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!tab5_wifi_connected()) {
            s_lan_tcp_ok   = false;
            s_ngrok_tcp_ok = false;
            s_last_probe_ms = lv_tick_get();
            zombie_rounds = 0;   /* fresh start once Wi-Fi is back */
            continue;
        }

        /* Probe LAN target from NVS (honors user-configured dragon_host). */
        char lan_host[64] = {0};
        tab5_settings_get_dragon_host(lan_host, sizeof(lan_host));
        uint16_t lan_port = tab5_settings_get_dragon_port();
        bool lan_ok = probe_tcp_connect(lan_host, lan_port ? lan_port : 3502);

        /* Probe ngrok unconditionally so we can tell the user "remote
         * is reachable" even when on LAN.  Uses TLS port 443. */
        bool ngrok_ok = probe_tcp_connect(TAB5_NGROK_HOST, TAB5_NGROK_PORT);

        s_lan_tcp_ok   = lan_ok;
        s_ngrok_tcp_ok = ngrok_ok;
        s_last_probe_ms = lv_tick_get();
        size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Link probe: lan[%s]=%d ngrok=%d (current=%s) zombie_rounds=%d int_largest=%uB",
                 lan_host, lan_ok, ngrok_ok,
                 s_using_ngrok ? "ngrok" : "lan", zombie_rounds,
                 (unsigned)int_largest);

        /* #146 progressive escalation: both probes failed while Wi-Fi
         * reports associated — network stack is wedged.  Escalate
         * based on how many consecutive rounds have failed.
         *
         * #159: BUT — if the voice WebSocket is currently connected, the
         * WiFi stack is provably alive: Tab5 is actively exchanging
         * frames with Dragon on port 3502.  A TCP-probe timeout under
         * that condition is almost always transient back-pressure (LVGL
         * render storm starving the probe task, SDIO queue full from a
         * screenshot, debug-server httpd handler hogging the stack)
         * rather than genuine WiFi failure.  Rebooting the device at
         * that point is a false positive and destroys the user's
         * session for no reason.  Reset the counter instead and let the
         * next round re-probe with clean state.
         *
         * This fix is specifically for the "every ~3 min under stress"
         * reboot cluster we saw in the 30-min stability test — 9 SW
         * resets, each at zombie_rounds=6, while the WS stayed
         * connected throughout. */
        if (!lan_ok && !ngrok_ok && !voice_is_connected()) {
            zombie_rounds++;
            if (zombie_rounds >= ZOMBIE_REBOOT) {
                ESP_LOGE(TAG, "Zombie Wi-Fi: %d rounds (~%d s) failed even "
                              "after hard kick — controlled reboot",
                         zombie_rounds, zombie_rounds * 30);
                /* Give the log buffer a moment to flush before reboot. */
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                /* unreachable */
            } else if (zombie_rounds >= ZOMBIE_HARD) {
                ESP_LOGW(TAG, "Zombie Wi-Fi: %d rounds — escalating to HARD kick "
                              "(esp_wifi_stop/start)", zombie_rounds);
                esp_err_t r = tab5_wifi_hard_kick();
                if (r != ESP_OK) {
                    ESP_LOGE(TAG, "Hard kick returned %s — next round reboots",
                             esp_err_to_name(r));
                }
                /* Keep zombie_rounds ticking so the reboot fires if
                 * the hard kick didn't recover.  Not reached when the
                 * voice WS is alive — the outer check short-circuited
                 * at the top of this block (see #159). */
            } else if (zombie_rounds >= ZOMBIE_SOFT) {
                ESP_LOGW(TAG, "Zombie Wi-Fi detected: both probes failed "
                              "%d rounds in a row — soft kick (deauth+reconnect)",
                         zombie_rounds);
                tab5_wifi_kick();
            }
        } else {
            zombie_rounds = 0;
        }

        /* Auto-mode: if we're stuck on ngrok but LAN is reachable, swap back. */
        uint8_t conn_mode = tab5_settings_get_connection_mode();
        if (conn_mode == 0 && s_using_ngrok && lan_ok) {
            char lan_uri[160];
            snprintf(lan_uri, sizeof(lan_uri),
                     "ws://%s:%u%s",
                     lan_host, (unsigned)(lan_port ? lan_port : 3502),
                     TAB5_VOICE_WS_PATH);
            ESP_LOGI(TAG, "Link probe: swapping ngrok -> LAN (%s)", lan_uri);
            esp_websocket_client_stop(s_ws);
            esp_websocket_client_set_uri(s_ws, lan_uri);
            strncpy(s_dragon_host, lan_host, sizeof(s_dragon_host) - 1);
            s_dragon_host[sizeof(s_dragon_host) - 1] = '\0';
            s_dragon_port = lan_port ? lan_port : 3502;
            s_using_ngrok = false;
            s_handshake_fail_cnt = 0;
            s_connect_attempt = 0;
            esp_websocket_client_start(s_ws);
        }
    }
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

    /* Wave 14 W14-C04: attach `Authorization: Bearer <token>` to the
     * WS upgrade request.  Dragon's /ws/voice handler validates this
     * against server.api_token (DRAGON_API_TOKEN env on Dragon) before
     * ws.prepare().  Without the header, Dragon 401s the upgrade.
     *
     * `headers` is a CRLF-separated string of "Header: value\r\n" pairs
     * per esp_websocket_client docs.  Static buffer so it outlives this
     * function (the client stores the pointer, not a copy). */
    static char s_ws_headers[128];
    if (TAB5_DRAGON_TOKEN && TAB5_DRAGON_TOKEN[0] &&
        strcmp(TAB5_DRAGON_TOKEN, "CHANGEME_SET_IN_SDKCONFIG_LOCAL") != 0) {
        snprintf(s_ws_headers, sizeof(s_ws_headers),
                 "Authorization: Bearer %s\r\n", TAB5_DRAGON_TOKEN);
    } else {
        s_ws_headers[0] = '\0';
        ESP_LOGW(TAG, "TAB5_DRAGON_TOKEN not configured — WS upgrade will "
                      "fail if Dragon has DRAGON_API_TOKEN set.");
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
        .headers                = (s_ws_headers[0] ? s_ws_headers : NULL),
        /* v4·D connectivity audit -- ROOT CAUSE FIX #2.
         *
         * Enable TCP-level keepalive on the underlying socket so we
         * detect half-open connections at the OS layer rather than
         * waiting for the next app-level write to fail.  With no
         * keepalive, a WiFi blip that silently breaks the TCP 4-tuple
         * mapping on the AP leaves both sides thinking the socket is
         * alive until the 180 s WS pong timeout finally fires -- hence
         * the long mysterious "stuck connecting" windows.
         *
         * Values: idle 10 s, probe every 5 s, 3 probes -> dead socket
         * detected in ~25 s even if the app path is quiet. */
        .keep_alive_enable      = true,
        .keep_alive_idle        = 10,
        .keep_alive_interval    = 5,
        .keep_alive_count       = 3,
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
        /* Wait for WS + session_start → READY (up to 15s).
         * Wave 13 H1: read s_state through voice_get_state() so the poll
         * is torn-read-safe on multi-core ESP32-P4. */
        for (int i = 0; i < 150 && voice_get_state() != VOICE_STATE_READY; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (voice_get_state() == VOICE_STATE_READY) {
            ESP_LOGI(TAG, "async_connect_task: calling voice_start_listening (auto_listen)");
            voice_start_listening();
        } else {
            ESP_LOGW(TAG, "async_connect_task: never reached READY, skipping auto_listen");
        }
    }

    free(args);
    vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
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
    /* Wave 13 H1: snapshot state once under the mutex so the log and the
     * guard below see the same value, and neither races the WS RX callback. */
    voice_state_t cur_state = voice_get_state();
    ESP_LOGI(TAG, "voice_start_listening: initialized=%d, ws_live=%d, state=%d",
             s_initialized, ws_live, cur_state);

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
    if (cur_state != VOICE_STATE_READY) {
        ESP_LOGW(TAG, "Cannot start listening in state %d", cur_state);
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
    /* Wave 13 H1: read s_state under mutex so WS RX callback can't flip
     * us into IDLE mid-check and then have voice_set_state lose a wakeup. */
    if (voice_get_state() == VOICE_STATE_IDLE) {
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

/* Wave 14 W14-M01: raw-pointer getters kept for back-compat with
 * same-task readers (main.c:184 logs them synchronously).  Prefer
 * voice_get_*_copy below when the caller runs on a task that races
 * with the WS RX path (debug_server handlers, UI thread).  The
 * pointer-returning variants can observe a half-written strcat
 * because writers mutate these buffers under s_state_mutex. */
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

/* Wave 14 W14-M01: copy-under-mutex variants.  `buf` receives a
 * NUL-terminated snapshot; returns true on success, false on invalid
 * args.  An empty source copies the empty string (still returns true). */
static bool _voice_copy_under_lock(const char *src, char *buf, size_t len)
{
    if (!buf || len == 0) return false;
    if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strncpy(buf, src, len - 1);
    buf[len - 1] = '\0';
    if (s_state_mutex) xSemaphoreGive(s_state_mutex);
    return true;
}

bool voice_get_last_transcript_copy(char *buf, size_t len)
{
    return _voice_copy_under_lock(s_transcript, buf, len);
}

bool voice_get_stt_text_copy(char *buf, size_t len)
{
    return _voice_copy_under_lock(s_stt_text, buf, len);
}

bool voice_get_llm_text_copy(char *buf, size_t len)
{
    return _voice_copy_under_lock(s_llm_text, buf, len);
}

esp_err_t voice_send_text(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;

    /* Wave 13 H1: snapshot state once so the LISTENING branch and the
     * PROCESSING/SPEAKING branch below reason about the same tick. */
    voice_state_t cur_state = voice_get_state();
    if (cur_state == VOICE_STATE_LISTENING) {
        ESP_LOGI(TAG, "voice_send_text: cancelling active LISTENING to allow text send");
        voice_cancel();
        cur_state = voice_get_state();
    }
    if (cur_state == VOICE_STATE_PROCESSING || cur_state == VOICE_STATE_SPEAKING) {
        /* v4·D Gauntlet G1: queue the text instead of rejecting, so the
         * user can stack up a follow-up while the current turn is still
         * playing.  Single-slot queue -- a 2nd stash overwrites the 1st;
         * the voice overlay surfaces the queue depth as "+1 QUEUED". */
        if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        strncpy(s_queued_text, text, MAX_TRANSCRIPT_LEN - 1);
        s_queued_text[MAX_TRANSCRIPT_LEN - 1] = '\0';
        s_queue_pending = true;
        if (s_state_mutex) xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "voice_send_text: queued (pipeline busy state=%d): %s",
                 cur_state, text);
        /* Poke the voice overlay caption so the badge updates immediately. */
        if (s_state_cb) {
            if (tab5_ui_try_lock(100)) {
                s_state_cb(s_state, NULL);   /* re-render current state */
                tab5_ui_unlock();
            }
        }
        return ESP_OK;
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

esp_err_t voice_send_config_update_ex(int voice_mode, const char *llm_model,
                                      const char *reason)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "config_update");
    cJSON_AddNumberToObject(msg, "voice_mode", voice_mode);
    cJSON_AddBoolToObject(msg, "cloud_mode", voice_mode >= 1);
    if (voice_mode == 2 && llm_model && llm_model[0]) {
        cJSON_AddStringToObject(msg, "llm_model", llm_model);
    }
    if (reason && reason[0]) {
        cJSON_AddStringToObject(msg, "reason", reason);
    }
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Sending config_update: voice_mode=%d llm_model=%s reason=%s",
             voice_mode, (llm_model && llm_model[0]) ? llm_model : "(local)",
             (reason && reason[0]) ? reason : "(none)");
    esp_err_t ret = voice_ws_send_text(json);
    cJSON_free(json);
    return ret;
}

esp_err_t voice_send_config_update(int voice_mode, const char *llm_model)
{
    return voice_send_config_update_ex(voice_mode, llm_model, NULL);
}

float voice_get_current_rms(void)
{
    return s_current_rms;
}

/* v4·D Gauntlet G1: expose queue depth so the voice overlay can render
 * a "+N QUEUED" badge while the current turn is busy.  Single-slot
 * queue for now, so return value is 0 or 1. */
int voice_get_queue_depth(void)
{
    return s_queue_pending ? 1 : 0;
}

void voice_get_link_health(voice_link_health_t *out)
{
    if (!out) return;
    out->lan_tcp_ok     = s_lan_tcp_ok;
    out->ngrok_tcp_ok   = s_ngrok_tcp_ok;
    out->using_ngrok    = s_using_ngrok;
    out->last_probe_ms  = s_last_probe_ms;
    out->connect_attempt = s_connect_attempt;
}

/* v4·D connectivity audit T1.2: human-readable status string so the
 * home pill always tells the truth.  Ordered worst→best so the first
 * matching branch wins.  Returns NULL on healthy connected session. */
const char *voice_get_degraded_reason(void)
{
    if (!s_initialized)                 return "Voice off";
    if (!tab5_wifi_connected())         return "WiFi offline";
    switch (s_state) {
        case VOICE_STATE_READY:
        case VOICE_STATE_LISTENING:
        case VOICE_STATE_PROCESSING:
        case VOICE_STATE_SPEAKING:
            /* Healthy, but surface ngrok as a "remote mode" cue so user
             * knows their latency is going through the tunnel. */
            return s_using_ngrok ? "Remote (ngrok)" : NULL;
        case VOICE_STATE_CONNECTING:
            return "Connecting...";
        case VOICE_STATE_RECONNECTING:
            if (s_connect_attempt > 3 && !s_lan_tcp_ok && !s_ngrok_tcp_ok) {
                return "Dragon unreachable";
            }
            return "Reconnecting...";
        case VOICE_STATE_IDLE:
        default:
            return tab5_wifi_connected() ? "Offline" : "WiFi offline";
    }
}

bool voice_get_vision_capability(char *model_out, size_t model_len,
                                 int *per_frame_mils_out)
{
    if (model_out && model_len > 0) {
        snprintf(model_out, model_len, "%s", s_vision_model);
    }
    if (per_frame_mils_out) {
        *per_frame_mils_out = s_vision_per_frame_mils;
    }
    return s_vision_capable;
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
