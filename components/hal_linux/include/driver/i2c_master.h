#pragma once
/* Stub: driver/i2c_master.h for linux target */
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef int   i2c_port_num_t;

#define I2C_CLK_SRC_DEFAULT 0
#define I2C_NUM_0 0
#define I2C_NUM_1 1

typedef struct {
    i2c_port_num_t  i2c_port;
    int             sda_io_num;
    int             scl_io_num;
    int             clk_source;
    uint8_t         glitch_ignore_cnt;
    struct {
        uint32_t enable_internal_pullup : 1;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    uint16_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

static inline esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg,
    i2c_master_bus_handle_t *ret) { if (ret) *ret = (void*)1; return ESP_FAIL; }
static inline esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus,
    uint16_t addr, int timeout_ms) { return ESP_FAIL; }
static inline esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
    const i2c_device_config_t *cfg, i2c_master_dev_handle_t *ret_dev) { return ESP_FAIL; }
static inline esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
    const uint8_t *data, size_t len, int timeout_ms) { return ESP_FAIL; }
static inline esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev,
    uint8_t *data, size_t len, int timeout_ms) { return ESP_FAIL; }
static inline esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
    const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
    int timeout_ms) { return ESP_FAIL; }
static inline esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { return ESP_OK; }
static inline esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus) { return ESP_OK; }
