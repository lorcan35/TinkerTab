#pragma once
/* FreeRTOS semphr.h stub */
#include "FreeRTOS.h"
#include <stdlib.h>

static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (void*)1; }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void*)1; }
static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) { return pdTRUE; }
static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return pdTRUE; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { return pdTRUE; }
