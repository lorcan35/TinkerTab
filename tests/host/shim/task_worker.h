/* Host shim for task_worker.h — synchronous FIFO job queue.
 *
 * Shadows main/task_worker.h under the host build (SHIM_DIR precedes
 * MAIN_DIR on the include path).  Jobs are queued by tab5_worker_enqueue
 * and run, FIFO-order, only when the test calls tab5_worker_pump() — so
 * the decay enqueue→apply interleave is fully test-controlled.  Definition
 * lives in host_stubs.c.  Mirrors the real contract: the worker does NOT
 * free(arg). */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define TAB5_WORKER_QUEUE_DEPTH 16

typedef void (*tab5_worker_fn_t)(void *arg);

esp_err_t tab5_worker_init(void);
esp_err_t tab5_worker_enqueue(tab5_worker_fn_t fn, void *arg, const char *tag);

/* ── host-test helpers (not part of the real task_worker API) ── */
void tab5_worker_pump(void);               /* run all queued jobs, FIFO order */
void tab5_worker_stub_set_full(bool full); /* force enqueue to return ESP_ERR_NO_MEM */
