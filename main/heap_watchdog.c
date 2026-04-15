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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

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

static void heap_watchdog_task(void *arg)
{
    (void)arg;
    int frag_count = 0;
    int internal_frag_count = 0;

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
                ESP_LOGE(TAG, "Heap watchdog: severe internal SRAM fragmentation for %d minutes, rebooting",
                         HEAP_WD_INT_FRAG_REBOOT_COUNT);
                vTaskDelay(pdMS_TO_TICKS(100));  /* Let log flush */
                esp_restart();
            }
        } else {
            if (internal_frag_count > 0) {
                ESP_LOGI(TAG, "Internal SRAM fragmentation recovered (was %d/%d)",
                         internal_frag_count, HEAP_WD_INT_FRAG_REBOOT_COUNT);
            }
            internal_frag_count = 0;
        }

        if (dma_free < 16384) {
            ESP_LOGE(TAG, "DMA pool critically low: %zu bytes free (largest block %zu)", dma_free, dma_largest);
        }

        /* C14: Monitor system event task stack high-water mark */
        TaskHandle_t evt_task = xTaskGetHandle("sys_evt");
        if (evt_task) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(evt_task);
            if (hwm < 512) {
                ESP_LOGW(TAG, "System event task stack low: %u bytes remaining", (unsigned)hwm);
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
                vTaskDelay(pdMS_TO_TICKS(100));  /* Let log flush */
                esp_restart();
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
        2560,           /* Stack: 2.5KB — heap_caps calls + task HWM query + logging */
        NULL,
        1,              /* Priority 1 — lowest, background monitoring */
        NULL,
        1               /* Core 1 — same as httpd, away from LVGL on Core 0 */
    );
}
