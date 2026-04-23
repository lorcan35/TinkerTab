/**
 * TinkerTab Dragon Service — voice-pipeline coordination shim
 *
 * Post-#154 this service is nearly vestigial: the CDP browser-streaming
 * link is gone, and voice WS connects directly from main.c. All that
 * remains is initializing the mode-manager mutex so ui_voice / ui_notes
 * can call tab5_mode_switch(MODE_VOICE).
 *
 * Kept in the service_registry lifecycle so ordering vs Network/Display
 * doesn't change; if we ever shed mode_manager entirely, delete this file
 * and its registry entry together.
 */

#include "service_registry.h"
#include "mode_manager.h"
#include "esp_log.h"

static const char *TAG = "svc_dragon";

esp_err_t dragon_service_init(void)
{
    tab5_mode_init();
    return ESP_OK;
}

esp_err_t dragon_service_start(void)
{
    ESP_LOGI(TAG, "Dragon service start — voice WS is driven directly from main.c");
    return ESP_OK;
}

esp_err_t dragon_service_stop(void)
{
    tab5_mode_switch(MODE_IDLE);
    return ESP_OK;
}
