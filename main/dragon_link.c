/**
 * TinkerTab — Dragon Link
 *
 * Connection state machine for Tab5 ↔ Dragon Q6A.
 * Discovers Dragon via HTTP health check, performs handshake,
 * then starts MJPEG streaming + touch WebSocket forwarding.
 * Auto-reconnects with exponential backoff on disconnect.
 */

#include "dragon_link.h"
#include "config.h"
#include "wifi.h"
#include "mjpeg_stream.h"
#include "udp_stream.h"
#include "touch_ws.h"
#include "mdns_discovery.h"
#include "mode_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "dragon_link";

// State
static dragon_state_t s_state = DRAGON_STATE_IDLE;
static volatile bool s_stop_requested = false;
static volatile bool s_start_requested = false;
static uint32_t s_backoff_ms = TAB5_DRAGON_RECONNECT_BASE_MS;

// Active Dragon endpoint (populated by mDNS discovery or fallback)
#define MDNS_DISCOVERY_TIMEOUT_MS 5000
static char s_dragon_host[64] = {0};
static uint16_t s_dragon_port = 0;

// Negotiated params from handshake
static int s_stream_quality = 60;
static int s_stream_fps = 15;
static bool s_udp_supported = false;  // Dragon reports UDP streaming capability

// Disconnect tracking
static volatile bool s_mjpeg_disconnected = false;
static volatile bool s_touch_disconnected = false;

// -------------------------------------------------------------------------
// Callbacks from MJPEG and touch WS tasks — passed to stream start APIs
// -------------------------------------------------------------------------

static void on_touch_disconnect(void)
{
    ESP_LOGW(TAG, "Touch WS disconnected");
    s_touch_disconnected = true;
}

// -------------------------------------------------------------------------
// HTTP helpers
// -------------------------------------------------------------------------

/** HTTP response buffer for health/handshake. */
typedef struct {
    char *buf;
    int len;
    int cap;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    if (!resp) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp->len + evt->data_len < resp->cap - 1) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * Perform HTTP GET and return response body.
 * Returns ESP_OK on 200 status, body in resp->buf.
 */
static esp_err_t http_get(const char *url, http_response_t *resp)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = TAB5_DRAGON_HEALTH_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = resp,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) return ESP_FAIL;
    return ESP_OK;
}

// -------------------------------------------------------------------------
// State machine actions
// -------------------------------------------------------------------------

static bool do_health_check(void)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             s_dragon_host, s_dragon_port, TAB5_DRAGON_HEALTH_PATH);

    char body[256] = {0};
    http_response_t resp = { .buf = body, .len = 0, .cap = sizeof(body) };

    esp_err_t err = http_get(url, &resp);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Health check failed: %s", esp_err_to_name(err));
        return false;
    }

    // Parse JSON response
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "Health check: invalid JSON");
        return false;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    bool ok = status && cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
    cJSON_Delete(root);

    if (ok) {
        ESP_LOGI(TAG, "Dragon health check OK");
    }
    return ok;
}

static bool do_handshake(void)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://%s:%d%s?device=tab5&fw=1.0.0&w=%d&h=%d",
             s_dragon_host, s_dragon_port, TAB5_DRAGON_HANDSHAKE_PATH,
             TAB5_DISPLAY_WIDTH, TAB5_DISPLAY_HEIGHT);

    char body[512] = {0};
    http_response_t resp = { .buf = body, .len = 0, .cap = sizeof(body) };

    esp_err_t err = http_get(url, &resp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Handshake failed: %s", esp_err_to_name(err));
        return false;
    }

    // Parse negotiated params
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "Handshake: invalid JSON");
        return false;
    }

    cJSON *q = cJSON_GetObjectItem(root, "quality");
    cJSON *f = cJSON_GetObjectItem(root, "fps");
    cJSON *udp = cJSON_GetObjectItem(root, "udp_stream");
    if (q && cJSON_IsNumber(q)) s_stream_quality = q->valueint;
    if (f && cJSON_IsNumber(f)) s_stream_fps = f->valueint;
    s_udp_supported = (udp && cJSON_IsBool(udp) && cJSON_IsTrue(udp));

    ESP_LOGI(TAG, "Handshake OK — quality=%d, fps=%d, udp=%s",
             s_stream_quality, s_stream_fps, s_udp_supported ? "yes" : "no");
    cJSON_Delete(root);
    return true;
}

// -------------------------------------------------------------------------
// Main task
// -------------------------------------------------------------------------

static void do_mdns_discovery(void)
{
    mdns_discovery_result_t disc = {0};
    tab5_mdns_discover_dragon(&disc, MDNS_DISCOVERY_TIMEOUT_MS);

    strncpy(s_dragon_host, disc.host, sizeof(s_dragon_host) - 1);
    s_dragon_host[sizeof(s_dragon_host) - 1] = '\0';
    s_dragon_port = disc.port;

    if (disc.from_mdns) {
        ESP_LOGI(TAG, "Using mDNS-discovered Dragon: %s:%d", s_dragon_host, s_dragon_port);
    } else {
        ESP_LOGI(TAG, "Using hardcoded fallback Dragon: %s:%d", s_dragon_host, s_dragon_port);
    }
}

