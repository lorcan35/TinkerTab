#pragma once
/* FreeRTOS.h stub for desktop simulator */
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t TickType_t;
typedef void    *TaskHandle_t;
typedef void    *QueueHandle_t;
typedef void    *SemaphoreHandle_t;
typedef long     BaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY 0xFFFFFFFF

static inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
static inline int xPortGetCoreID(void) { return 0; }
