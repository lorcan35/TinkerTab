/*
 * task_worker.h — wave 14 W14-H06 shared job queue.
 *
 * Replaces the "spawn a throwaway task that does one thing then
 * vTaskSuspend(NULL)" pattern that was leaking 8 KB of PSRAM + one TCB
 * per user-reachable action (mic tap, wifi retry, widget-media URL
 * change, session-drawer open). After ~40 mic taps or ~200 drawer
 * opens the PSRAM fragmentation watchdog eventually rebooted the
 * device. With this queue, every "background blip" becomes a single
 * enqueue() call, and one long-lived worker task handles them all in
 * sequence.
 *
 * Concurrency: single worker, single-threaded with respect to the
 * jobs it runs. Jobs must not block LVGL.  If a job needs the LVGL
 * mutex it takes it itself.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Maximum number of queued jobs. Jobs past this are dropped + logged. */
#define TAB5_WORKER_QUEUE_DEPTH 16

/* Jobs are just a function pointer + a single void* argument.  The
 * worker does NOT free(arg) — if the caller malloc'd it, the job
 * function must free it.  `tag` is a short string used in the
 * ESP_LOGI "running job X" trace. */
typedef void (*tab5_worker_fn_t)(void *arg);

/* Create the worker task.  Idempotent.  Must be called once at boot
 * before any enqueue — typically from main.c after NVS init. */
esp_err_t tab5_worker_init(void);

/* Post a job to the worker.  Returns ESP_OK on success,
 * ESP_ERR_NO_MEM if the queue is full (caller MUST handle — the
 * argument they passed will not be freed by the worker). */
esp_err_t tab5_worker_enqueue(tab5_worker_fn_t fn,
                              void *arg,
                              const char *tag);
