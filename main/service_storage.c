/**
 * TinkerTab Storage Service — NVS + SD card lifecycle
 */

#include "service_registry.h"
#include "settings.h"
#include "sdcard.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "svc_storage";
static bool s_nvs_ok = false;
static bool s_sd_ok = false;

esp_err_t storage_service_init(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_nvs_ok = true;

    /* Settings (NVS-backed) */
    ret = tab5_settings_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Settings init failed: %s (using defaults)", esp_err_to_name(ret));
    }

    /* SD card */
    ret = tab5_sdcard_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card: %s (continuing without)", esp_err_to_name(ret));
    } else {
        s_sd_ok = true;
        ESP_LOGI(TAG, "SD: %.1f GB free",
                 tab5_sdcard_free_bytes() / 1073741824.0);
    }

    return ESP_OK; /* NVS success is enough */
}

esp_err_t storage_service_start(void)
{
    /* Storage is always available after init — no runtime start needed */
    return ESP_OK;
}

esp_err_t storage_service_stop(void)
{
    /* NVS stays open, SD stays mounted — nothing to stop */
    return ESP_OK;
}

bool storage_service_sd_ok(void)
{
    return s_sd_ok;
}

bool storage_service_nvs_ok(void)
{
    return s_nvs_ok;
}
