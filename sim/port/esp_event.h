#pragma once
/* ESP-IDF esp_event.h stub */
#include "esp_err.h"
#include <stdint.h>

typedef void *esp_event_loop_handle_t;
typedef void *esp_event_handler_instance_t;
typedef int esp_event_base_t;

#define ESP_EVENT_ANY_ID -1

static inline esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
static inline esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
    void *handler, void *handler_arg) { return ESP_OK; }
static inline esp_err_t esp_event_handler_instance_register(
    esp_event_base_t base, int32_t id, void *handler, void *handler_arg,
    esp_event_handler_instance_t *inst) { if (inst) *inst = NULL; return ESP_OK; }
static inline esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t base, int32_t id, esp_event_handler_instance_t inst) { return ESP_OK; }
static inline esp_err_t esp_event_post(esp_event_base_t base, int32_t id,
    void *event_data, size_t data_size, uint32_t ticks) { return ESP_OK; }
