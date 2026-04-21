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

/** OTA progress callback — called from the OTA task during download.
 *  percent: 0-100 download progress. phase: "download" or "verify". */
typedef void (*tab5_ota_progress_cb_t)(int percent, const char *phase);

/** Set progress callback (NULL to disable). Thread-safe — callback may be
 *  invoked from any core. Caller must use lv_async_call for LVGL updates. */
void tab5_ota_set_progress_cb(tab5_ota_progress_cb_t cb);

/** Check Dragon for available firmware update.
 *  Returns ESP_OK if check succeeded (even if no update available). */
esp_err_t tab5_ota_check(tab5_ota_info_t *info);

/** Download and apply firmware update from URL.
 *  If expected_sha256 is non-NULL and non-empty, the downloaded image is
 *  verified against it (SHA256 hex digest). Mismatch aborts the OTA.
 *  If expected_sha256 is NULL or empty, a warning is logged but OTA proceeds
 *  (backward compatibility).
 *  Reboots on success. Returns error if download/verify fails. */
esp_err_t tab5_ota_apply(const char *url, const char *expected_sha256);

/** Mark current firmware as valid (call after successful boot).
 *  Prevents auto-rollback on next reboot. */
esp_err_t tab5_ota_mark_valid(void);

/** Get current running partition name ("ota_0" or "ota_1"). */
const char *tab5_ota_current_partition(void);

/** Wave 10 #77 extended-use fix: stage the OTA for the NEXT boot instead of
 *  running it in the current session. Stores url + sha256 in NVS, sets a
 *  pending flag, then reboots immediately. The newly-booted firmware
 *  invokes tab5_ota_apply_pending_if_any() BEFORE LVGL/voice/camera init,
 *  so DMA-capable internal SRAM is pristine when esp_https_ota runs.
 *
 *  Solves the "OTA can't allocate 181 bytes because heap is exhausted
 *  from an hour of normal use" failure mode the reactive watchdog had
 *  to cover up.  Returns ESP_OK on successful stage (never returns to
 *  caller — reboots). */
esp_err_t tab5_ota_schedule(const char *url, const char *expected_sha256);

/** Boot-time companion to tab5_ota_schedule(). Checks NVS for a pending
 *  OTA, clears the flag, then invokes tab5_ota_apply(). Only safe to call
 *  AFTER WiFi + Dragon are reachable but BEFORE LVGL / voice / camera
 *  init has burned through the DMA reserve. Call from main.c. Returns
 *  ESP_ERR_NOT_FOUND if nothing is pending (non-fatal). */
esp_err_t tab5_ota_apply_pending_if_any(void);
