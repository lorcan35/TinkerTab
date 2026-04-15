/**
 * TinkerClaw Tab5 — Battery monitor driver (INA226)
 *
 * I2C address 0x41.
 *
 * NOTE: The default INA226 address (A0=GND, A1=GND) is 0x40, which clashes
 * with the ES7210 audio codec on the Tab5.  M5Stack routes A0 high → 0x41.
 * If you see NACK at 0x41, try 0x40 (and check for ES7210 conflict).
 *
 * Battery: NP-F550, 7.4 V nominal (2S LiPo), ~2000 mAh.
 * Voltage-to-SoC is a rough linear approximation of a 2S LiPo discharge curve:
 *   8.40 V → 100 %
 *   7.40 V →  50 %
 *   6.00 V →   0 %
 */

#include "battery.h"
#include "bsp_config.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "tab5_bat";

// ---------------------------------------------------------------------------
// INA226 constants
// ---------------------------------------------------------------------------
#define INA226_ADDR            0x41

// Registers
#define INA226_REG_CONFIG      0x00
#define INA226_REG_SHUNT_V     0x01
#define INA226_REG_BUS_V       0x02
#define INA226_REG_POWER       0x03
#define INA226_REG_CURRENT     0x04
#define INA226_REG_CALIB       0x05
#define INA226_REG_MASK_EN     0x06
#define INA226_REG_ALERT_LIM   0x07
#define INA226_REG_MFR_ID      0xFE
#define INA226_REG_DIE_ID      0xFF

// Config: AVG=64, VBUS_CT=1.1 ms, VSHUNT_CT=1.1 ms, mode=shunt+bus continuous
#define INA226_CONFIG_VAL      0x4527

// Bus voltage LSB = 1.25 mV
#define INA226_BUS_V_LSB       0.00125f

// Shunt voltage LSB = 2.5 µV
#define INA226_SHUNT_V_LSB     0.0000025f

// TODO: Measure the actual shunt resistor on the Tab5 board.
//       Typical values: 10 mΩ, 20 mΩ, 50 mΩ, 100 mΩ.
//       Calibration register = 0.00512 / (Current_LSB * R_shunt)
//       Until measured, we read raw bus voltage for SoC estimation and skip
//       the current/power registers (they'll read zero without calibration).
#define INA226_SHUNT_OHMS      0.010f    // assumed 10 mΩ — VERIFY ON HARDWARE
#define INA226_CURRENT_LSB     0.001f    // 1 mA per bit

// Battery voltage thresholds (2S LiPo)
#define BAT_V_FULL  8.40f
#define BAT_V_NOM   7.40f
#define BAT_V_EMPTY 6.00f

#define I2C_TIMEOUT_MS 50

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static i2c_master_dev_handle_t s_ina_dev = NULL;

// ---------------------------------------------------------------------------
// Low-level I2C helpers (INA226 uses big-endian 16-bit registers)
// ---------------------------------------------------------------------------
static esp_err_t ina_write_reg(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(s_ina_dev, buf, 3, I2C_TIMEOUT_MS);
}

static esp_err_t ina_read_reg(uint8_t reg, uint16_t *val)
{
    uint8_t tx = reg;
    uint8_t rx[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(s_ina_dev, &tx, 1, rx, 2,
                                                 I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        *val = ((uint16_t)rx[0] << 8) | rx[1];
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Voltage-to-percentage (simple linear 2S LiPo curve)
// ---------------------------------------------------------------------------
static uint8_t voltage_to_percent(float v)
{
    if (v >= BAT_V_FULL)  return 100;
    if (v <= BAT_V_EMPTY) return 0;
    // Linear map 6.0 V → 0 %, 8.4 V → 100 %
    float pct = (v - BAT_V_EMPTY) / (BAT_V_FULL - BAT_V_EMPTY) * 100.0f;
    return (uint8_t)(pct + 0.5f);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t tab5_battery_init(i2c_master_bus_handle_t i2c_bus)
{
    if (s_ina_dev) {
        ESP_LOGW(TAG, "already initialised");
        return ESP_OK;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = INA226_ADDR,
        .scl_speed_hz    = TAB5_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &cfg, &s_ina_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to add INA226 device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify manufacturer ID (should be 0x5449 = "TI")
    uint16_t mfr_id = 0;
    ret = ina_read_reg(INA226_REG_MFR_ID, &mfr_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INA226 not responding at 0x%02X: %s",
                 INA226_ADDR, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "INA226 manufacturer ID: 0x%04X %s",
             mfr_id, (mfr_id == 0x5449) ? "(TI — OK)" : "(unexpected!)");

    // Write configuration: 64-sample averaging, 1.1 ms conversion
    ret = ina_write_reg(INA226_REG_CONFIG, INA226_CONFIG_VAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "config write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Write calibration register
    // CAL = 0.00512 / (Current_LSB * R_shunt)
    uint16_t cal = (uint16_t)(0.00512f / (INA226_CURRENT_LSB * INA226_SHUNT_OHMS));
    ret = ina_write_reg(INA226_REG_CALIB, cal);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "calibration write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "INA226 calibration register = 0x%04X (shunt=%.3f Ω, LSB=%.4f A)",
             cal, INA226_SHUNT_OHMS, INA226_CURRENT_LSB);

    ESP_LOGI(TAG, "INA226 battery monitor initialised");
    return ESP_OK;
}

esp_err_t tab5_battery_read(tab5_battery_info_t *info)
{
    if (!s_ina_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_bus = 0, raw_shunt = 0, raw_power = 0, raw_current = 0;
    esp_err_t ret;

    ret = ina_read_reg(INA226_REG_BUS_V, &raw_bus);
    if (ret != ESP_OK) goto fail;

    ret = ina_read_reg(INA226_REG_SHUNT_V, &raw_shunt);
    if (ret != ESP_OK) goto fail;

    ret = ina_read_reg(INA226_REG_POWER, &raw_power);
    if (ret != ESP_OK) goto fail;

    ret = ina_read_reg(INA226_REG_CURRENT, &raw_current);
    if (ret != ESP_OK) goto fail;

    // Bus voltage
    info->voltage = (float)raw_bus * INA226_BUS_V_LSB;

    // Current (signed 16-bit, twos complement)
    int16_t signed_current = (int16_t)raw_current;
    info->current = (float)signed_current * INA226_CURRENT_LSB;

    // Power (unsigned, LSB = 25 * Current_LSB as per INA226 datasheet)
    info->power = (float)raw_power * 25.0f * INA226_CURRENT_LSB;

    // Percentage from voltage
    info->percent = voltage_to_percent(info->voltage);

    // Charging: negative current means current flowing into the battery
    // (depends on shunt orientation — may need to flip sign after HW testing)
    info->charging = (signed_current < 0);

    ESP_LOGD(TAG, "V=%.2f I=%.3f P=%.2f pct=%u%% chg=%d",
             info->voltage, info->current, info->power,
             info->percent, info->charging);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(ret));
    return ret;
}

uint8_t tab5_battery_percent(void)
{
    tab5_battery_info_t info = {0};
    if (tab5_battery_read(&info) != ESP_OK) {
        return 0;
    }
    return info.percent;
}

bool tab5_battery_charging(void)
{
    tab5_battery_info_t info = {0};
    if (tab5_battery_read(&info) != ESP_OK) {
        return false;
    }
    return info.charging;
}
