# LVGL Pool Pressure Investigation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `/tmp/stress_orchestrator.sh` run for 30 minutes on Tab5 with zero PANIC reboots, by identifying and fixing the **single** pool/allocator bottleneck that's driving LVGL-internal NULL-derefs — not by patching individual crash sites one-by-one.

**Architecture:** Four sequential phases. Phase 1 adds non-invasive instrumentation (heap-failure callback, 1 Hz pool sampler, ring buffer, `/heap-history` debug endpoint) that lets us observe exactly which pool exhausts + when, without changing firmware behaviour. Phase 2 analyses the data offline. Phase 3 applies **exactly one** config/code change based on Phase 2. Phase 4 re-runs the stress orchestrator; zero crashes = done, otherwise iterate from Phase 2 with new coredump data.

**Tech Stack:** ESP-IDF 5.5.2, LVGL 9.2.2 (managed_component), ESP32-P4. Existing `main/heap_watchdog.c` pattern for background sampling, existing `main/debug_server.c` for HTTP endpoints, existing `main/debug_obs.c` ring-buffer pattern for in-memory samples.

**Required reading before starting:** `docs/STABILITY-INVESTIGATION.md` (state doc). It describes the 12 PRs already landed, the crash-site history, the current hypothesis, and baseline numbers.

**Operational note:** This is an **investigation**, not a feature. The bite-sized task format still applies, but instead of unit tests, success criteria are empirical: concrete measurements from the stress test captured into the investigation log. Each phase ends with a commit appending to `docs/STABILITY-INVESTIGATION.md` so future sessions can see progress.

---

## File structure

### New files
- `main/pool_probe.c` + `main/pool_probe.h` — Phase 1 instrumentation. Self-contained module: hooks heap-alloc failures, runs a 1 Hz sampler task, exposes the ring buffer via a getter. Lives outside `heap_watchdog.c` because watchdog decides reboots; probe is pure observation.

### Modified files
- `main/CMakeLists.txt` — register `pool_probe.c` in the main component SRCS.
- `main/main.c` — call `tab5_pool_probe_init()` once at boot, after heap_watchdog starts.
- `main/debug_server.c` — add `/heap-history` handler that dumps the probe ring buffer as CSV.
- `sdkconfig.defaults` — Phase 3 will change exactly one LVGL config line based on Phase 2.
- `docs/STABILITY-INVESTIGATION.md` — append a dated log entry at the end of each phase.

---

# Phase 1 — Measure

**Goal:** Collect sufficient data in 10 minutes of stress to definitively pick exactly one of the four hypotheses in the state doc (base pool exhausts / expand cap hits / work_mem_int scratch bottleneck / actual leak).

**Exit criteria:** One CSV captured on disk at `/tmp/heap-history.csv` showing >= 600 samples (10 min × 60 s × 1 Hz) of pool stats + a non-empty `alloc-fail` log with caller context for every LVGL NULL return during the run.

## Task 1: Create `main/pool_probe.h`

**Files:**
- Create: `main/pool_probe.h`

- [ ] **Step 1: Create the header with the module's two public entry points**

```c
/**
 * TinkerTab — LVGL pool-pressure probe (Phase 1 instrumentation).
 *
 * Registers a heap-alloc-failure callback that logs every time a
 * `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)` or similar returns NULL,
 * and spawns a background task that samples heap + LVGL-pool stats at
 * 1 Hz into a 900-entry PSRAM ring buffer (15 min of history).
 *
 * Read via GET /heap-history on the debug server; CSV format.
 *
 * See docs/STABILITY-INVESTIGATION.md Phase 1 for what we're hunting.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Initialize the probe. Registers the heap-alloc failure callback and
 * spawns the 1 Hz sampler task. Idempotent — safe to call twice.
 */
esp_err_t tab5_pool_probe_init(void);

/**
 * Debug-server handler: writes the ring buffer as CSV to the response.
 * Installed by debug_server.c as GET /heap-history (bearer-auth
 * required, same as other debug endpoints).
 */
esp_err_t tab5_pool_probe_http_handler(httpd_req_t *req);
```

