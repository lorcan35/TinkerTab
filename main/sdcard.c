/**
 * TinkerClaw Tab5 — SD Card Driver
 *
 * 4-bit SDMMC interface for the M5Stack Tab5 micro-SD slot.
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │  KNOWN ISSUE — SD / WiFi CONFLICT                               │
 * │                                                                  │
 * │  The Tab5 routes both the micro-SD card and the ESP32-C6 WiFi   │
 * │  co-processor through SDMMC / SDIO on the ESP32-P4. Both need   │
 * │  the SDMMC peripheral, and they use different GPIO banks:        │
 * │                                                                  │
 * │      SD  card : CLK=43  CMD=44  D0-D3 = 39-42                  │
 * │      WiFi SDIO: CLK=12  CMD=13  D0-D3 = 11-8                   │
 * │                                                                  │
 * │  While the GPIOs differ, the ESP-IDF SDMMC host driver only     │
 * │  supports a single slot configuration at a time. If WiFi is     │
 * │  active (ESP-Hosted over SDIO), you must deinitialise the SDIO  │
 * │  link before mounting the SD card, and vice versa.               │
 * │                                                                  │
 * │  Possible strategies:                                            │
 * │    1. Time-multiplex: tear down WiFi, mount SD, do I/O, unmount │
 * │       SD, bring WiFi back.                                       │
 * │    2. Boot-time choice: decide at startup which one is needed.   │
 * │    3. Use SPI mode for the SD card (slower, but avoids the      │
 * │       SDMMC peripheral conflict).                                │
 * └──────────────────────────────────────────────────────────────────┘
 */

#include "sdcard.h"
#include "config.h"

#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "tab5_sd";

#define MOUNT_POINT "/sdcard"

/* State -------------------------------------------------------------------*/
static bool            s_mounted = false;
static sdmmc_card_t  *s_card    = NULL;

/* -------------------------------------------------------------------------*/

esp_err_t tab5_sdcard_init(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD card already mounted at %s", MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting SD card (SDMMC 4-bit) at %s", MOUNT_POINT);

    /* --- VFS FAT mount configuration --- */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    /* --- SDMMC host — ESP32-P4 Tab5 uses SLOT 0 (IOMUX pins 39-44) --- */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

    /* --- SD card IO power via on-chip LDO (channel 4, 3.3V) --- */
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t pwr_ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (pwr_ret == ESP_OK) {
        host.pwr_ctrl_handle = pwr_ctrl_handle;
        ESP_LOGI(TAG, "SD card LDO power enabled (channel 4)");
    } else {
        ESP_LOGW(TAG, "SD LDO power init failed: %s (continuing anyway)", esp_err_to_name(pwr_ret));
    }

    /* --- Slot configuration with Tab5 GPIO mapping --- */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.clk = TAB5_SD_CLK;   // GPIO 43
    slot_config.cmd = TAB5_SD_CMD;   // GPIO 44
    slot_config.d0  = TAB5_SD_D0;    // GPIO 39
    slot_config.d1  = TAB5_SD_D1;    // GPIO 40
    slot_config.d2  = TAB5_SD_D2;    // GPIO 41
    slot_config.d3  = TAB5_SD_D3;    // GPIO 42
    slot_config.width = 4;           // 4-bit bus

    /* Internal pull-ups are sufficient for short PCB traces on the Tab5 */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* --- Mount --- */
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        MOUNT_POINT, &host, &slot_config, &mount_config, &s_card
    );

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount FAT filesystem. "
                     "If the card is freshly formatted, ensure it is FAT32.");
        } else {
            ESP_LOGE(TAG, "Failed to initialise SD card (%s). "
                     "Check that a card is inserted and WiFi SDIO is not active.",
                     esp_err_to_name(ret));
        }
        s_card = NULL;
        return ret;
    }

    s_mounted = true;

    /* --- Print card info --- */
    ESP_LOGI(TAG, "SD card mounted successfully");
    sdmmc_card_print_info(stdout, s_card);

    const char *type_str = (s_card->ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC";
    uint64_t capacity = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;

    ESP_LOGI(TAG, "  Name     : %s", s_card->cid.name);
    ESP_LOGI(TAG, "  Type     : %s", type_str);
    ESP_LOGI(TAG, "  Capacity : %llu MB", capacity / (1024 * 1024));
    ESP_LOGI(TAG, "  Speed    : %s",
             (s_card->csd.tr_speed > 25000000) ? "High Speed" : "Default Speed");

    return ESP_OK;
}

esp_err_t tab5_sdcard_deinit(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    s_card    = NULL;
    s_mounted = false;

    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

bool tab5_sdcard_mounted(void)
{
    return s_mounted;
}

uint64_t tab5_sdcard_total_bytes(void)
{
    if (!s_mounted || !s_card) {
        return 0;
    }
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

uint64_t tab5_sdcard_free_bytes(void)
{
    if (!s_mounted) {
        return 0;
    }

    FATFS *fs;
    DWORD free_clusters;

    FRESULT fres = f_getfree("0:", &free_clusters, &fs);
    if (fres != FR_OK) {
        ESP_LOGE(TAG, "f_getfree() failed (%d)", fres);
        return 0;
    }

    /* Each cluster is fs->csize sectors; SD sector size is always 512 bytes */
    uint64_t free_bytes = (uint64_t)free_clusters * fs->csize * 512;

    return free_bytes;
}

const char *tab5_sdcard_mount_point(void)
{
    return MOUNT_POINT;
}
