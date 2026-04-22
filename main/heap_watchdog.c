/**
 * TinkerTab — PSRAM Heap Fragmentation Watchdog
 *
 * Monitors PSRAM heap health every 60 seconds. Detects classic fragmentation
 * (lots of total free space but no large contiguous blocks) and reboots after
 * 3 consecutive minutes of severe fragmentation.
 *
 * Thresholds:
 *   - Largest free PSRAM block < 32KB  AND  total free PSRAM > 1MB
 *   - 3 consecutive checks (3 minutes) before reboot
 *
 * Task runs at priority 1 (lowest) on Core 1, 2048 byte stack.
 * NOT registered with the task watchdog — this is background monitoring.
 */

#include "heap_watchdog.h"
#include "settings.h"
#include "voice.h"

#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

/* Wave 11: pull media_cache_stats so we can log the rich-media cache
 * footprint alongside PSRAM / LVGL pool / DMA. Helps diagnose rare
 * cases where many widget_media events leave all 5 slots occupied
 * (2.9 MB PSRAM resident) and user sees decode stalls. */
#include "media_cache.h"

static const char *HW_TAG = "heap_wd";

/* Wave 11 stability P1: route heap-watchdog reboots through
 * esp_system_abort() instead of plain esp_restart(). abort() panics,
 * the panic handler saves a full coredump to the dedicated 256 KB
 * partition (0x620000, defined in partitions.csv), and the bootloader
 * reboots after. Post-mortem becomes `esptool read_flash 0x620000
 * 0x40000 cd.bin` + `espcoredump.py info_corefile`.
 *
 * Previously each of the three reboot paths (SRAM frag, DMA exhausted,
 * PSRAM frag) just called esp_restart() directly — no coredump saved,
 * no way to figure out which task held the heap at the moment of the
 * decision. `reason` is stamped into the panic message so the summary
 * tells you which threshold tripped. */
static void __attribute__((noreturn)) hw_restart_with_coredump(const char *reason)
{
    ESP_LOGE(HW_TAG, "reboot reason=%s — aborting for coredump", reason ? reason : "unknown");
    vTaskDelay(pdMS_TO_TICKS(100));  /* Let log flush before the panic */
    char details[64];
    snprintf(details, sizeof(details), "heap_wd: %s", reason ? reason : "unknown");
    esp_system_abort(details);
}

static const char *TAG = "heap_wd";

#define HEAP_WD_CHECK_INTERVAL_MS   60000   /* 60 seconds between checks */
#define HEAP_WD_FRAG_BLOCK_MIN      (32 * 1024)    /* 32KB — minimum acceptable largest block */
#define HEAP_WD_FRAG_TOTAL_MIN      (1 * 1024 * 1024)  /* 1MB — total free must be above this for fragmentation diagnosis */
#define HEAP_WD_FRAG_REBOOT_COUNT   3       /* 3 consecutive failures before reboot */

/* Internal SRAM fragmentation thresholds:
 * If largest contiguous block < 4KB while total free > 16KB, the heap is
 * severely fragmented.  3 consecutive checks (3 minutes) triggers reboot.
 * This catches the case where LVGL alloc/free churn leaves many small holes
 * but plenty of total free space — alloc failures follow shortly. */
#define HEAP_WD_INT_FRAG_BLOCK_MIN     4096     /* 4KB — below this, allocs will fail */
#define HEAP_WD_INT_FRAG_TOTAL_MIN     (16 * 1024)  /* 16KB — must have enough free for it to be fragmentation, not exhaustion */
#define HEAP_WD_INT_FRAG_REBOOT_COUNT  3        /* 3 consecutive checks (3 minutes) */