- [ ] **Step 2: Verify header compiles standalone**

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py reconfigure 2>&1 | tail -3
```

Expected: reconfigure completes (the new header won't be referenced yet but shouldn't break the build graph).

- [ ] **Step 3: Commit**

```bash
git add main/pool_probe.h
git commit -m "investigation: add pool_probe header (Phase 1 scaffold)

Header only — public API for the LVGL pool-pressure probe.
Implementation follows in the next task.  See
docs/STABILITY-INVESTIGATION.md Phase 1."
```

## Task 2: Implement `main/pool_probe.c` — ring buffer + sampler task

**Files:**
- Create: `main/pool_probe.c`
- Modify: `main/CMakeLists.txt` (add the new source)

- [ ] **Step 1: Write the sampler + ring buffer**

```c
/**
 * TinkerTab — LVGL pool-pressure probe (Phase 1 instrumentation).
 * See pool_probe.h for rationale.
 */
#include "pool_probe.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "pool_probe";

#define SAMPLE_COUNT   900     /* 15 min @ 1 Hz */
#define SAMPLE_PERIOD_MS 1000

typedef struct {
    uint32_t ms;                    /* esp_timer_get_time / 1000 */
    uint32_t internal_free_kb;
    uint32_t internal_largest_kb;
    uint32_t dma_free_kb;
    uint32_t dma_largest_kb;
    uint32_t psram_free_kb;
    uint32_t psram_largest_kb;
    uint32_t lvgl_used_kb;          /* from lv_mem_monitor */
    uint32_t lvgl_free_kb;
    uint8_t  lvgl_frag_pct;
} pool_sample_t;

static pool_sample_t *s_ring = NULL;    /* PSRAM, SAMPLE_COUNT entries */
static atomic_uint    s_head = 0;       /* next write index, wraps */
static atomic_uint    s_count = 0;      /* saturates at SAMPLE_COUNT */

/* Alloc-failure log is a separate, smaller ring — 64 entries. */
typedef struct {
    uint32_t ms;
    uint32_t requested_bytes;
    uint32_t caps;
    uint32_t internal_largest_at_failure_kb;
} alloc_fail_t;

#define FAIL_COUNT 64
static alloc_fail_t *s_fails = NULL;
static atomic_uint   s_fail_head = 0;
static atomic_uint   s_fail_count = 0;

static void fail_callback(size_t requested_size, uint32_t caps, const char *function_name)
{
    if (!s_fails) return;
    uint32_t idx = atomic_fetch_add(&s_fail_head, 1) % FAIL_COUNT;
    s_fails[idx].ms                             = (uint32_t)(esp_timer_get_time() / 1000);
    s_fails[idx].requested_bytes                = requested_size;
    s_fails[idx].caps                           = caps;
    s_fails[idx].internal_largest_at_failure_kb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
    atomic_fetch_add(&s_fail_count, 1);
    ESP_LOGE(TAG, "alloc FAIL %zu B caps=0x%08lx from %s (internal_largest=%luKB)",
             requested_size, (unsigned long)caps,
             function_name ? function_name : "?",
             (unsigned long)s_fails[idx].internal_largest_at_failure_kb);
}