static void dragon_link_task(void *arg)
{
    // Initialize mDNS and register Tab5 service
    tab5_mdns_init();

    // Discover Dragon via mDNS (falls back to config if not found)
    do_mdns_discovery();

    ESP_LOGI(TAG, "Dragon link task started (target: %s:%d)",
             s_dragon_host, s_dragon_port);

    s_state = DRAGON_STATE_DISCOVERING;

    while (1) {
        // Check for manual stop
        if (s_stop_requested) {
            s_stop_requested = false;
            tab5_mode_switch(MODE_IDLE);
            s_state = DRAGON_STATE_IDLE;
            ESP_LOGI(TAG, "Stopped by request");

            // Wait for start request
            while (!s_start_requested) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            s_start_requested = false;
            s_state = DRAGON_STATE_DISCOVERING;
            s_backoff_ms = TAB5_DRAGON_RECONNECT_BASE_MS;
        }

        // Check WiFi
        if (!tab5_wifi_connected()) {
            tab5_mode_switch(MODE_IDLE);
            s_state = DRAGON_STATE_IDLE;
            ESP_LOGW(TAG, "WiFi lost, waiting...");
            while (!tab5_wifi_connected()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            // Re-discover Dragon — IP may have changed
            do_mdns_discovery();
            s_state = DRAGON_STATE_DISCOVERING;
            s_backoff_ms = TAB5_DRAGON_RECONNECT_BASE_MS;
        }

        switch (s_state) {
        case DRAGON_STATE_DISCOVERING:
            if (do_health_check()) {
                s_state = DRAGON_STATE_HANDSHAKE;
                s_backoff_ms = TAB5_DRAGON_RECONNECT_BASE_MS;
            } else {
                ESP_LOGI(TAG, "Dragon not found, retry in %lums", (unsigned long)s_backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(s_backoff_ms));
                // Exponential backoff
                if (s_backoff_ms < TAB5_DRAGON_RECONNECT_MAX_MS) {
                    s_backoff_ms = s_backoff_ms * 2;
                    if (s_backoff_ms > TAB5_DRAGON_RECONNECT_MAX_MS) {
                        s_backoff_ms = TAB5_DRAGON_RECONNECT_MAX_MS;
                    }
                }
            }
            break;

        case DRAGON_STATE_HANDSHAKE:
            if (do_handshake()) {
                s_state = DRAGON_STATE_CONNECTED;
            } else {
                ESP_LOGW(TAG, "Handshake failed, back to discovery");
                s_state = DRAGON_STATE_DISCOVERING;
                vTaskDelay(pdMS_TO_TICKS(TAB5_DRAGON_RECONNECT_BASE_MS));
            }
            break;

        case DRAGON_STATE_CONNECTED:
            // Dragon is reachable — don't auto-start MJPEG streaming.
            // User explicitly starts streaming from the UI when they want it.
            ESP_LOGI(TAG, "Dragon connected (idle — no auto-stream)");
            s_state = DRAGON_STATE_STREAMING;  // reuse state for "connected" monitoring
            break;

        case DRAGON_STATE_STREAMING:
            // Monitor Dragon connectivity. MJPEG only runs if user started it.
            {
                tab5_mode_t cur = tab5_mode_get();
                if (cur == MODE_STREAMING && (s_mjpeg_disconnected || s_touch_disconnected)) {
                    // Only reconnect streams if user explicitly started streaming
                    ESP_LOGW(TAG, "Stream lost (mjpeg=%d, touch=%d), reconnecting...",
                             s_mjpeg_disconnected, s_touch_disconnected);
                    tab5_mode_switch(MODE_IDLE);
                    s_state = DRAGON_STATE_RECONNECTING;
                    s_backoff_ms = TAB5_DRAGON_RECONNECT_BASE_MS;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            break;

        case DRAGON_STATE_RECONNECTING:
            ESP_LOGI(TAG, "Reconnecting in %lums...", (unsigned long)s_backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(s_backoff_ms));
            if (s_backoff_ms < TAB5_DRAGON_RECONNECT_MAX_MS) {
                s_backoff_ms *= 2;
                if (s_backoff_ms > TAB5_DRAGON_RECONNECT_MAX_MS) {
                    s_backoff_ms = TAB5_DRAGON_RECONNECT_MAX_MS;
                }
            }
            // Re-discover in case Dragon moved IPs
            do_mdns_discovery();
            s_state = DRAGON_STATE_DISCOVERING;
            break;

        case DRAGON_STATE_IDLE:
        case DRAGON_STATE_OFFLINE:
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }
    }
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

esp_err_t tab5_dragon_link_init(void)
{
    xTaskCreatePinnedToCore(
        dragon_link_task,
        "dragon_link",
        8192,
        NULL,
        3,      // Below WiFi (5), below MJPEG (5), below touch WS (4)
        NULL,
        0       // Core 0
    );
    return ESP_OK;
}

dragon_state_t tab5_dragon_get_state(void)
{
    return s_state;
}

const char *tab5_dragon_state_str(void)
{
    switch (s_state) {
    case DRAGON_STATE_IDLE:          return "idle";
    case DRAGON_STATE_DISCOVERING:   return "discovering";
    case DRAGON_STATE_HANDSHAKE:     return "handshake";
    case DRAGON_STATE_CONNECTED:     return "connected";
    case DRAGON_STATE_STREAMING:     return "streaming";
    case DRAGON_STATE_RECONNECTING:  return "reconnecting";
    case DRAGON_STATE_OFFLINE:       return "offline";
    default:                         return "unknown";
    }
}

float tab5_dragon_get_fps(void)
{
    if (s_state == DRAGON_STATE_STREAMING) {
        if (udp_stream_is_active()) {
            return udp_stream_get_fps();
        }
        return tab5_mjpeg_get_fps();
    }
    return 0.0f;
}

void tab5_dragon_request_stop(void)
{
    s_stop_requested = true;
}

void tab5_dragon_request_start(void)
{
    s_start_requested = true;
}

bool tab5_dragon_is_streaming(void)
{
    return s_state == DRAGON_STATE_STREAMING;
}
