#pragma once
/* ESP-IDF esp_timer.h stub for desktop simulator */
#include "esp_err.h"
#include <stdint.h>
#include <time.h>

typedef void (*esp_timer_cb_t)(void *arg);

typedef struct {
    esp_timer_cb_t callback;
    void          *arg;
    const char    *name;
} esp_timer_create_args_t;

typedef void *esp_timer_handle_t;

static inline int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    (void)args; *out = NULL; return ESP_OK;
}
static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period) { return ESP_OK; }
static inline esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t delay) { return ESP_OK; }
static inline esp_err_t esp_timer_stop(esp_timer_handle_t t) { return ESP_OK; }
static inline esp_err_t esp_timer_delete(esp_timer_handle_t t) { return ESP_OK; }
