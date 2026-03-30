#pragma once
/* FreeRTOS task.h stub */
#include "FreeRTOS.h"
#include <unistd.h>

static inline void vTaskDelay(TickType_t ticks) { usleep(ticks * 1000); }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char *n,
    uint32_t stack, void *arg, uint32_t prio, TaskHandle_t *h, int core) {
    (void)fn; (void)n; (void)stack; (void)arg; (void)prio; (void)h; (void)core;
    return pdPASS; /* tasks not actually spawned on desktop */
}
static inline BaseType_t xTaskCreate(void (*fn)(void*), const char *n,
    uint32_t stack, void *arg, uint32_t prio, TaskHandle_t *h) {
    (void)fn; (void)n; (void)stack; (void)arg; (void)prio; (void)h;
    return pdPASS;
}
static inline void vTaskSuspend(TaskHandle_t h) { (void)h; }
static inline void vTaskResume(TaskHandle_t h) { (void)h; }
