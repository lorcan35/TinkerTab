/**
 * TinkerTab Network Service — ESP-Hosted SDIO WiFi
 */

#include "service_registry.h"
#include "wifi.h"
#include "settings.h"
#include "esp_log.h"

static const char *TAG = "svc_network";

esp_err_t network_service_init(void)
{
    esp_err_t ret = tab5_wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t network_service_start(void)
{
    char ssid[33];
    tab5_settings_get_wifi_ssid(ssid, sizeof(ssid));
    ESP_LOGI(TAG, "Connecting to %s...", ssid);

    esp_err_t ret = tab5_wifi_wait_connected(60000);  /* ESP-Hosted C6 needs up to 45s */
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect timeout: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi connected");
    return ESP_OK;
}

esp_err_t network_service_stop(void)
{
    /* WiFi disconnect not implemented yet — just log */
    ESP_LOGI(TAG, "Network stop (WiFi stays connected)");
    return ESP_OK;
}
