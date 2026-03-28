/**
 * TinkerTab Display Service — MIPI DSI + JPEG decoder + touch
 */

#include "service_registry.h"
#include "display.h"
#include "touch.h"
#include "settings.h"
#include "esp_log.h"

static const char *TAG = "svc_display";
static bool s_touch_ok = false;

/* I2C bus handle — set by main before services init */
extern i2c_master_bus_handle_t tab5_get_i2c_bus(void);

esp_err_t display_service_init(void)
{
    esp_err_t ret = tab5_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Hardware JPEG decoder */
    ret = tab5_display_jpeg_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "JPEG decoder init failed: %s", esp_err_to_name(ret));
    }

    /* Touch controller */
    i2c_master_bus_handle_t bus = tab5_get_i2c_bus();
    if (bus) {
        ret = tab5_touch_init(bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Touch init failed: %s", esp_err_to_name(ret));
        } else {
            s_touch_ok = true;
        }
    }

    return ESP_OK;
}

esp_err_t display_service_start(void)
{
    /* Apply stored brightness — effectively "turns on" the screen */
    tab5_display_set_brightness(tab5_settings_get_brightness());
    return ESP_OK;
}

esp_err_t display_service_stop(void)
{
    /* Turn off backlight */
    tab5_display_set_brightness(0);
    return ESP_OK;
}

bool display_service_touch_ok(void)
{
    return s_touch_ok;
}