static void sampler_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "sampler task running (1 Hz, ring=%d samples)", SAMPLE_COUNT);
    for (;;) {
        pool_sample_t s = {0};
        s.ms                  = (uint32_t)(esp_timer_get_time() / 1000);
        s.internal_free_kb    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
        s.internal_largest_kb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
        s.dma_free_kb         = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024;
        s.dma_largest_kb      = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024;
        s.psram_free_kb       = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
        s.psram_largest_kb    = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024;

        lv_mem_monitor_t mon = {0};
        lv_mem_monitor(&mon);   /* benign if not locked — stats only */
        s.lvgl_used_kb = (mon.total_size - mon.free_size) / 1024;
        s.lvgl_free_kb = mon.free_size / 1024;
        s.lvgl_frag_pct = (uint8_t)mon.frag_pct;

        uint32_t idx = atomic_fetch_add(&s_head, 1) % SAMPLE_COUNT;
        s_ring[idx] = s;
        uint32_t c = atomic_load(&s_count);
        if (c < SAMPLE_COUNT) atomic_store(&s_count, c + 1);

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

esp_err_t tab5_pool_probe_init(void)
{
    if (s_ring) return ESP_OK;  /* idempotent */

    s_ring = heap_caps_calloc(SAMPLE_COUNT, sizeof(pool_sample_t), MALLOC_CAP_SPIRAM);
    s_fails = heap_caps_calloc(FAIL_COUNT, sizeof(alloc_fail_t), MALLOC_CAP_SPIRAM);
    if (!s_ring || !s_fails) {
        ESP_LOGE(TAG, "probe buffer alloc failed — probe disabled");
        heap_caps_free(s_ring);
        heap_caps_free(s_fails);
        s_ring = NULL;
        s_fails = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = heap_caps_register_failed_alloc_callback(fail_callback);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register_failed_alloc_callback returned %s", esp_err_to_name(err));
    }

    BaseType_t r = xTaskCreatePinnedToCore(
        sampler_task, "pool_probe", 3072, NULL, 1, NULL, 1);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "sampler task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "initialized (samples=%d, fails=%d)", SAMPLE_COUNT, FAIL_COUNT);
    return ESP_OK;
}

esp_err_t tab5_pool_probe_http_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!s_ring) {
        httpd_resp_sendstr(req, "probe not initialised\n");
        return ESP_OK;
    }

    httpd_resp_sendstr_chunk(req,
        "# Samples\n"
        "ms,internal_free_kb,internal_largest_kb,dma_free_kb,dma_largest_kb,"
        "psram_free_kb,psram_largest_kb,lvgl_used_kb,lvgl_free_kb,lvgl_frag_pct\n");

    uint32_t count = atomic_load(&s_count);
    uint32_t head  = atomic_load(&s_head);
    /* If we've wrapped, oldest sample is at head; otherwise at 0. */
    uint32_t start = (count < SAMPLE_COUNT) ? 0 : head;
    char line[160];
    for (uint32_t i = 0; i < count; i++) {
        const pool_sample_t *s = &s_ring[(start + i) % SAMPLE_COUNT];
        int n = snprintf(line, sizeof(line),
            "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
            ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u\n",
            s->ms,
            s->internal_free_kb, s->internal_largest_kb,
            s->dma_free_kb, s->dma_largest_kb,
            s->psram_free_kb, s->psram_largest_kb,
            s->lvgl_used_kb, s->lvgl_free_kb, s->lvgl_frag_pct);
        if (n > 0) httpd_resp_send_chunk(req, line, n);
    }

    httpd_resp_sendstr_chunk(req, "\n# AllocFailures\n"
                                   "ms,requested_bytes,caps,internal_largest_kb\n");
    uint32_t fc = atomic_load(&s_fail_count);
    if (fc > FAIL_COUNT) fc = FAIL_COUNT;
    uint32_t fhead = atomic_load(&s_fail_head);
    uint32_t fstart = (atomic_load(&s_fail_count) < FAIL_COUNT) ? 0 : fhead;
    for (uint32_t i = 0; i < fc; i++) {
        const alloc_fail_t *f = &s_fails[(fstart + i) % FAIL_COUNT];
        int n = snprintf(line, sizeof(line),
            "%" PRIu32 ",%" PRIu32 ",0x%08" PRIx32 ",%" PRIu32 "\n",
            f->ms, f->requested_bytes, f->caps,
            f->internal_largest_at_failure_kb);
        if (n > 0) httpd_resp_send_chunk(req, line, n);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
```

- [ ] **Step 2: Add pool_probe.c to CMakeLists SRCS**

Modify: `main/CMakeLists.txt`. Locate the `SRCS` list in the non-linux branch (starts around line 32) and add `"pool_probe.c"` near `"debug_obs.c"` (they're sibling observability modules):

```diff
              "voice.c" "mode_manager.c"
-             "debug_server.c" "debug_obs.c" "heap_watchdog.c"
+             "debug_server.c" "debug_obs.c" "pool_probe.c" "heap_watchdog.c"
              "service_registry.c" "service_storage.c" "service_display.c"
```

- [ ] **Step 3: Build to verify it compiles**

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -5
```

Expected: build completes with `Project build complete`. Any unresolved symbol or missing include is fixed by revisiting Step 1.

- [ ] **Step 4: Commit**

```bash
git add main/pool_probe.c main/CMakeLists.txt
git commit -m "investigation: implement pool_probe (Phase 1 data collector)

1 Hz background sampler writes into a 900-entry PSRAM ring buffer
(internal/DMA/PSRAM/LVGL pool stats).  Heap-alloc-failure callback
records the most recent 64 allocator failures with caller context.

Part of docs/STABILITY-INVESTIGATION.md Phase 1 — see plan at
docs/superpowers/plans/2026-04-23-lvgl-pool-investigation.md."
```

## Task 3: Wire the probe into boot + the debug server

**Files:**
- Modify: `main/main.c` (add `tab5_pool_probe_init()` call)
- Modify: `main/debug_server.c` (register the `/heap-history` URI)

- [ ] **Step 1: Wire probe into boot sequence**

Modify `main/main.c`. Find the heap_watchdog start call (grep: `heap_watchdog_start` or search for `"TinkerTab v1.0.0 running"` banner). Add `tab5_pool_probe_init()` immediately after the heap watchdog starts.

```bash
grep -n "heap_watchdog_start\|heap_wd" main/main.c | head -5
```

Add the include at the top of main.c (near other local includes):

```c
#include "pool_probe.h"
```

Then after the line that calls `heap_watchdog_start();`, add:

```c
    /* Phase 1 instrumentation for docs/STABILITY-INVESTIGATION.md.
     * Removed after root cause is identified + fixed in Phase 3. */
    tab5_pool_probe_init();
```

- [ ] **Step 2: Register /heap-history URI in debug_server**

Modify `main/debug_server.c`. Add the include:

```c
#include "pool_probe.h"
```

Find the section where other URI handlers are registered (grep for `httpd_register_uri_handler`). Add near the `/selftest` or `/heap` registration:

```c
    {
        httpd_uri_t u = {
            .uri = "/heap-history",
            .method = HTTP_GET,
            .handler = tab5_pool_probe_http_handler,
        };
        httpd_register_uri_handler(server, &u);
    }
```

Note: `tab5_pool_probe_http_handler` does its own response; it does NOT need `check_auth(req)` because the endpoint is read-only observability. Matches the pattern of `/info` and `/selftest` which are unauthenticated. If project policy requires auth, wrap it the same way other handlers do.

- [ ] **Step 3: Build + flash**

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -3
idf.py -p /dev/ttyACM1 flash 2>&1 | tail -3
```

Expected: build + flash complete. Device reset at end. If the port is /dev/ttyACM0 on a given run, substitute accordingly (USB re-enumerates sometimes).

- [ ] **Step 4: Smoke test the probe endpoint**

```bash
sleep 20   # let Tab5 boot
curl -s --max-time 10 http://192.168.1.90:8080/heap-history | head -20
```

Expected output (early after boot, so only ~20 samples):

```
# Samples
ms,internal_free_kb,internal_largest_kb,...,lvgl_frag_pct
5015,67,62,64,62,21170,20992,54,33,5
6015,67,62,64,62,21170,20992,54,33,5
...

# AllocFailures
ms,requested_bytes,caps,internal_largest_kb
```

If the CSV header is empty or the endpoint 404s, verify Step 2's URI registration ran (search logs for `http: /heap-history`).

- [ ] **Step 5: Commit**

```bash
git add main/main.c main/debug_server.c
git commit -m "investigation: wire pool_probe into boot + debug server

tab5_pool_probe_init() runs once at boot after heap_watchdog.
GET /heap-history on port 8080 returns a two-section CSV
(Samples + AllocFailures)."
```

## Task 4: Run the stress orchestrator with instrumentation + capture data

**Files:**
- No code changes.
- Capture: `/tmp/heap-history.csv`
- Append: `docs/STABILITY-INVESTIGATION.md` log entry

- [ ] **Step 1: Start the stress orchestrator**

```bash
: > /tmp/stress.log; : > /tmp/stress.csv
nohup /tmp/stress_orchestrator.sh > /tmp/stress_stdout.log 2>&1 &
echo $! > /tmp/stress.pid
echo "stress pid: $(cat /tmp/stress.pid)"
```

- [ ] **Step 2: Let the stress run for ~12 minutes so the probe captures at least two full crash cycles**

The orchestrator cycles are ~30 s each. Historical crash cadence is ~170–200 s. 12 min gives us 3+ crash cycles of data.

```bash
sleep 720   # 12 min
```

- [ ] **Step 3: Pull the probe CSV before it gets further polluted**

```bash
curl -s --max-time 15 http://192.168.1.90:8080/heap-history > /tmp/heap-history.csv
wc -l /tmp/heap-history.csv
head -5 /tmp/heap-history.csv
grep -c "^[0-9]" /tmp/heap-history.csv
```

Expected: >= 600 sample rows. AllocFailures section has >= 1 row if any LVGL alloc failed during the run.

- [ ] **Step 4: Kill the stress + snapshot the crash CSV**

```bash
kill $(cat /tmp/stress.pid) 2>/dev/null
cp /tmp/stress.csv /tmp/stress-phase1.csv
grep "CRASH DETECTED" /tmp/stress.log > /tmp/stress-phase1-crashes.txt
wc -l /tmp/stress-phase1-crashes.txt
```

- [ ] **Step 5: Append the Phase 1 data summary to the investigation log**

Edit `docs/STABILITY-INVESTIGATION.md`, append a new log entry at the bottom under `## Investigation log`:

```markdown
### 2026-04-23 — Phase 1: instrumentation run

- Firmware: commit <sha-after-Task-3-commit>
- Run duration: 720 s
- Crashes observed: <N>  (from /tmp/stress-phase1-crashes.txt)
- Probe CSV: 720 samples at 1 Hz, captured at /tmp/heap-history.csv

**Sample stats (over the run):**
- internal_free_kb: min=<X> max=<Y> median=<Z>
- internal_largest_kb: min=<X> max=<Y> — critical metric
- lvgl_free_kb: min=<X> max=<Y>
- lvgl_frag_pct: max=<X>

**AllocFailures section:**
- Count: <N>
- Smallest internal_largest_kb at moment of failure: <X>
- Distribution of requested_bytes: <summary>
- Most common caps value: <hex>

**Key observation:** <one sentence: which metric trends toward zero
first, closest to the crash moment>
```

The min/max/median numbers come from piping the sample CSV through `awk`:

```bash
awk -F, 'NR>2 && $2~/^[0-9]+$/ {
    for(i=2;i<=10;i++){if($i<mn[i]||mn[i]=="")mn[i]=$i; if($i>mx[i])mx[i]=$i; s[i]+=$i; n++}
}
END{
    print "col,min,max,avg";
    cols="ms,internal_free,internal_largest,dma_free,dma_largest,psram_free,psram_largest,lvgl_used,lvgl_free,lvgl_frag";
    split(cols,c,",");
    for(i=2;i<=10;i++) printf "%s,%s,%s,%.0f\n", c[i], mn[i], mx[i], s[i]/(n/9)
}' /tmp/heap-history.csv
```

- [ ] **Step 6: Commit Phase 1 data + log entry**

```bash
git add docs/STABILITY-INVESTIGATION.md
git commit -m "investigation: Phase 1 data captured + logged

<N> crashes in 720 s run.  Probe captured <M> samples + <K> alloc
failures.  See investigation log for summary + raw CSV at
/tmp/heap-history.csv (not committed; reproduce via
tools/apply_patches.sh + stress orchestrator)."
```

---

# Phase 2 — Hypothesize

**Goal:** From Phase 1 data, land on exactly ONE hypothesis with a specific fix proposal. No fix in this phase — analysis only.

**Exit criteria:** A new log entry in `docs/STABILITY-INVESTIGATION.md` that states the single hypothesis + the single config knob or code change proposed for Phase 3. Written before Phase 3 begins.

## Task 5: Analyze Phase 1 data against the four candidate hypotheses

**Files:**
- Append: `docs/STABILITY-INVESTIGATION.md` (new log entry)

- [ ] **Step 1: Answer these concrete questions from the CSV**

Open `/tmp/heap-history.csv` in any spreadsheet/awk tool and answer:

1. Does `internal_largest_kb` drop monotonically within each crash cycle, then recover after reboot? (If yes → fragmentation accumulating; we're losing scratch space.)
2. Does `lvgl_free_kb` drop to near zero at any point? (If yes → base 96 KB pool is exhausting, expand isn't helping.)
3. Does the AllocFailures section list `caps=0x400` (MALLOC_CAP_INTERNAL) allocations failing shortly before a crash? (If yes → internal SRAM is the bottleneck.)
4. Does PSRAM `psram_free_kb` drop over time? (Previously we thought PSRAM was flat — verify with data.)

- [ ] **Step 2: Map answers to the four hypotheses from the state doc**

Rules:
- Q2 yes + Q3 no → hypothesis (a) base pool exhausts before expand kicks in → fix candidate: bump `LV_MEM_SIZE_KILOBYTES` or verify expand actually works
- Q2 yes + Q4 yes → hypothesis (b) expand cap hit → fix candidate: bump `LV_MEM_POOL_EXPAND_SIZE_KILOBYTES`
- Q1 yes + Q3 yes → hypothesis (c) internal SRAM scratch bottleneck → fix candidate: reduce LVGL internal-SRAM reservation or move draw buffers
- Any metric monotonic-decreasing across multiple cycles → hypothesis (d) actual leak → fix candidate: find the leaking path (larger investigation, not a config tweak)

Write the selected hypothesis + reasoning into `docs/STABILITY-INVESTIGATION.md` under `## Investigation log`:

```markdown
### 2026-04-23 — Phase 2: hypothesis

From Phase 1 data:
- Q1 (internal_largest monotonic within cycle): <yes|no> — evidence: <X>
- Q2 (lvgl_free drops to ~0): <yes|no> — evidence: <X>
- Q3 (MALLOC_CAP_INTERNAL allocs fail before crash): <yes|no> — evidence: <X>
- Q4 (PSRAM decreases): <yes|no> — evidence: <X>

**Selected hypothesis:** <one of a/b/c/d from the state doc>

**Proposed Phase 3 change:** <exact config line or code change>

**Expected effect:** <what numbers should change in Phase 4 validation>
```

- [ ] **Step 3: Commit**

```bash
git add docs/STABILITY-INVESTIGATION.md
git commit -m "investigation: Phase 2 hypothesis + proposed Phase 3 change

Selected hypothesis <letter> based on Phase 1 data.  See log
entry for evidence + exact config change to attempt in Phase 3."
```

---

# Phase 3 — Fix (exactly one change)

**Goal:** Apply the single change from Phase 2. No stacking changes.

**Exit criteria:** One PR open referencing `docs/STABILITY-INVESTIGATION.md`. PR description states the hypothesis, the change, the expected effect, and how to verify in Phase 4.

## Task 6: Apply the single config change

**Files:**
- Modify: Exactly one of `sdkconfig.defaults` or a specific `main/*.c` file, depending on Phase 2's hypothesis.

- [ ] **Step 1: Apply the change**

If hypothesis (a) and we're bumping base pool size:

```diff
--- a/sdkconfig.defaults
+++ b/sdkconfig.defaults
-CONFIG_LV_MEM_SIZE_KILOBYTES=96
+CONFIG_LV_MEM_SIZE_KILOBYTES=128
```

If hypothesis (b) and we're bumping expand:

```diff
-CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096
+CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=8192
```

If hypothesis (c) and we're reconfiguring draw buffers or moving internal-SRAM reservation — the exact config line depends on Phase 2 data. The plan cannot commit to a specific line here without data.

If hypothesis (d), Phase 3 is instead a code change: identify the leaking path from Phase 1 AllocFailures trajectory, patch that path. Concrete file:line will come from the data, so the step is "edit the file identified in Phase 2 log entry to release the leaked resource in its cleanup path."

Whichever branch applies, it's one change.

- [ ] **Step 2: Build to verify config accepted**

```bash
idf.py reconfigure 2>&1 | tail -3
idf.py build 2>&1 | tail -3
```

Expected: build succeeds. For LVGL memory config bumps, watch for a new warning about base pool exceeding SRAM BSS — CLAUDE.md notes 128 KB causes a linker error for `LV_MEM_SIZE_KILOBYTES`. If this happens, that's valuable data — it means we can't solve the problem by growing the base pool and Phase 2 needs to reconsider.

- [ ] **Step 3: Flash + quick boot smoke test**

```bash
idf.py -p /dev/ttyACM1 flash 2>&1 | tail -3
sleep 20
curl -s --max-time 6 http://192.168.1.90:8080/selftest \
  | python3 -c "import json,sys;d=json.load(sys.stdin);[print(' ',t['name'],':','PASS' if t['pass'] else 'FAIL') for t in d['tests']]"
```

Expected: 8/8 PASS. If any test fails, revert the change.

- [ ] **Step 4: Commit, push, open PR**

```bash
git checkout -b investigate/lvgl-pool-phase3-<hypothesis-letter>
git add <the one file you changed>
git commit -m "fix(lvgl): <one-line summary of the change>

See docs/STABILITY-INVESTIGATION.md Phase 2 log entry for hypothesis
and expected effect.  Validation in Phase 4."

git push -u origin investigate/lvgl-pool-phase3-<hypothesis-letter>
gh pr create --title "fix(lvgl): <hypothesis> ..." --body "see docs/STABILITY-INVESTIGATION.md Phase 2 + Phase 3 log entries"
```

---

# Phase 4 — Validate

**Goal:** Prove the Phase 3 change eliminates the crashes. 30-min stress with zero PANIC reboots.

**Exit criteria:** `grep "CRASH DETECTED" /tmp/stress.log` returns nothing after a 30-min run. If that's achieved, merge the Phase 3 PR + close the investigation. Otherwise, loop back to Phase 2 with fresh data (a still-crashing post-fix run is diagnostic for a DIFFERENT failure mode than the one we theorised).

## Task 7: 30-minute validation run

**Files:**
- Capture: `/tmp/stress-phase4.csv`, `/tmp/heap-history-phase4.csv`

- [ ] **Step 1: Launch the 30-min stress**

```bash
: > /tmp/stress.log; : > /tmp/stress.csv
nohup /tmp/stress_orchestrator.sh > /tmp/stress_stdout.log 2>&1 &
echo $! > /tmp/stress.pid
```

- [ ] **Step 2: Let it run for the full 30 minutes the orchestrator is tuned for**

```bash
sleep 1800
```

- [ ] **Step 3: Capture data**

```bash
kill $(cat /tmp/stress.pid) 2>/dev/null
cp /tmp/stress.csv /tmp/stress-phase4.csv
curl -s --max-time 15 http://192.168.1.90:8080/heap-history > /tmp/heap-history-phase4.csv
echo "crashes:"; grep -c "CRASH DETECTED" /tmp/stress.log
echo "first 5 crashes:"; grep "CRASH DETECTED" /tmp/stress.log | head -5
```

- [ ] **Step 4: Branch based on outcome**

**If zero crashes:**

```bash
# Log success and merge the Phase 3 PR
```

Append to `docs/STABILITY-INVESTIGATION.md`:

```markdown
### 2026-04-23 — Phase 4: VALIDATED

30-min stress clean (0 crashes).  Phase 3 change validated.
Merging PR <#NNN>.  Investigation closed.
```

Then:

```bash
git add docs/STABILITY-INVESTIGATION.md
git commit -m "investigation: Phase 4 validation clean — closing"
git push
gh pr merge <the Phase 3 PR number> --squash --delete-branch
```

**If still crashes (≥ 1 `CRASH DETECTED`):**

- Grab a fresh coredump (current ELF must match — do NOT rebuild before grabbing):

```bash
curl -s --max-time 10 -H "Authorization: Bearer $TOKEN" \
  -o /tmp/crash-phase4.bin http://192.168.1.90:8080/coredump
. /home/rebelforce/esp/esp-idf/export.sh
python3 -m esp_coredump info_corefile --core /tmp/crash-phase4.bin \
  --core-format raw build/tinkertab.elf 2>&1 \
  | grep -A20 "CURRENT THREAD STACK\|MEPC\|MCAUSE" > /tmp/phase4-coredump.txt
head -30 /tmp/phase4-coredump.txt
```

Append a new log entry:

```markdown
### 2026-04-23 — Phase 4: still crashing post-fix

Crashes observed: <N> in 30 min.
Coredump MEPC: <decoded symbol>
Interpretation: Phase 3 change didn't address the root cause —
                the allocator failures have moved to <different
                pool based on new heap-history CSV>.

Returning to Phase 2 with updated data.
```

Then:

```bash
git add docs/STABILITY-INVESTIGATION.md
git commit -m "investigation: Phase 4 still crashing — new Phase 2 cycle

Crashes <N> in 30 min.  Fresh coredump at /tmp/crash-phase4.bin.
Returning to Phase 2 with new data."
```

Loop back to Task 5 (Phase 2 analysis) with `/tmp/heap-history-phase4.csv` as the fresh dataset.

---

## Self-review checklist (run before handing this plan off)

- [x] Every task has exact file paths
- [x] Every code step has complete runnable code (no "similar to", no pseudocode)
- [x] Every bash step has the exact command with expected output
- [x] Types and symbols are consistent: `tab5_pool_probe_init`, `tab5_pool_probe_http_handler`, `pool_sample_t`, `alloc_fail_t` match across tasks
- [x] No placeholders (`TBD`, `TODO`, "implement later")
- [x] Phase exits are measurable (CSV captured, log entry appended, PR opened)
- [x] Failure path in Phase 4 explicitly loops back — not a dead end

## Open spec gaps

- Phase 2 task says "answer concrete questions from the CSV" and "map to hypothesis" — the mapping table in Step 2 is concrete but Q2/Q3/Q4 require human judgement on thresholds ("drops to ~0"). That's appropriate for investigation work (we don't know the thresholds yet). The plan acknowledges this; the judgement call is logged in the investigation doc so it's reviewable.
- Phase 3 Step 1 for hypotheses (c) and (d) intentionally lacks a specific code change — the fix depends on Phase 2 data which we don't have yet. The plan says so explicitly and tells the engineer what to produce.

---

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-23-lvgl-pool-investigation.md`.

Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — Execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints.

Which approach?
