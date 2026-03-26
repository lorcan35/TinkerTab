/**
 * TinkerClaw Tab5 — Bluetooth Low Energy stub driver
 *
 * BLE on the Tab5 requires the ESP32-C6 co-processor to forward BLE traffic
 * to the ESP32-P4 host via ESP-Hosted.  As of ESP-Hosted v1.4.0, only WiFi
 * forwarding is supported — BLE is NOT yet available.
 *
 * What would need to happen for real BLE support:
 *   1. Espressif adds a BLE HCI transport layer to esp_hosted / esp_wifi_remote
 *      so that NimBLE (or Bluedroid) on the P4 can send/receive HCI frames
 *      through the SDIO link to the C6's Bluetooth controller.
 *   2. The C6 firmware (network_adapter) must expose the BLE controller.
 *   3. On the P4 side, configure NimBLE with an "external controller" transport
 *      backed by the hosted HCI channel.
 *
 * Until then, every function logs a warning and returns ESP_ERR_NOT_SUPPORTED.
 *
 * TODO: When ESP-Hosted gains BLE support:
 *   - #include "esp_bt.h" / "host/ble_hs.h" (NimBLE)
 *   - Initialise the hosted BLE transport in tab5_ble_init()
 *   - Implement GAP scanning in tab5_ble_start_scan / stop_scan
 *   - Store discovered devices in a ring buffer, expose via get_scan_count()
 */

#include "bluetooth.h"
#include "esp_log.h"

static const char *TAG = "tab5_ble";

esp_err_t tab5_ble_init(void)
{
    ESP_LOGW(TAG, "BLE is NOT available on ESP32-P4 natively.");
    ESP_LOGW(TAG, "BLE requires ESP-Hosted BLE forwarding from the ESP32-C6.");
    ESP_LOGW(TAG, "As of ESP-Hosted v1.4.0, BLE forwarding is not implemented.");
    ESP_LOGW(TAG, "This driver is a stub — all calls return ESP_ERR_NOT_SUPPORTED.");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tab5_ble_start_scan(uint32_t duration_sec)
{
    ESP_LOGW(TAG, "tab5_ble_start_scan(%lu s): BLE not supported yet",
             (unsigned long)duration_sec);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tab5_ble_stop_scan(void)
{
    ESP_LOGW(TAG, "tab5_ble_stop_scan: BLE not supported yet");
    return ESP_ERR_NOT_SUPPORTED;
}

uint8_t tab5_ble_get_scan_count(void)
{
    ESP_LOGW(TAG, "tab5_ble_get_scan_count: BLE not supported yet");
    return 0;
}
