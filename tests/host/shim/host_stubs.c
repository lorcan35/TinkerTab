/* host_stubs.c — single-TU definitions for the esp_timer fake-clock and
 * the task_worker FIFO shims (declared in shim/esp_timer.h + task_worker.h).
 *
 * Shared host state lives here so every TU in the test executable sees one
 * clock + one job queue. Linked only into test_voice_dictation. */

#include "esp_timer.h"
#include "task_worker.h"

/* ── fake clock + one-shot timer registry ───────────────────────────── */

static int64_t s_now_us = 0;

struct host_esp_timer {
   esp_timer_cb_t cb;
   void *arg;
   bool armed;
   bool used;
   int64_t deadline_us;
};

#define HOST_TIMER_MAX 8
static struct host_esp_timer s_timers[HOST_TIMER_MAX];

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
   if (!args || !out) return ESP_ERR_INVALID_ARG;
   for (int i = 0; i < HOST_TIMER_MAX; i++) {
      if (!s_timers[i].used) {
         s_timers[i].used = true;
         s_timers[i].cb = args->callback;
         s_timers[i].arg = args->arg;
         s_timers[i].armed = false;
         *out = &s_timers[i];
         return ESP_OK;
      }
   }
   return ESP_ERR_NO_MEM;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us) {
   if (!t) return ESP_ERR_INVALID_ARG;
   t->armed = true;
   t->deadline_us = s_now_us + (int64_t)timeout_us;
   return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t t) {
   if (t) t->armed = false;
   return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t) {
   if (t) {
      t->armed = false;
      t->used = false;
   }
   return ESP_OK;
}

int64_t esp_timer_get_time(void) { return s_now_us; }

void host_clock_set_ms(uint32_t ms) { s_now_us = (int64_t)ms * 1000; }

void host_clock_advance_ms(uint32_t ms) {
   s_now_us += (int64_t)ms * 1000;
   /* Fire every armed one-shot whose deadline has passed. Disarm BEFORE
    * calling so a re-arm inside the callback (the full-queue retry path,
    * which arms a FUTURE deadline) sticks and does not re-fire this pass.
    * Loop until quiescent in case a callback arms another due timer. */
   bool fired;
   do {
      fired = false;
      for (int i = 0; i < HOST_TIMER_MAX; i++) {
         if (s_timers[i].used && s_timers[i].armed && s_timers[i].deadline_us <= s_now_us) {
            s_timers[i].armed = false;
            if (s_timers[i].cb) s_timers[i].cb(s_timers[i].arg);
            fired = true;
         }
      }
   } while (fired);
}

/* ── task_worker FIFO ────────────────────────────────────────────────── */

typedef struct {
   tab5_worker_fn_t fn;
   void *arg;
} host_job_t;

static host_job_t s_jobs[TAB5_WORKER_QUEUE_DEPTH];
static int s_job_head = 0, s_job_tail = 0, s_job_count = 0;
static bool s_force_full = false;

esp_err_t tab5_worker_init(void) { return ESP_OK; }

esp_err_t tab5_worker_enqueue(tab5_worker_fn_t fn, void *arg, const char *tag) {
   (void)tag;
   if (s_force_full || s_job_count >= TAB5_WORKER_QUEUE_DEPTH) return ESP_ERR_NO_MEM;
   s_jobs[s_job_tail].fn = fn;
   s_jobs[s_job_tail].arg = arg;
   s_job_tail = (s_job_tail + 1) % TAB5_WORKER_QUEUE_DEPTH;
   s_job_count++;
   return ESP_OK;
}

void tab5_worker_pump(void) {
   while (s_job_count > 0) {
      host_job_t j = s_jobs[s_job_head];
      s_job_head = (s_job_head + 1) % TAB5_WORKER_QUEUE_DEPTH;
      s_job_count--;
      if (j.fn) j.fn(j.arg);
   }
}

void tab5_worker_stub_set_full(bool full) { s_force_full = full; }

/* ── shared reset (called per test after voice_dictation_init) ───────── */

void host_test_reset(void) {
   s_now_us = 0;
   for (int i = 0; i < HOST_TIMER_MAX; i++) s_timers[i].armed = false;
   s_job_head = s_job_tail = s_job_count = 0;
   s_force_full = false;
}
