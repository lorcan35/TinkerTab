/* Host shim for freertos/semphr.h — no-op recursive mutex.
 *
 * Host tests are single-threaded, so the recursive-mutex contract collapses
 * to "always succeeds, never blocks".  The `(void)h` casts keep -Werror
 * -Wunused-parameter / -Wunused-variable quiet at the call sites. */
#pragma once

#include <stddef.h>

typedef int *SemaphoreHandle_t;

#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xffffffffu
#endif

typedef unsigned int TickType_t;
typedef int          BaseType_t;

static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
   /* Sentinel non-NULL pointer so callers' NULL-check passes.  Never
    * dereferenced under host — give/take are no-ops below. */
   static int s_sentinel = 1;
   return &s_sentinel;
}

static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t h, TickType_t t) {
   (void)h; (void)t;
   return pdTRUE;
}

static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t h) {
   (void)h;
   return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t h) {
   (void)h;
}
