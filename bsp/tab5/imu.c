/**
 * @file imu.c
 * @brief BMI270 IMU driver for M5Stack Tab5 (ESP32-P4)
 *
 * NOTE on BMI270 config upload:
 *   The BMI270 requires an ~8 KB binary firmware blob uploaded to register 0x5E
 *   after soft-reset before the sensor is fully operational.  Bosch distributes
 *   this blob in their BMI270 Sensor API (bmi270_config_file[]).
 *
 *   This driver attempts basic accel/gyro operation WITHOUT the full config
 *   upload.  If the chip returns all-zeros or fails to leave config-load mode,
 *   you must:
 *     1. Obtain bmi270_config_file[] from the Bosch SensorTec GitHub repo.
 *     2. Call tab5_bmi270_upload_config() with that blob before enabling
 *        accel/gyro.
 *
 *   A stub upload function is included below and clearly marked.
 */

#include "imu.h"

#include <math.h>
#include <string.h>

#include "bmi270_config.h" /* TT #511 fix: Bosch's required firmware blob */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── BMI270 register map ──────────────────────────────────────────────── */

#define BMI270_I2C_ADDR         0x68

#define BMI270_REG_CHIP_ID      0x00
#define BMI270_CHIP_ID_VAL      0x24

#define BMI270_REG_DATA_8       0x0C  /* accel X LSB (12 bytes: aX aY aZ gX gY gZ) */

#define BMI270_REG_ACC_CONF     0x40
#define BMI270_REG_ACC_RANGE    0x41
#define BMI270_REG_GYR_CONF     0x42
#define BMI270_REG_GYR_RANGE    0x43

#define BMI270_REG_INIT_CTRL    0x59
#define BMI270_REG_INIT_ADDR_0 0x5B /* 2-byte burst sets word-index for next chunk */
#define BMI270_REG_INIT_DATA    0x5E
#define BMI270_REG_INTERNAL_STATUS 0x21

#define BMI270_REG_PWR_CONF     0x7C
#define BMI270_REG_PWR_CTRL     0x7D
#define BMI270_REG_CMD          0x7E

#define BMI270_CMD_SOFT_RESET   0xB6

/* Accel config: ODR 100 Hz, OSR4, continuous filter */
#define BMI270_ACC_CONF_VAL     0xA8  /* osr4 | odr_100hz (0x08) | bwp normal */
#define BMI270_ACC_RANGE_4G     0x01  /* +/- 4g */

/* Gyro config: ODR 100 Hz, OSR4, noise-perf normal */
#define BMI270_GYR_CONF_VAL     0xA9  /* osr4 | odr_100hz */
#define BMI270_GYR_RANGE_2000   0x00  /* +/- 2000 dps */

/* Scale factors */
#define ACCEL_SCALE             (4.0f / 32768.0f)    /* g per LSB at +/-4g  */
#define GYRO_SCALE              (2000.0f / 32768.0f)  /* dps per LSB at +/-2000 */

/* Gesture thresholds */
#define TAP_THRESHOLD_G         1.8f   /* accel magnitude spike (g)          */
#define TAP_COOLDOWN_US         300000 /* 300 ms between taps                */
#define SHAKE_THRESHOLD_G       2.0f   /* accel delta to count as shake axis */
#define SHAKE_COUNT_NEEDED      4      /* hits within the window             */
#define SHAKE_WINDOW_US         800000 /* 800 ms rolling window              */

#define I2C_TIMEOUT_MS          100

static const char *TAG = "tab5_imu";

/* ── Module state ─────────────────────────────────────────────────────── */

static i2c_master_dev_handle_t s_imu_dev  = NULL;
static bool                    s_inited   = false;

/* Tap detection state */
static int64_t s_last_tap_us = 0;

/* Shake detection state */
static tab5_imu_vec3_t s_prev_accel     = {0};
static int64_t         s_shake_hits_us[SHAKE_COUNT_NEEDED];
static int             s_shake_idx      = 0;
static bool            s_have_prev      = false;

/* ── Low-level I2C helpers ────────────────────────────────────────────── */

