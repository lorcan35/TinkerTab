/**
 * TinkerTab Dragon Service — Dragon Q6A link management
 *
 * Depends on: NetworkService (WiFi must be connected)
 */

#include "service_registry.h"
#include "dragon_link.h"
#include "mode_manager.h"
#include "esp_log.h"

static const char *TAG = "svc_dragon";

esp_err_t dragon_service_init(void)
{
    /* Mode manager must be ready before dragon_link */
    tab5_mode_init();
    return ESP_OK;
}

esp_err_t dragon_service_start(void)
{
    ESP_LOGI(TAG, "Starting Dragon link...");
    esp_err_t ret = tab5_dragon_link_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Dragon link init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t dragon_service_stop(void)
{
    /* Switch to IDLE mode (stops any active streams) */
    tab5_mode_switch(MODE_IDLE);
    tab5_dragon_request_stop();
    return ESP_OK;
}
