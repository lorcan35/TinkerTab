/**
 * TinkerTab — Touch driver (ST7123 integrated TDDI)
 *
 * Reads capacitive touch from the ST7123 at I2C 0x55.
 * Interrupt on GPIO23, reset via PI4IOE5V6408 pin P5.
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

#define TAB5_TOUCH_MAX_POINTS 5

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t strength;
} tab5_touch_point_t;

/**
 * Initialize ST7123 touch controller.
 * Must call after I2C bus and IO expanders are initialized.
 */
esp_err_t tab5_touch_init(i2c_master_bus_handle_t i2c_bus);

/**
 * Read current touch state.
 * Returns true if screen is being touched.
 */
bool tab5_touch_read(tab5_touch_point_t *points, uint8_t *count);

/**
 * Run touch diagnostics — reads raw registers and prints debug info.
 * Returns adv_info byte value, or negative on error.
 */
int tab5_touch_diag(void);
