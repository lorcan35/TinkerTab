/**
 * TinkerClaw Tab5 — RTC driver (RX8130CE)
 *
 * Real-time clock on the system I2C bus (0x32).
 * Supports get/set time and NTP synchronisation over WiFi.
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>

typedef struct {
    uint8_t year;      // 0–99 (offset from 2000)
    uint8_t month;     // 1–12
    uint8_t day;       // 1–31
    uint8_t hour;      // 0–23
    uint8_t minute;    // 0–59
    uint8_t second;    // 0–59
    uint8_t weekday;   // 0=Sun … 6=Sat
} tab5_rtc_time_t;

/**
 * Initialise the RX8130CE RTC on the given I2C bus.
 * Must be called before any other tab5_rtc_* function.
 */
esp_err_t tab5_rtc_init(i2c_master_bus_handle_t i2c_bus);

/** Read current date/time from the RTC. */
esp_err_t tab5_rtc_get_time(tab5_rtc_time_t *time);

/** Write date/time to the RTC. */
esp_err_t tab5_rtc_set_time(const tab5_rtc_time_t *time);

/**
 * Synchronise the RTC from an NTP server (pool.ntp.org).
 * Requires WiFi to be connected. Blocks up to ~10 s waiting for sync.
 * On success the RTC is updated with the UTC time.
 */
esp_err_t tab5_rtc_sync_from_ntp(void);
