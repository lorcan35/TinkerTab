/**
 * TinkerClaw Tab5 — Bluetooth Low Energy (BLE) via ESP32-C6
 *
 * IMPORTANT: The ESP32-P4 has NO native Bluetooth radio.  All BLE traffic on
 * the Tab5 must be forwarded through the ESP32-C6 co-processor, which is
 * connected via SDIO and managed by ESP-Hosted / WiFi Remote.
 *
 * As of ESP-Hosted v1.4.0 (and esp_wifi_remote), only WiFi is forwarded from
 * the C6 to the P4.  BLE forwarding is NOT yet implemented in the public
 * ESP-Hosted SDK.  Espressif has indicated BLE support is planned, but there
 * is no public timeline.
 *
 * Until BLE forwarding lands, every function in this driver returns
 * ESP_ERR_NOT_SUPPORTED with a descriptive log message.  Once the SDK adds
 * support, replace the stubs with real NimBLE / Bluedroid calls.
 *
 * Tracking: https://github.com/espressif/esp-hosted/issues (search "BLE")
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Initialise the BLE subsystem via ESP-Hosted.
 * Currently returns ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t tab5_ble_init(void);

/**
 * Start a BLE scan for the given duration.
 * Currently returns ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t tab5_ble_start_scan(uint32_t duration_sec);

/**
 * Stop an in-progress BLE scan.
 * Currently returns ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t tab5_ble_stop_scan(void);

/**
 * Return the number of BLE devices found during the last scan.
 * Currently always returns 0.
 */
uint8_t tab5_ble_get_scan_count(void);
