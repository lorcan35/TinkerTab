/*
 * debug_server_voice.c — voice-pipeline debug HTTP endpoint family.
 *
 * Wave 23b follow-up (#332): fourth per-family extract.  Four
 * handlers moved verbatim from debug_server.c with `check_auth(req)`
 * → `tab5_debug_check_auth(req)` and `send_json_resp(req, root)` →
 * `tab5_debug_send_json_resp(req, root)`.  Identical behavior.
 */

#include "debug_server_voice.h"

#include <stddef.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"                /* TAB5_VOICE_PORT */
#include "debug_server_internal.h" /* tab5_debug_check_auth / send_json_resp */
#include "settings.h"              /* tab5_settings_get_dragon_host */
#include "voice.h"                 /* voice_* state + lifecycle */

/* chat_store_clear() is declared as extern at its single original
 * call site to avoid pulling all of chat_msg_store.h into this file
 * (matches the pattern from the pre-extract code). */
extern void chat_store_clear(void);

static const char *TAG = "debug_voice";

/* ── GET /voice — voice pipeline state snapshot ──────────────────── */
static esp_err_t voice_state_handler(httpd_req_t *req)
{
    if (!tab5_debug_check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", voice_is_connected());
    cJSON_AddNumberToObject(root, "state", (int)voice_get_state());

    const char *state_names[] = {
        "IDLE", "CONNECTING", "READY", "LISTENING",
        "PROCESSING", "SPEAKING", "RECONNECTING", "DICTATING"
    };
    int st = (int)voice_get_state();
    /* State enum has 8 values (0..7); the old bound stopped at 6 which
     * reported DICTATING as "UNKNOWN" — fix alongside the /dictation
     * endpoint so test harness sees the right label. */
    cJSON_AddStringToObject(root, "state_name",
                             (st >= 0 && st <= 7) ? state_names[st] : "UNKNOWN");

    /* Wave 14 W14-M01: the debug httpd task races with voice.c's
     * WS RX task.  Use the copy-under-mutex variants to avoid
     * observing a mid-strcat string. */
    char llm_buf[512];
    if (voice_get_llm_text_copy(llm_buf, sizeof(llm_buf)) && llm_buf[0]) {
        cJSON_AddStringToObject(root, "last_llm_text", llm_buf);
    }

    char stt_buf[512];
    if (voice_get_stt_text_copy(stt_buf, sizeof(stt_buf)) && stt_buf[0]) {
        cJSON_AddStringToObject(root, "last_stt_text", stt_buf);
    }

    return tab5_debug_send_json_resp(req, root);
}

/* ── POST /voice/reconnect — force voice WS reconnect ──────────── */
static esp_err_t voice_reconnect_handler(httpd_req_t *req)
{
    if (!tab5_debug_check_auth(req)) return ESP_OK;

    ESP_LOGI(TAG, "Debug: forcing voice reconnect");
    voice_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    /* Reconnect using current NVS settings + voice port (3502, not
     * dragon_port which is 3501 for CDP). */
    char host[64] = {0};
    tab5_settings_get_dragon_host(host, sizeof(host));
    voice_connect(host, TAB5_VOICE_PORT);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "reconnecting");
    return tab5_debug_send_json_resp(req, root);
}

/* ── POST /voice/cancel — cancel in-flight voice turn ──────────── */
static esp_err_t voice_cancel_handler(httpd_req_t *req)
{
    if (!tab5_debug_check_auth(req)) return ESP_OK;
    esp_err_t r = voice_cancel();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", r == ESP_OK);
    if (r != ESP_OK) cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
    return tab5_debug_send_json_resp(req, root);
}

/* ── POST /voice/clear — clear Dragon + Tab5 conversation history ── */
static esp_err_t voice_clear_handler(httpd_req_t *req)
{
    if (!tab5_debug_check_auth(req)) return ESP_OK;
    esp_err_t r = voice_clear_history();
    /* Also wipe Tab5 side so UI matches. */
    chat_store_clear();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", r == ESP_OK);
    cJSON_AddBoolToObject(root, "store_cleared", true);
    if (r != ESP_OK) cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
    return tab5_debug_send_json_resp(req, root);
}

void debug_server_voice_register(httpd_handle_t server)
{
    const httpd_uri_t uri_voice_state = {
        .uri = "/voice", .method = HTTP_GET, .handler = voice_state_handler
    };
    const httpd_uri_t uri_voice_reconnect = {
        .uri = "/voice/reconnect", .method = HTTP_POST, .handler = voice_reconnect_handler
    };
    const httpd_uri_t uri_voice_cancel = {
        .uri = "/voice/cancel", .method = HTTP_POST, .handler = voice_cancel_handler
    };
    const httpd_uri_t uri_voice_clear = {
        .uri = "/voice/clear", .method = HTTP_POST, .handler = voice_clear_handler
    };

    httpd_register_uri_handler(server, &uri_voice_state);
    httpd_register_uri_handler(server, &uri_voice_reconnect);
    httpd_register_uri_handler(server, &uri_voice_cancel);
    httpd_register_uri_handler(server, &uri_voice_clear);
    ESP_LOGI(TAG, "Voice endpoint family registered (4 URIs)");
}
