/**
 * TinkerTab Mode FSM — voice-pipeline coordinator
 *
 * Post-#154 the only real responsibility is serializing voice_connect
 * and voice_disconnect across tasks (orb-tap, long-press, settings,
 * debug server).  The streaming stop/start helpers are gone.
 */

#include "mode_manager.h"
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

static const char *mode_names[] = {
    [MODE_IDLE]  = "IDLE",
    [MODE_VOICE] = "VOICE",
};

static void stop_voice(void)
{
    voice_state_t vs = voice_get_state();
    if (vs != VOICE_STATE_IDLE) {
        voice_disconnect();
    }
}

static void start_voice(void)
{
    char dhost[64];
    tab5_settings_get_dragon_host(dhost, sizeof(dhost));
    // Connect only — don't auto-listen. User chooses mode:
    // Short tap from READY → Ask (30s), Long-press from READY → Dictate (unlimited)
    voice_connect_async(dhost, TAB5_VOICE_PORT, false);
}

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
    if (m <= MODE_VOICE) return mode_names[m];
    return "UNKNOWN";
}

esp_err_t tab5_mode_switch(tab5_mode_t new_mode)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

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

    switch (new_mode) {
    case MODE_VOICE:
        start_voice();
        break;
    case MODE_IDLE:
        /* Keep voice WS connected across screen transitions.
         * Session survives going to Notes/Settings and back.
         * Only disconnect on explicit voice_disconnect() call. */
        (void)stop_voice;  /* kept for future use; no-op today */
        break;
    }

    s_mode = new_mode;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
