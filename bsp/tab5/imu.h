/**
 * @file imu.h
 * @brief BMI270 IMU driver for M5Stack Tab5 (ESP32-P4)
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 3-axis vector (acceleration in g, angular rate in dps) */
typedef struct {
    float x;
    float y;
    float z;
} tab5_imu_vec3_t;

/** Combined accelerometer + gyroscope reading */
typedef struct {
    tab5_imu_vec3_t accel;  /**< Acceleration in g */
    tab5_imu_vec3_t gyro;   /**< Angular rate in degrees/s */
} tab5_imu_data_t;

/** Screen orientation derived from accelerometer */
typedef enum {
    TAB5_ORIENT_PORTRAIT,       /**< Normal portrait (USB down) */
    TAB5_ORIENT_LANDSCAPE,      /**< Landscape (rotated CW) */
    TAB5_ORIENT_PORTRAIT_INV,   /**< Inverted portrait (USB up) */
    TAB5_ORIENT_LANDSCAPE_INV,  /**< Landscape (rotated CCW) */
} tab5_orientation_t;

/**
 * @brief Initialize the BMI270 IMU on the given I2C bus.
 *
 * Performs soft reset, verifies chip ID, configures accel (100 Hz, +/-4g)
 * and gyro (100 Hz, +/-2000 dps), then enters normal power mode.
 *
 * @param i2c_bus  Handle to an already-initialized I2C master bus.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t tab5_imu_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Read the latest accelerometer and gyroscope data.
 *
 * @param[out] data  Pointer to receive the converted readings.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t tab5_imu_read(tab5_imu_data_t *data);

/**
 * @brief Determine the current screen orientation from the accel vector.
 *
 * Reads the accelerometer internally and returns the dominant-axis orientation.
 * Returns TAB5_ORIENT_PORTRAIT if the read fails.
 */
tab5_orientation_t tab5_imu_get_orientation(void);

/**
 * @brief Check whether a tap gesture was detected since the last call.
 *
 * Uses accel magnitude spike detection. Non-blocking.
 */
bool tab5_imu_detect_tap(void);

/**
 * @brief Check whether a shake gesture was detected since the last call.
 *
 * Detects sustained high-frequency acceleration changes. Non-blocking.
 */
bool tab5_imu_detect_shake(void);

#ifdef __cplusplus
}
#endif
