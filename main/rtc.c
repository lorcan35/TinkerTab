/**
 * TinkerClaw Tab5 — RTC driver (RX8130CE)
 *
 * I2C address 0x32.  Registers 0x10‥0x16 hold BCD‑encoded time.
 * NTP sync uses the ESP-IDF SNTP helper and writes the result back to the chip.
 */

#include "rtc.h"
#include "config.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "tab5_rtc";

// ---------------------------------------------------------------------------
// RX8130CE constants
// ---------------------------------------------------------------------------
#define RX8130_ADDR        0x32
#define RX8130_REG_SEC     0x10
#define RX8130_REG_MIN     0x11
#define RX8130_REG_HOUR    0x12
#define RX8130_REG_WDAY    0x13
#define RX8130_REG_DAY     0x14
#define RX8130_REG_MONTH   0x15
#define RX8130_REG_YEAR    0x16

#define I2C_TIMEOUT_MS     50
#define NTP_SYNC_TIMEOUT_S 10

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static i2c_master_dev_handle_t s_rtc_dev = NULL;

// ---------------------------------------------------------------------------
// BCD helpers
// ---------------------------------------------------------------------------
static inline uint8_t bcd_to_bin(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static inline uint8_t bin_to_bcd(uint8_t bin)
{
    return ((bin / 10) << 4) | (bin % 10);
}

// ---------------------------------------------------------------------------
// Low-level I2C helpers
// ---------------------------------------------------------------------------
static esp_err_t rtc_read_regs(uint8_t start_reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_rtc_dev, &start_reg, 1, buf, len,
                                       I2C_TIMEOUT_MS);
}

static esp_err_t rtc_write_regs(uint8_t start_reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];  // reg + up to 7 data bytes
    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = start_reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_rtc_dev, buf, 1 + len, I2C_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t tab5_rtc_init(i2c_master_bus_handle_t i2c_bus)
{
    if (s_rtc_dev) {
        ESP_LOGW(TAG, "already initialised");
        return ESP_OK;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RX8130_ADDR,
        .scl_speed_hz    = TAB5_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &cfg, &s_rtc_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to add RX8130CE device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Quick probe: try reading the seconds register
    uint8_t probe = 0;
    ret = rtc_read_regs(RX8130_REG_SEC, &probe, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RX8130CE not responding at 0x%02X", RX8130_ADDR);
        return ret;
    }

    ESP_LOGI(TAG, "RX8130CE RTC initialised (probe sec=0x%02X)", probe);
    return ESP_OK;
}

esp_err_t tab5_rtc_get_time(tab5_rtc_time_t *time)
{
    if (!s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[7];  // sec, min, hour, wday, day, month, year
    esp_err_t ret = rtc_read_regs(RX8130_REG_SEC, raw, 7);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    time->second  = bcd_to_bin(raw[0] & 0x7F);  // bit 7 may be VLF flag
    time->minute  = bcd_to_bin(raw[1] & 0x7F);
    time->hour    = bcd_to_bin(raw[2] & 0x3F);
    time->weekday = raw[3] & 0x07;               // bitmask, lowest set bit
    time->day     = bcd_to_bin(raw[4] & 0x3F);
    time->month   = bcd_to_bin(raw[5] & 0x1F);
    time->year    = bcd_to_bin(raw[6]);

    ESP_LOGD(TAG, "read: 20%02u-%02u-%02u %02u:%02u:%02u wday=%u",
             time->year, time->month, time->day,
             time->hour, time->minute, time->second, time->weekday);
    return ESP_OK;
}

esp_err_t tab5_rtc_set_time(const tab5_rtc_time_t *time)
{
    if (!s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[7] = {
        bin_to_bcd(time->second),
        bin_to_bcd(time->minute),
        bin_to_bcd(time->hour),
        time->weekday & 0x07,
        bin_to_bcd(time->day),
        bin_to_bcd(time->month),
        bin_to_bcd(time->year),
    };

    esp_err_t ret = rtc_write_regs(RX8130_REG_SEC, raw, 7);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "set: 20%02u-%02u-%02u %02u:%02u:%02u",
             time->year, time->month, time->day,
             time->hour, time->minute, time->second);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// NTP synchronisation
// ---------------------------------------------------------------------------

esp_err_t tab5_rtc_sync_from_ntp(void)
{
    if (!s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "starting NTP sync …");

    // Initialise SNTP if not already running
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
    }

    // Wait for time to be set (SNTP callback sets the system clock)
    int attempts = 0;
    time_t now = 0;
    struct tm tm_info;
    while (attempts < NTP_SYNC_TIMEOUT_S * 2) {  // poll every 500 ms
        time(&now);
        localtime_r(&now, &tm_info);
        if (tm_info.tm_year >= (2024 - 1900)) {
            break;  // got a plausible year
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
    }

    if (tm_info.tm_year < (2024 - 1900)) {
        ESP_LOGE(TAG, "NTP sync timed out after %d s", NTP_SYNC_TIMEOUT_S);
        return ESP_ERR_TIMEOUT;
    }

    // Convert system time → RTC struct (UTC)
    gmtime_r(&now, &tm_info);
    tab5_rtc_time_t rtc_time = {
        .year    = (uint8_t)(tm_info.tm_year - 100),  // tm_year is years since 1900
        .month   = (uint8_t)(tm_info.tm_mon + 1),
        .day     = (uint8_t)tm_info.tm_mday,
        .hour    = (uint8_t)tm_info.tm_hour,
        .minute  = (uint8_t)tm_info.tm_min,
        .second  = (uint8_t)tm_info.tm_sec,
        .weekday = (uint8_t)tm_info.tm_wday,
    };

    esp_err_t ret = tab5_rtc_set_time(&rtc_time);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RTC synced from NTP: 20%02u-%02u-%02u %02u:%02u:%02u UTC",
                 rtc_time.year, rtc_time.month, rtc_time.day,
                 rtc_time.hour, rtc_time.minute, rtc_time.second);
    }
    return ret;
}
