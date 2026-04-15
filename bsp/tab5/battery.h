/**
 * TinkerClaw Tab5 — Battery monitor (INA226)
 *
 * Reads voltage, current, and power from the INA226 high-side current/power
 * monitor on the system I2C bus.  The Tab5 uses an NP-F550 (7.4 V, 2 S LiPo).
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float    voltage;   // bus voltage in volts
    float    current;   // current in amps (positive = discharging)
    float    power;     // power in watts
    uint8_t  percent;   // estimated SoC 0–100 %
    bool     charging;  // true when current flows into the battery
} tab5_battery_info_t;

/**
 * Initialise the INA226 battery monitor.
 * Must be called before any other tab5_battery_* function.
 */
esp_err_t tab5_battery_init(i2c_master_bus_handle_t i2c_bus);

/** Read all battery parameters at once. */
esp_err_t tab5_battery_read(tab5_battery_info_t *info);

/** Quick-read: return estimated percentage (0–100). Returns 0 on error. */
uint8_t tab5_battery_percent(void);

/** Quick-read: return true if the battery is charging. */
bool tab5_battery_charging(void);
