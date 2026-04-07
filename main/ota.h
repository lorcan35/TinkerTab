/**
 * TinkerTab — OTA Firmware Updates
 *
 * Checks Dragon server for new firmware, downloads to inactive OTA slot,
 * reboots. Auto-rollback if new firmware fails to validate.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/** OTA update info returned by check */
typedef struct {
    bool available;
    char version[32];
    char url[256];
    char sha256[65];
} tab5_ota_info_t;

/** Check Dragon for available firmware update.
 *  Returns ESP_OK if check succeeded (even if no update available). */
esp_err_t tab5_ota_check(tab5_ota_info_t *info);

/** Download and apply firmware update from URL.
 *  Reboots on success. Returns error if download/verify fails. */
esp_err_t tab5_ota_apply(const char *url);

/** Mark current firmware as valid (call after successful boot).
 *  Prevents auto-rollback on next reboot. */
esp_err_t tab5_ota_mark_valid(void);

/** Get current running partition name ("ota_0" or "ota_1"). */
const char *tab5_ota_current_partition(void);