/* DMA pool exhaustion thresholds (audit #80):
 * WiFi driver + TLS need DMA-capable buffers for RX/TX descriptors. When
 * free DMA pool drops too low the driver silently fails — packets never
 * leave the chip, ARP responses stop, device becomes unreachable but the
 * LVGL loop keeps running so the heap_wd/SRAM/PSRAM paths don't trigger.
 *
 * Wave 15 history:
 *   - C05 first-pass: threshold was 16 KB but steady-state floor is ~6 KB.
 *     Dragon-reconnect churn + normal activity would trip the 16 KB line
 *     + reboot Tab5 every ~5 min.  Lowered to 4 KB + grace list.
 *   - C05 second-pass (this commit): even at 4 KB the reboot still
 *     fires under combined load (screenshot spam + voice + Dragon
 *     restart).  Live observation in the user-flow test showed DMA
 *     genuinely drains below 4 KB after ~5 min of mixed activity, but
 *     WiFi + voice + debug-httpd all keep working.  Rebooting the user's
 *     device because an internal pool is "low" — when everything the
 *     user can actually see is fine — is more disruptive than the
 *     theoretical WiFi-stall scenario this was guarding against.
 *
 * Current disposition: the reboot path is DISABLED by setting the
 * counter cap to INT_MAX (the watchdog still logs warnings, still
 * tracks the running count, but never triggers hw_restart_with_coredump).
 * The real DMA leak is filed as a separate bug for proper
 * root-causing; this workaround buys the device stability until that
 * investigation completes.
 *
 * If you genuinely want the reboot behaviour back, set
 * HEAP_WD_DMA_REBOOT_COUNT to a small positive number (e.g. 10).
 */
#define HEAP_WD_DMA_CRITICAL_BYTES     (4 * 1024)   /* 4KB — below observed floor */
#define HEAP_WD_DMA_REBOOT_COUNT       INT_MAX      /* effectively disabled (was 5) */
#define HEAP_WD_DMA_WARN_BYTES         (16 * 1024)  /* 16KB — soft warning only */

