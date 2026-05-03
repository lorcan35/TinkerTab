/**
 * Voice WebSocket protocol layer (Tab5 ↔ Dragon) — implementation.
 *
 * Wave 23 SOLID-audit closure for TT #331 (extract A1).  See
 * voice_ws_proto.h for the layer contract.  Pre-extract this all
 * lived inline in voice.c at L525-1948.
 */
#include "voice_ws_proto.h"

#include "voice.h"   /* g_voice_ws extern + voice_set_state */

#include <limits.h>  /* INT_MAX (W14-L02 send-bound check) */
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"        /* lv_tick_get for the widget_action rate limiter */
#include "settings.h"
#include "voice_codec.h" /* VOICE_CODEC_OPUS_UPLINK_ENABLED */

static const char *TAG = "tab5_voice_ws";

/* US-C04: counter for audio frames dropped under WS back-pressure.
 * Internal to voice_ws_send_binary; pre-extract this was a file-static
 * in voice.c.  No external readers — kept here so the dropped-frame
 * accounting stays colocated with the sender that increments it. */
static int s_audio_drop_count = 0;

// ---------------------------------------------------------------------------
// WebSocket send helpers (wrapping esp_websocket_client_send_*)
// ---------------------------------------------------------------------------
esp_err_t voice_ws_send_text(const char *msg)
{
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

esp_err_t voice_ws_send_binary(const void *data, size_t len)
{
    if (!g_voice_ws) return ESP_ERR_INVALID_STATE;
    if (!esp_websocket_client_is_connected(g_voice_ws)) return ESP_ERR_INVALID_STATE;
    if (!data) return ESP_ERR_INVALID_ARG;
    /* W14-L02: same bound as text. */
    if (len > INT_MAX) return ESP_ERR_INVALID_ARG;

    /* Short 100 ms timeout — we drop frames under pressure rather than
     * block the mic task. If WiFi stalls, I2S DMA would overflow. */
    int w = esp_websocket_client_send_bin(g_voice_ws, (const char *)data, (int)len,
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

/* #266: thin public wrapper for voice_video.c (and any future binary
 * sender).  Behavior is identical to voice_ws_send_binary — same
 * 100 ms send timeout, same drop accounting. */
esp_err_t voice_ws_send_binary_public(const void *data, size_t len)
{
    return voice_ws_send_binary(data, len);
}

// ---------------------------------------------------------------------------
// Device registration — sent as FIRST text frame from the CONNECTED handler.
// This is the #76 fix: register is sent from inside the event handler, AFTER
// the client's event task is running and ready to dispatch the server's
// session_start reply. The legacy code sent register synchronously before
// spawning the receive task, which opened a 50–500ms window where Dragon's
// reply arrived while nothing was draining the SDIO RX buffer.
// ---------------------------------------------------------------------------
esp_err_t voice_ws_send_register(void)
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
    /* #266: live JPEG video uplink.  Tab5 sends per-frame binary WS
     * frames prefixed with the "VID0" 4-byte magic + 4-byte length.
     * Dragon detects the magic to route video vs audio. */
    cJSON_AddBoolToObject(caps, "video_send", true);
    cJSON_AddBoolToObject(caps, "video_recv", true);   /* #268 Phase 3B */
    cJSON_AddNumberToObject(caps, "video_max_fps", 10);
    cJSON_AddNumberToObject(caps, "video_format", 0);  /* 0 = JPEG */
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

    ESP_LOGI(TAG, "Sending register: device=%s hw=%s session=%s",
             device_id, hardware_id, session_id[0] ? session_id : "(new)");

    esp_err_t err = voice_ws_send_text(json);
    cJSON_free(json);
    return err;
}

// ---------------------------------------------------------------------------
// Outbound widget_action (Tab5 → Dragon).  Pure WS-proto concern: builds a
// JSON frame, rate-limits, sends via voice_ws_send_text.  Pre-extract this
// lived inline in voice.c at the bottom of the public-API section.
// ---------------------------------------------------------------------------
esp_err_t voice_send_widget_action(const char *card_id, const char *event,
                                   const char *payload_json)
{
    if (!card_id || !event) return ESP_ERR_INVALID_ARG;
    if (!g_voice_ws || !esp_websocket_client_is_connected(g_voice_ws)) return ESP_ERR_INVALID_STATE;

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

// ---------------------------------------------------------------------------
// RX dispatchers — added in Tasks 1.6 / 1.7 / 1.8.
// voice_ws_proto_handle_text   — moved from voice.c handle_text_message
// voice_ws_proto_handle_binary — moved from voice.c handle_binary_message
// voice_ws_proto_event_handler — moved from voice.c voice_ws_event_handler
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// URI helper — added in Task 1.9.
// voice_ws_proto_build_local_uri — moved from voice.c voice_build_local_uri
// ---------------------------------------------------------------------------
