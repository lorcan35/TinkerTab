/* Host shim for esp_timer.h — fake-clock one-shot timers.
 *
 * The fake clock + timer registry are SHARED across translation units
 * (voice_dictation.c, the test TU, host_stubs.c), so this header only
 * DECLARES the API; the single definition lives in host_stubs.c.  Tests
 * drive time with host_clock_advance_ms(), which fires any armed one-shot
 * whose deadline has passed — making the FSM self-decay deterministic. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*esp_timer_cb_t)(void *arg);
typedef struct host_esp_timer *esp_timer_handle_t;
typedef enum { ESP_TIMER_TASK = 0 } esp_timer_dispatch_t;

typedef struct {
   esp_timer_cb_t callback;
   void *arg;
   esp_timer_dispatch_t dispatch_method;
   const char *name;
   bool skip_unhandled_events;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
esp_err_t esp_timer_delete(esp_timer_handle_t t);
int64_t esp_timer_get_time(void);

/* ── host-test helpers (not part of the real esp_timer API) ── */
void host_clock_set_ms(uint32_t ms);
void host_clock_advance_ms(uint32_t ms); /* advances clock + fires due one-shots */
void host_test_reset(void);              /* reset clock + worker FIFO + disarm timers */