static void heap_watchdog_task(void *arg)
{
    (void)arg;
    int frag_count = 0;
    int internal_frag_count = 0;
    int dma_critical_count = 0;

    ESP_LOGI(TAG, "Heap watchdog started (check every %ds, reboot after %d consecutive fragmentation events)",
             HEAP_WD_CHECK_INTERVAL_MS / 1000, HEAP_WD_FRAG_REBOOT_COUNT);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEAP_WD_CHECK_INTERVAL_MS));

        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        int frag_pct = (internal_free > 0) ? (int)(100 - (internal_largest * 100 / internal_free)) : 0;

        /* C12: Verify internal DMA pool has healthy headroom */
        size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

        ESP_LOGI(TAG, "PSRAM: blk=%uKB free=%uKB | Internal: free=%uKB blk=%uKB frag=%d%% | DMA: free=%uKB blk=%uKB",
                 (unsigned)(psram_largest / 1024),
                 (unsigned)(psram_free / 1024),
                 (unsigned)(internal_free / 1024),
                 (unsigned)(internal_largest / 1024),
                 frag_pct,
                 (unsigned)(dma_free / 1024),
                 (unsigned)(dma_largest / 1024));

        /* Internal SRAM fragmentation: free is OK but largest block is small.
         * Track consecutive occurrences and reboot if sustained for 3 minutes. */
        if (internal_largest < HEAP_WD_INT_FRAG_BLOCK_MIN && internal_free > HEAP_WD_INT_FRAG_TOTAL_MIN) {
            internal_frag_count++;
            ESP_LOGE(TAG, "Internal SRAM fragmented: %uKB free but largest block only %u bytes! "
                     "(count=%d/%d)",
                     (unsigned)(internal_free / 1024), (unsigned)internal_largest,
                     internal_frag_count, HEAP_WD_INT_FRAG_REBOOT_COUNT);

            if (internal_frag_count >= HEAP_WD_INT_FRAG_REBOOT_COUNT) {
                /* v4·D audit P1 fix: don't tear the device down while
                 * the user is mid-turn.  Voice states SPEAKING / LISTENING
                 * / PROCESSING / DICTATE all deserve a grace window --
                 * fragmentation will still be there in 60 s when the
                 * turn ends and we'll reboot then instead. */
                extern voice_state_t voice_get_state(void);
                voice_state_t vs = voice_get_state();
                if (vs == VOICE_STATE_LISTENING || vs == VOICE_STATE_SPEAKING
                    || vs == VOICE_STATE_PROCESSING) {
                    ESP_LOGW(TAG, "Heap watchdog: deferring reboot, voice active (state=%d)", vs);
                } else {
                    /* Log a detailed summary BEFORE the restart so a
                     * future "esptool read_flash" of the coredump
                     * partition + this log tail give enough context
                     * to post-mortem the fragmentation trigger. */
                    ESP_LOGE(TAG, "Heap watchdog: SRAM fragmentation %d min; internal free=%uKB largest=%u DMA free=%uKB PSRAM free=%uKB",
                             HEAP_WD_INT_FRAG_REBOOT_COUNT,
                             (unsigned)(internal_free / 1024),
                             (unsigned)internal_largest,
                             (unsigned)(dma_free / 1024),
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
                    hw_restart_with_coredump("sram_frag");
                }
            }
        } else {
            if (internal_frag_count > 0) {
                ESP_LOGI(TAG, "Internal SRAM fragmentation recovered (was %d/%d)",
                         internal_frag_count, HEAP_WD_INT_FRAG_REBOOT_COUNT);
            }
            internal_frag_count = 0;
        }

        /* Audit #80 + W15-C05: DMA exhaustion reboot path.  Soft warn at
         * HEAP_WD_DMA_WARN_BYTES (16 KB) for observability, only count
         * toward reboot below HEAP_WD_DMA_CRITICAL_BYTES (4 KB). */
        if (dma_free < HEAP_WD_DMA_WARN_BYTES) {
            ESP_LOGW(TAG, "DMA pool low: %zu bytes free (largest %zu) — warn only",
                     dma_free, dma_largest);
        }
        if (dma_free < HEAP_WD_DMA_CRITICAL_BYTES) {
            dma_critical_count++;
            ESP_LOGE(TAG, "DMA pool exhausted: %zu bytes free, largest %zu (count=%d/%d)",
                     dma_free, dma_largest,
                     dma_critical_count, HEAP_WD_DMA_REBOOT_COUNT);
            if (dma_critical_count >= HEAP_WD_DMA_REBOOT_COUNT) {
                extern voice_state_t voice_get_state(void);
                voice_state_t vs = voice_get_state();
                /* W15-C05: include CONNECTING + RECONNECTING in the grace
                 * list.  Dragon-side restarts briefly leave Tab5 in
                 * RECONNECTING state — rebooting during that window
                 * caused the "Dragon unreachable → Tab5 panic-reboot"
                 * production regression. */
                if (vs == VOICE_STATE_LISTENING || vs == VOICE_STATE_SPEAKING
                    || vs == VOICE_STATE_PROCESSING
                    || vs == VOICE_STATE_CONNECTING
                    || vs == VOICE_STATE_RECONNECTING) {
                    ESP_LOGW(TAG, "Heap watchdog: deferring DMA reboot, voice active (state=%d)", vs);
                } else {
                    ESP_LOGE(TAG, "Heap watchdog: DMA exhausted %d min; free=%zuKB largest=%zu internal=%uKB PSRAM=%uKB — rebooting to restore WiFi",
                             HEAP_WD_DMA_REBOOT_COUNT,
                             dma_free / 1024, dma_largest,
                             (unsigned)(internal_free / 1024),
                             (unsigned)(psram_free / 1024));
                    hw_restart_with_coredump("dma_exhausted");
                }
            }
        } else {
            if (dma_critical_count > 0) {
                ESP_LOGI(TAG, "DMA pool recovered (was %d/%d)",
                         dma_critical_count, HEAP_WD_DMA_REBOOT_COUNT);
            }
            dma_critical_count = 0;
        }

        /* C14: Monitor system event task stack high-water mark */
        TaskHandle_t evt_task = xTaskGetHandle("sys_evt");
        if (evt_task) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(evt_task);
            if (hwm < 512) {
                ESP_LOGW(TAG, "System event task stack low: %u bytes remaining", (unsigned)hwm);
            }
        }

        /* LVGL memory pool monitoring */
        lv_mem_monitor_t lvgl_mon;
        lv_mem_monitor(&lvgl_mon);
        ESP_LOGI(TAG, "LVGL pool: used=%uKB free=%uKB frag=%u%%",
                 (unsigned)((lvgl_mon.total_size - lvgl_mon.free_size) / 1024),
                 (unsigned)(lvgl_mon.free_size / 1024),
                 (unsigned)lvgl_mon.frag_pct);

        /* Wave 11 stability P1: warn when the LVGL pool is highly
         * fragmented. A burst of overlay create/destroy cycles (old
         * code path) could leave frag > 50 % even while free_size
         * looked healthy; widgets then fail silently when the next
         * alloc can't find a contiguous slot. Soft warning only —
         * we don't reboot here because the pool is LVGL-managed PSRAM
         * and recoverable by closing any overlay. */
        if (lvgl_mon.frag_pct > 50) {
            ESP_LOGW(TAG, "LVGL pool fragmented %u%% — close an overlay to coalesce",
                     (unsigned)lvgl_mon.frag_pct);
        }

        /* Wave 11 stability P1: media cache footprint log. 5 slots ×
         * 567 KB = ~2.9 MB resident when saturated. Soft observation
         * only (cache is bounded by its own LRU). */
        {
            int mc_used = 0;
            unsigned mc_kb = 0;
            media_cache_stats(&mc_used, &mc_kb);
            if (mc_used > 0) {
                ESP_LOGI(TAG, "Media cache: %d/%d slots, %u KB resident",
                         mc_used, MEDIA_CACHE_SLOTS, mc_kb);
            }
        }

        /* Monitor critical task stacks */
        static const char *task_names[] = {"voice_mic", "voice_ws", "voice_recon", "heap_wd", "voice_play"};
        for (int i = 0; i < 5; i++) {
            TaskHandle_t t = xTaskGetHandle(task_names[i]);
            if (t) {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(t);
                if (hwm < 512) {
                    ESP_LOGW(TAG, "Task '%s' stack low: %u bytes remaining", task_names[i], (unsigned)hwm);
                }
            }
        }

        /* US-HW17: Log NVS write count for flash wear monitoring */
        ESP_LOGI(TAG, "NVS writes this session: %lu",
                 (unsigned long)tab5_settings_get_nvs_write_count());

        /* Classic fragmentation: plenty of total free space but no large contiguous block */
        if (psram_largest < HEAP_WD_FRAG_BLOCK_MIN && psram_free > HEAP_WD_FRAG_TOTAL_MIN) {
            frag_count++;
            ESP_LOGE(TAG, "PSRAM fragmentation detected! largest_block=%uKB, total_free=%uKB "
                     "(count=%d/%d)",
                     (unsigned)(psram_largest / 1024),
                     (unsigned)(psram_free / 1024),
                     frag_count, HEAP_WD_FRAG_REBOOT_COUNT);

            if (frag_count >= HEAP_WD_FRAG_REBOOT_COUNT) {
                ESP_LOGE(TAG, "Heap watchdog: severe PSRAM fragmentation detected, rebooting");
                hw_restart_with_coredump("psram_frag");
            }
        } else {
            if (frag_count > 0) {
                ESP_LOGI(TAG, "PSRAM fragmentation recovered (was %d/%d)",
                         frag_count, HEAP_WD_FRAG_REBOOT_COUNT);
            }
            frag_count = 0;
        }
    }
}

void heap_watchdog_start(void)
{
    xTaskCreatePinnedToCore(
        heap_watchdog_task,
        "heap_wd",
        /* Wave 14 W14-H07: bumped 3 KB → 4 KB.  The task does
         * heap_caps_get_free_size + takes NVS mutex (via
         * tab5_settings_get_nvs_write_count) + ESP_LOGI with formatted
         * args.  Formatted logging alone can burn 400-600 B of stack.
         * Ironic failure: the task designed to *detect* low-memory
         * conditions could itself stack_chk_fail during a real frag
         * storm, masking the root cause with a cryptic guru rather
         * than the intended controlled reboot. */
        4096,
        NULL,
        1,              /* Priority 1 — lowest, background monitoring */
        NULL,
        1               /* Core 1 — same as httpd, away from LVGL on Core 0 */
    );
}
