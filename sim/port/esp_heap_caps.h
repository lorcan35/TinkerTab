#pragma once
/* ESP-IDF esp_heap_caps.h stub for desktop simulator */
#include <stdlib.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM       (1 << 3)
#define MALLOC_CAP_8BIT         (1 << 4)
#define MALLOC_CAP_DMA          (1 << 5)
#define MALLOC_CAP_INTERNAL     (1 << 6)

static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}

static inline void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
    (void)caps;
    return realloc(ptr, size);
}

static inline void heap_caps_free(void *ptr) {
    free(ptr);
}

static inline size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return 16 * 1024 * 1024; /* pretend 16MB free */
}

static inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return 8 * 1024 * 1024;
}
