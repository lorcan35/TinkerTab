#pragma once
/* ESP-IDF esp_event.h stub */
#include "esp_err.h"
#include <stdint.h>

typedef void *esp_event_loop_handle_t;
typedef int32_t esp_event_base_t;
#define WIFI_EVENT   ((esp_event_base_t)1)
#define IP_EVENT     ((esp_event_base_t)2)
#define ESP_EVENT_ANY_ID (-1)

typedef void (*esp_event_handler_t)(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

static inline esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
static inline esp_err_t esp_event_handler_instance_register(esp_event_base_t b, int32_t id, esp_event_handler_t h, void *a, void **i) { return ESP_OK; }
static inline esp_err_t esp_event_handler_register(esp_event_base_t b, int32_t id, esp_event_handler_t h, void *a) { return ESP_OK; }
