/**
 * TinkerClaw Tab5 — SD Card Driver
 *
 * SDMMC 4-bit interface for the M5Stack Tab5 (ESP32-P4) micro-SD slot.
 * Mounts a FAT filesystem at /sdcard.
 *
 * WARNING: The Tab5 uses the same SDMMC peripheral for both the SD card
 * and the ESP32-C6 WiFi co-processor (ESP-Hosted over SDIO). The two
 * share the SDMMC controller and cannot be active simultaneously.
 * See sdcard.c for details.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Mount the SD card FAT filesystem at /sdcard.
 *
 * Configures the SDMMC host in 4-bit mode on the Tab5 SD GPIOs
 * (CLK=43, CMD=44, D0-D3=39-42) and mounts the first FAT partition.
 *
 * @return ESP_OK on success, or an error code.
 *         Returns ESP_OK if the card is already mounted.
 */
esp_err_t tab5_sdcard_init(void);

/**
 * Unmount the SD card and release the SDMMC bus.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not mounted.
 */
esp_err_t tab5_sdcard_deinit(void);

/**
 * Check whether the SD card is currently mounted.
 */
bool tab5_sdcard_mounted(void);

/**
 * Return the total capacity of the mounted SD card in bytes.
 *
 * @return Total bytes, or 0 if not mounted.
 */
uint64_t tab5_sdcard_total_bytes(void);

/**
 * Return the free space on the mounted SD card in bytes.
 *
 * @return Free bytes, or 0 if not mounted.
 */
uint64_t tab5_sdcard_free_bytes(void);

/**
 * Return the VFS mount point string ("/sdcard").
 */
const char *tab5_sdcard_mount_point(void);
