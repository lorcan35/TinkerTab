#pragma once
/* ESP-IDF esp_task_wdt.h stub — all no-ops on desktop */
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t timeout_ms;
    uint32_t idle_core_mask;
    bool     trigger_panic;
} esp_task_wdt_config_t;

static inline esp_err_t esp_task_wdt_init(uint32_t timeout, bool panic) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *cfg) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_add(void *handle) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(void *handle) { return ESP_OK; }
