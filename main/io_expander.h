/**
 * TinkerClaw Tab5 — IO Expander (PI4IOE5V6416)
 *
 * Two I2C-controlled GPIO expanders manage:
 *   PI4IOE1 (0x43): LCD reset, touch reset, speaker, ext 5V, camera
 *   PI4IOE2 (0x44): WiFi power, USB 5V, charging, poweroff
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

esp_err_t tab5_io_expander_init(i2c_master_bus_handle_t bus);
void tab5_set_wifi_power(bool en);
void tab5_set_lcd_reset(bool active);
void tab5_set_touch_reset(bool active);
void tab5_reset_display_and_touch(void);
