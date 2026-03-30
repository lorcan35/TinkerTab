#pragma once
/* ESP-IDF esp_system.h stub */
#include <stdlib.h>
#include <stdint.h>

static inline void esp_restart(void) { exit(0); }
static inline uint32_t esp_get_free_heap_size(void) { return 16 * 1024 * 1024; }
static inline uint32_t esp_get_minimum_free_heap_size(void) { return 8 * 1024 * 1024; }
