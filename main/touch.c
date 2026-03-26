/**
 * TinkerTab — ST7123 Touch driver
 *
 * Uses esp_lcd_touch_st7123 component to read capacitive touch
 * from the integrated ST7123 TDDI at I2C 0x55.
 */

#include "touch.h"
#include "config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_lcd_panel_io.h"

static const char *TAG = "tab5_touch";

static esp_lcd_touch_handle_t s_touch = NULL;

esp_err_t tab5_touch_init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing ST7123 touch (I2C 0x55, INT=GPIO%d)", TAB5_TOUCH_INT_GPIO);

    // Create I2C panel IO for touch controller
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = 0x55,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = {
            .disable_control_phase = 1,
        },
        .scl_speed_hz = TAB5_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c_v2(i2c_bus, &tp_io_config, &tp_io),
        TAG, "Touch panel IO create failed");

    // Touch controller config
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = TAB5_DISPLAY_WIDTH,
        .y_max = TAB5_DISPLAY_HEIGHT,
        .rst_gpio_num = -1,  // Reset via IO expander (already done)
        .int_gpio_num = TAB5_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_st7123(tp_io, &tp_cfg, &s_touch),
        TAG, "ST7123 touch create failed");

    ESP_LOGI(TAG, "ST7123 touch initialized (0--%d x 0--%d)", TAB5_DISPLAY_WIDTH, TAB5_DISPLAY_HEIGHT);
    return ESP_OK;
}

bool tab5_touch_read(tab5_touch_point_t *points, uint8_t *count)
{
    if (!s_touch || !points || !count) return false;

    *count = 0;

    esp_err_t ret = esp_lcd_touch_read_data(s_touch);
    if (ret != ESP_OK) return false;

    uint16_t x[TAB5_TOUCH_MAX_POINTS];
    uint16_t y[TAB5_TOUCH_MAX_POINTS];
    uint16_t strength[TAB5_TOUCH_MAX_POINTS];
    uint8_t cnt = 0;

    bool pressed = esp_lcd_touch_get_coordinates(
        s_touch, x, y, strength, &cnt, TAB5_TOUCH_MAX_POINTS);

    if (pressed && cnt > 0) {
        *count = cnt;
        for (int i = 0; i < cnt; i++) {
            points[i].x = x[i];
            points[i].y = y[i];
            points[i].strength = strength[i];
        }
        return true;
    }

    return false;
}

int tab5_touch_diag(void)
{
    if (!s_touch) {
        ESP_LOGE(TAG, "Touch not initialized");
        return -1;
    }

    // Read adv_info register
    uint8_t adv_info = 0;
    esp_err_t ret = esp_lcd_panel_io_rx_param(s_touch->io, 0x0010, &adv_info, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DIAG: Failed to read reg 0x0010: %s", esp_err_to_name(ret));
        return -2;
    }
    ESP_LOGI(TAG, "DIAG: adv_info=0x%02X with_coord=%d", adv_info, (adv_info >> 3) & 1);

    // Read max_touches from reg 0x0009
    uint8_t max_touches = 0;
    ret = esp_lcd_panel_io_rx_param(s_touch->io, 0x0009, &max_touches, 1);
    ESP_LOGI(TAG, "DIAG: max_touches=%d (reg 0x0009)", max_touches);

    // Read raw touch report bytes from 0x0014 (7 bytes per report, up to max_touches)
    if (max_touches > 0 && max_touches <= 10) {
        uint8_t raw[70];  // 10 * 7 bytes max
        uint8_t read_len = max_touches * 7;
        ret = esp_lcd_panel_io_rx_param(s_touch->io, 0x0014, raw, read_len);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "DIAG: Raw report (%d bytes):", read_len);
            for (int i = 0; i < max_touches && i < 3; i++) {
                uint8_t *r = &raw[i * 7];
                ESP_LOGI(TAG, "  [%d] %02X %02X %02X %02X %02X %02X %02X (valid=%d x=%d y=%d)",
                         i, r[0], r[1], r[2], r[3], r[4], r[5], r[6],
                         (r[0] >> 7) & 1,
                         ((r[0] & 0x3F) << 8) | r[1],
                         (r[2] << 8) | r[3]);
            }
        } else {
            ESP_LOGE(TAG, "DIAG: Failed to read report: %s", esp_err_to_name(ret));
        }
    }

    // Try a full driver read cycle
    ret = esp_lcd_touch_read_data(s_touch);
    ESP_LOGI(TAG, "DIAG: read_data ret=%s, cached_points=%d",
             esp_err_to_name(ret), s_touch->data.points);

    return (int)adv_info;
}