static esp_err_t imu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_imu_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t imu_read_reg(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_imu_dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
}

/* ── BMI270 config upload stub ────────────────────────────────────────── */

/**
 * Upload the BMI270 config blob.  Replace the NULL / 0 with the real
 * bmi270_config_file[] array from Bosch's BMI270 Sensor API when available.
 *
 * Without this, the chip may still output basic accel/gyro data on some
 * silicon revisions, but features like step-counter and gesture engine
 * will not work.
 */
static esp_err_t bmi270_upload_config(void)
{
   /* TT #511 fix (2026-05-14): linked the real Bosch firmware blob.
    * The Tab5's BMI270 silicon returns all-zero accel/gyro until this
    * ~8 KB blob is burst-written to register INIT_DATA (0x5E).  Diagnosed
    * via GET /imu showing accel.x/y/z = 0 even when device was tilted;
    * the chip ID read at 0x00 returned the correct 0x24, proving I2C
    * was healthy and the issue was firmware-not-loaded.  See
    * bsp/tab5/bmi270_config.c for the blob + Bosch BSD-3 attribution. */
   const uint8_t *cfg = bmi270_config_file;
   const size_t cfg_len = bmi270_config_file_len;

   if (cfg == NULL || cfg_len == 0) {
      ESP_LOGW(TAG, "BMI270 config blob not linked — skipping firmware upload");
      ESP_LOGW(TAG, "Accel/gyro will return zeros on Tab5 silicon");
      return ESP_OK; /* non-fatal: try basic mode */
   }
   ESP_LOGI(TAG, "Uploading BMI270 config blob (%u bytes)...", (unsigned)cfg_len);

   esp_err_t ret;

   /* Prepare chip for config load */
   ret = imu_write_reg(BMI270_REG_PWR_CONF, 0x00); /* adv_power_save off */
   if (ret != ESP_OK) return ret;
   vTaskDelay(pdMS_TO_TICKS(1));

   ret = imu_write_reg(BMI270_REG_INIT_CTRL, 0x00); /* start config load */
   if (ret != ESP_OK) return ret;

   /* Burst-write config in 256-byte chunks.  Per Bosch's canonical
    * write_config_file() in BMI270_SensorAPI/bmi2.c, BEFORE each chunk
    * we MUST set INIT_ADDR_0/1 to the chunk's word-index (byte offset
    * / 2), otherwise the BMI270 piles every chunk at offset 0 and
    * INTERNAL_STATUS reads 0x00 instead of 0x01 after INIT_CTRL=1.
    * That's the bug TT #511's IMU work hit on first-flash 2026-05-14. */
   const size_t chunk = 256;
   for (size_t off = 0; off < cfg_len; off += chunk) {
      size_t n = (cfg_len - off < chunk) ? (cfg_len - off) : chunk;
      /* Word-index = byte-offset / 2 (BMI270 internal RAM is 16-bit wide) */
      uint16_t widx = (uint16_t)(off / 2);
      uint8_t addr_buf[3] = {
          BMI270_REG_INIT_ADDR_0,
          (uint8_t)(widx & 0x0F),
          (uint8_t)(widx >> 4),
      };
      ret = i2c_master_transmit(s_imu_dev, addr_buf, sizeof(addr_buf), I2C_TIMEOUT_MS);
      if (ret != ESP_OK) {
         ESP_LOGE(TAG, "INIT_ADDR write failed at offset %u: %s", (unsigned)off, esp_err_to_name(ret));
         return ret;
      }
      uint8_t buf[1 + 256];
      buf[0] = BMI270_REG_INIT_DATA;
      memcpy(&buf[1], cfg + off, n);
      ret = i2c_master_transmit(s_imu_dev, buf, 1 + n, I2C_TIMEOUT_MS);
      if (ret != ESP_OK) {
         ESP_LOGE(TAG, "Config upload failed at offset %u: %s", (unsigned)off, esp_err_to_name(ret));
         return ret;
      }
    }

    ret = imu_write_reg(BMI270_REG_INIT_CTRL, 0x01);  /* complete config load */
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));  /* wait for internal init */

    /* Verify */
    uint8_t status = 0;
    ret = imu_read_reg(BMI270_REG_INTERNAL_STATUS, &status, 1);
    if (ret != ESP_OK) return ret;

    if ((status & 0x0F) != 0x01) {
        ESP_LOGE(TAG, "BMI270 init status 0x%02X (expected 0x01)", status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "BMI270 config uploaded successfully (%u bytes)", (unsigned)cfg_len);
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────────────────── */

esp_err_t tab5_imu_init(i2c_master_bus_handle_t i2c_bus)
{
    if (s_inited) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    esp_err_t ret;

    /* Add device to bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BMI270_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_imu_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ── Soft reset ─────────────────────────────────────────────────── */
    ret = imu_write_reg(BMI270_REG_CMD, BMI270_CMD_SOFT_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Soft reset write failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    /*
     * On the Tab5 rollback build the BMI270 is visible on the bus, but an
     * immediate post-reset chip-ID read can transiently fail with
     * ESP_ERR_INVALID_STATE. Give the sensor time to come back and retry the
     * first read a few times before treating init as failed.
     */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ── Verify chip ID ─────────────────────────────────────────────── */
    uint8_t chip_id = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        ret = imu_read_reg(BMI270_REG_CHIP_ID, &chip_id, 1);
        if (ret == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Chip ID read failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    if (chip_id != BMI270_CHIP_ID_VAL) {
        ESP_LOGE(TAG, "Unexpected chip ID 0x%02X (expected 0x%02X)", chip_id, BMI270_CHIP_ID_VAL);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    ESP_LOGI(TAG, "BMI270 detected (chip ID 0x%02X)", chip_id);

    /* ── Upload config firmware ─────────────────────────────────────── */
    ret = bmi270_upload_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config upload failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* ── Accelerometer: 100 Hz ODR, +/-4g ──────────────────────────── */
    ret = imu_write_reg(BMI270_REG_ACC_CONF, BMI270_ACC_CONF_VAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Accel config failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = imu_write_reg(BMI270_REG_ACC_RANGE, BMI270_ACC_RANGE_4G);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Accel range failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* ── Gyroscope: 100 Hz ODR, +/-2000 dps ────────────────────────── */
    ret = imu_write_reg(BMI270_REG_GYR_CONF, BMI270_GYR_CONF_VAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Gyro config failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = imu_write_reg(BMI270_REG_GYR_RANGE, BMI270_GYR_RANGE_2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Gyro range failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* ── Power on accel + gyro (normal mode) ────────────────────────── */
    ret = imu_write_reg(BMI270_REG_PWR_CONF, 0x00);  /* adv_power_save off */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PWR_CONF write failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = imu_write_reg(BMI270_REG_PWR_CTRL, 0x0E);  /* acc_en | gyr_en | temp_en */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PWR_CTRL write failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* Let sensors stabilize */
    vTaskDelay(pdMS_TO_TICKS(50));

    s_inited = true;
    ESP_LOGI(TAG, "BMI270 initialized — accel +/-4g @ 100 Hz, gyro +/-2000 dps @ 100 Hz");
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(s_imu_dev);
    s_imu_dev = NULL;
    return ret;
}

esp_err_t tab5_imu_read(tab5_imu_data_t *data)
{
    if (!s_inited || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * BMI270 data registers (0x0C..0x17):
     *   0x0C-0x0D  ACC_X   (LSB, MSB)
     *   0x0E-0x0F  ACC_Y
     *   0x10-0x11  ACC_Z
     *   0x12-0x13  GYR_X
     *   0x14-0x15  GYR_Y
     *   0x16-0x17  GYR_Z
     */
    uint8_t raw[12];
    esp_err_t ret = imu_read_reg(BMI270_REG_DATA_8, raw, sizeof(raw));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Data read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Unpack little-endian 16-bit signed values */
    int16_t ax = (int16_t)((raw[1]  << 8) | raw[0]);
    int16_t ay = (int16_t)((raw[3]  << 8) | raw[2]);
    int16_t az = (int16_t)((raw[5]  << 8) | raw[4]);
    int16_t gx = (int16_t)((raw[7]  << 8) | raw[6]);
    int16_t gy = (int16_t)((raw[9]  << 8) | raw[8]);
    int16_t gz = (int16_t)((raw[11] << 8) | raw[10]);

    data->accel.x = (float)ax * ACCEL_SCALE;
    data->accel.y = (float)ay * ACCEL_SCALE;
    data->accel.z = (float)az * ACCEL_SCALE;

    data->gyro.x = (float)gx * GYRO_SCALE;
    data->gyro.y = (float)gy * GYRO_SCALE;
    data->gyro.z = (float)gz * GYRO_SCALE;

    return ESP_OK;
}

tab5_orientation_t tab5_imu_get_orientation(void)
{
    tab5_imu_data_t d;
    if (tab5_imu_read(&d) != ESP_OK) {
        return TAB5_ORIENT_PORTRAIT;  /* safe default */
    }

    float ax = d.accel.x;
    float ay = d.accel.y;
    float az = d.accel.z;

    float abs_x = fabsf(ax);
    float abs_y = fabsf(ay);
    float abs_z = fabsf(az);

    /*
     * If Z is the dominant axis the device is flat — fall back to portrait.
     * Otherwise pick the dominant X/Y axis to determine orientation.
     *
     * Axis mapping assumes the BMI270 is mounted with:
     *   +Y pointing towards the top of the device in portrait
     *   +X pointing towards the right of the device in portrait
     * Adjust signs if the physical mounting differs.
     */
    if (abs_z > abs_x && abs_z > abs_y) {
        return TAB5_ORIENT_PORTRAIT;  /* flat on table, keep current */
    }

    if (abs_y >= abs_x) {
        return (ay > 0) ? TAB5_ORIENT_PORTRAIT : TAB5_ORIENT_PORTRAIT_INV;
    } else {
        return (ax > 0) ? TAB5_ORIENT_LANDSCAPE : TAB5_ORIENT_LANDSCAPE_INV;
    }
}

bool tab5_imu_detect_tap(void)
{
    tab5_imu_data_t d;
    if (tab5_imu_read(&d) != ESP_OK) {
        return false;
    }

    float mag = sqrtf(d.accel.x * d.accel.x +
                      d.accel.y * d.accel.y +
                      d.accel.z * d.accel.z);

    int64_t now = esp_timer_get_time();

    if (mag > TAP_THRESHOLD_G && (now - s_last_tap_us) > TAP_COOLDOWN_US) {
        s_last_tap_us = now;
        ESP_LOGD(TAG, "Tap detected (mag=%.2f g)", mag);
        return true;
    }

    return false;
}

bool tab5_imu_detect_shake(void)
{
    tab5_imu_data_t d;
    if (tab5_imu_read(&d) != ESP_OK) {
        return false;
    }

    if (!s_have_prev) {
        s_prev_accel = d.accel;
        s_have_prev  = true;
        return false;
    }

    /* Compute delta from previous sample */
    float dx = fabsf(d.accel.x - s_prev_accel.x);
    float dy = fabsf(d.accel.y - s_prev_accel.y);
    float dz = fabsf(d.accel.z - s_prev_accel.z);
    float delta = dx + dy + dz;

    s_prev_accel = d.accel;

    int64_t now = esp_timer_get_time();

    if (delta > SHAKE_THRESHOLD_G) {
        s_shake_hits_us[s_shake_idx % SHAKE_COUNT_NEEDED] = now;
        s_shake_idx++;

        /* Check if we have enough hits within the window */
        if (s_shake_idx >= SHAKE_COUNT_NEEDED) {
            int oldest = (s_shake_idx) % SHAKE_COUNT_NEEDED;
            int64_t span = now - s_shake_hits_us[oldest];

            if (span <= SHAKE_WINDOW_US) {
                /* Reset so we don't re-trigger immediately */
                s_shake_idx = 0;
                memset(s_shake_hits_us, 0, sizeof(s_shake_hits_us));
                ESP_LOGD(TAG, "Shake detected (span=%lld us)", (long long)span);
                return true;
            }
        }
    }

    return false;
}
