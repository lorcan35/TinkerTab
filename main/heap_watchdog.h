/**
 * TinkerTab — PSRAM Heap Fragmentation Watchdog
 *
 * Background FreeRTOS task that monitors PSRAM heap health every 60 seconds.
 * If severe fragmentation is detected for 3 consecutive checks (largest free
 * block < 32KB while total free PSRAM > 1MB), triggers a clean reboot.
 *
 * This prevents the device from silently degrading after 24-72 hours of
 * continuous operation due to PSRAM fragmentation.
 */
#pragma once

#include "esp_err.h"

/**
 * Start the heap watchdog task.
 *
 * Creates a FreeRTOS task pinned to Core 1 (priority 1) that checks
 * heap health every 60 seconds. Safe to call once during boot.
 */
void heap_watchdog_start(void);
