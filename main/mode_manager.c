/**
 * TinkerTab Mode FSM — Resource Manager (closes #20)
 *
 * Coordinates exclusive access to SDIO WiFi bandwidth.
 * Only one heavy consumer (MJPEG streaming OR voice WebSocket) runs at a time.
 */

#include "mode_manager.h"
#include "mjpeg_stream.h"
#include "touch_ws.h"
#include "voice.h"
#include "config.h"
#include "settings.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "mode";

static tab5_mode_t s_mode = MODE_IDLE;
static SemaphoreHandle_t s_mutex = NULL;

// Settle time between stopping one service and starting another (ms).
// Lets SDIO DMA buffers drain and be freed.
#define MODE_SETTLE_MS 200

static const char *mode_names[] = {
    [MODE_IDLE]      = "IDLE",
    [MODE_STREAMING] = "STREAMING",
    [MODE_VOICE]     = "VOICE",
    [MODE_BROWSING]  = "BROWSING",
};

// -------------------------------------------------------------------------
// Stop helpers — each is idempotent
// -------------------------------------------------------------------------

static void stop_streaming(void)
{
    if (tab5_mjpeg_is_running()) {
        tab5_mjpeg_stop();
    }
    tab5_touch_ws_stop();
}

static void stop_voice(void)
{
    voice_state_t vs = voice_get_state();
    if (vs != VOICE_STATE_IDLE) {
        voice_disconnect();
    }
}

// Wait for MJPEG task to actually exit (it sets s_running = false).
// HTTP read can block up to 5s + retry delay, so wait up to 8s.
static void wait_streams_stopped(void)
{
    for (int i = 0; i < 80 && tab5_mjpeg_is_running(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (tab5_mjpeg_is_running()) {
        ESP_LOGW(TAG, "MJPEG task did not exit in 8s");
    }
}

// -------------------------------------------------------------------------
// Start helpers
// -------------------------------------------------------------------------

static void start_streaming(void)
{
    tab5_mjpeg_start();
    tab5_touch_ws_start();
}

static void start_voice(void)
{
    char dhost[64];
    tab5_settings_get_dragon_host(dhost, sizeof(dhost));
    // Connect only — don't auto-listen. User chooses mode:
    // Short tap from READY → Ask (30s), Long-press from READY → Dictate (unlimited)
    voice_connect_async(dhost, TAB5_VOICE_PORT, false);
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

void tab5_mode_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_mode = MODE_IDLE;
    ESP_LOGI(TAG, "Mode manager initialized (IDLE)");
}

tab5_mode_t tab5_mode_get(void)
{
    return s_mode;
}

const char *tab5_mode_str(void)
{
    tab5_mode_t m = s_mode;
    if (m <= MODE_BROWSING) return mode_names[m];
    return "UNKNOWN";
}

esp_err_t tab5_mode_switch(tab5_mode_t new_mode)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    // Try to acquire with timeout — prevents deadlock if called from competing tasks
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Mode switch mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    tab5_mode_t old_mode = s_mode;

    if (old_mode == new_mode) {
        /* Special case: if we're in VOICE mode but voice is disconnected,
         * force a reconnect instead of returning early. */
        if (new_mode == MODE_VOICE && voice_get_state() == VOICE_STATE_IDLE) {
            ESP_LOGI(TAG, "VOICE mode but disconnected — reconnecting");
            start_voice();
        } else {
            ESP_LOGD(TAG, "Already in %s", mode_names[new_mode]);
        }
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "%s -> %s", mode_names[old_mode], mode_names[new_mode]);

    // --- Phase 1: Stop services from old mode ---
    switch (old_mode) {
    case MODE_STREAMING:
    case MODE_BROWSING:
        stop_streaming();
        wait_streams_stopped();
        break;
    case MODE_VOICE:
        stop_voice();
        break;
    case MODE_IDLE:
        break;
    }

    // --- Phase 2: Settle — let DMA buffers drain ---
    vTaskDelay(pdMS_TO_TICKS(MODE_SETTLE_MS));

    // --- Phase 3: Start services for new mode ---
    switch (new_mode) {
    case MODE_STREAMING:
        start_streaming();
        break;
    case MODE_BROWSING:
        // Connected to Dragon but no active streaming — don't start MJPEG
        break;
    case MODE_VOICE:
        start_voice();
        break;
    case MODE_IDLE:
        break;
    }

    s_mode = new_mode;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
