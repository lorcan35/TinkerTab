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

static void heap_watchdog_task(void *arg)
{
    (void)arg;
    int frag_count = 0;

    ESP_LOGI(TAG, "Heap watchdog started (check every %ds, reboot after %d consecutive fragmentation events)",
             HEAP_WD_CHECK_INTERVAL_MS / 1000, HEAP_WD_FRAG_REBOOT_COUNT);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEAP_WD_CHECK_INTERVAL_MS));

        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        ESP_LOGI(TAG, "PSRAM: largest_block=%uKB free=%uKB | Internal: free=%uKB",
                 (unsigned)(psram_largest / 1024),
                 (unsigned)(psram_free / 1024),
                 (unsigned)(internal_free / 1024));

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
        2048,           /* Stack: 2KB — just heap_caps calls + logging */
        NULL,
        1,              /* Priority 1 — lowest, background monitoring */
        NULL,
        1               /* Core 1 — same as httpd, away from LVGL on Core 0 */
    );
}
