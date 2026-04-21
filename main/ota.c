/**
 * TinkerTab — OTA Firmware Updates
 *
 * Uses ESP-IDF's esp_https_ota for download + verify + partition write.
 * Dragon serves firmware via /api/ota/check and /api/ota/firmware.bin.
 * Auto-rollback: if new firmware crashes before tab5_ota_mark_valid(),
 * bootloader reverts to previous slot on next boot.
 */

#include "ota.h"
#include "config.h"
#include "settings.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ota";

/* Progress callback (set by UI layer, called from OTA task) */
static tab5_ota_progress_cb_t s_progress_cb = NULL;

void tab5_ota_set_progress_cb(tab5_ota_progress_cb_t cb)
{
    s_progress_cb = cb;
}

static void report_progress(int pct, const char *phase)
{
    if (s_progress_cb) s_progress_cb(pct, phase);
}

esp_err_t tab5_ota_check(tab5_ota_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));

    char dhost[64];
    tab5_settings_get_dragon_host(dhost, sizeof(dhost));
    if (!dhost[0]) {
        ESP_LOGW(TAG, "No Dragon host configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build URL: http://{dragon}:3502/api/ota/check?current=0.6.0 */
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s?current=%s",
             dhost, TAB5_OTA_PORT, TAB5_OTA_CHECK_PATH, TAB5_FIRMWARE_VER);

    ESP_LOGI(TAG, "Checking for updates: %s", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int content_len = esp_http_client_get_content_length(client);

    if (err != ESP_OK || status != 200 || content_len <= 0 || content_len > 1024) {
        ESP_LOGW(TAG, "OTA check failed: err=%s status=%d len=%d",
                 esp_err_to_name(err), status, content_len);
        esp_http_client_cleanup(client);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    /* Read response body */
    char *body = malloc(content_len + 1);
    if (!body) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    /* Re-open and read — esp_http_client_perform already consumed the body.
     * Use esp_http_client_read for streaming, but since perform() buffered
     * the response, we need to re-fetch. Simpler: use fetch approach. */
    esp_http_client_cleanup(client);

    /* Re-fetch with read method */
    client = esp_http_client_init(&cfg);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(body);
        return err;
    }
    content_len = esp_http_client_fetch_headers(client);
    if (content_len <= 0 || content_len > 1024) {
        esp_http_client_cleanup(client);
        free(body);
        return ESP_FAIL;
    }
    int read_len = esp_http_client_read(client, body, content_len);
    body[read_len > 0 ? read_len : 0] = '\0';
    esp_http_client_cleanup(client);

    /* Parse JSON response */
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse OTA check response");
        return ESP_FAIL;
    }

    cJSON *update = cJSON_GetObjectItem(root, "update");
    if (!cJSON_IsBool(update) || !cJSON_IsTrue(update)) {
        ESP_LOGI(TAG, "No update available (current: %s)", TAB5_FIRMWARE_VER);
        cJSON_Delete(root);
        return ESP_OK;  /* No update, but check succeeded */
    }

    info->available = true;

    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(ver)) {
        strncpy(info->version, ver->valuestring, sizeof(info->version) - 1);
    }

    cJSON *fw_url = cJSON_GetObjectItem(root, "url");
    if (cJSON_IsString(fw_url)) {
        strncpy(info->url, fw_url->valuestring, sizeof(info->url) - 1);
    }

    cJSON *sha = cJSON_GetObjectItem(root, "sha256");
    if (cJSON_IsString(sha)) {
        strncpy(info->sha256, sha->valuestring, sizeof(info->sha256) - 1);
    }

    ESP_LOGI(TAG, "Update available: %s → %s", TAB5_FIRMWARE_VER, info->version);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Convert a 32-byte SHA256 digest to a 64-char lowercase hex string.
 */
static void sha256_to_hex(const uint8_t digest[32], char hex[65])
{
    for (int i = 0; i < 32; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[64] = '\0';
}

esp_err_t tab5_ota_apply(const char *url, const char *expected_sha256)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    bool verify_sha = (expected_sha256 && expected_sha256[0]);
    if (verify_sha) {
        ESP_LOGI(TAG, "OTA SHA256 verification enabled: %.16s...", expected_sha256);
    } else {
        ESP_LOGW(TAG, "OTA SHA256 verification DISABLED — no hash provided (backward compat)");
    }

    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    ESP_LOGI(TAG, "Current partition: %s", tab5_ota_current_partition());

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 60000,  /* 60s for large firmware download */
        /* Tab5's Dragon runs on LAN HTTP (not HTTPS); without this flag
         * esp_https_ota refuses to download with "No option for server
         * verification is enabled".  We defend against MitM via the
         * version.json-carried SHA256 check further down (SEC07 + J2). */
        .skip_cert_common_name_check = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Initialize SHA256 context for streaming hash computation (SEC07) */
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    if (verify_sha) {
        mbedtls_sha256_starts(&sha_ctx, 0);  /* 0 = SHA-256 (not SHA-224) */
    }

    /* Download loop with progress logging + streaming SHA256.
     *
     * HW21: Each esp_https_ota_perform() call may trigger a 64KB flash block
     * erase (~150ms). During the erase the SPI flash bus is blocked, stalling
     * any LVGL rendering that touches flash .rodata (font bitmaps, strings).
     * With CONFIG_SPIRAM_XIP_FROM_PSRAM=y, code executes from PSRAM, but
     * const data (lv_font_montserrat_*) still lives in flash .rodata.
     *
     * Mitigation: yield 10ms every iteration so LVGL gets render time between
     * successive flash erases. This adds ~5s total to a 3MB OTA (trivial vs
     * the download time) but prevents multi-second display freezes. */
    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int total = esp_https_ota_get_image_size(ota_handle);
        int read_len = esp_https_ota_get_image_len_read(ota_handle);
        if (total > 0) {
            int pct = (int)((int64_t)read_len * 100 / total);
            if (pct / 5 != last_pct / 5) {
                ESP_LOGI(TAG, "OTA progress: %d%% (%d / %d bytes)", pct, read_len, total);
                report_progress(pct, "download");
                last_pct = pct;
            }
        }

        /* HW21: Yield to let LVGL render between flash erase cycles */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        return err;
    }

    /*
     * SHA256 verification (SEC07): compute hash over the written OTA partition
     * data and compare against expected_sha256 from version.json.
     *
     * esp_https_ota writes data directly to the OTA partition via esp_ota_write.
     * We read back from the partition to compute SHA256, which also verifies
     * the flash write integrity.
     */
    if (verify_sha) {
        const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
        if (!update_part) {
            ESP_LOGE(TAG, "SHA256: cannot find update partition");
            esp_https_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            return ESP_FAIL;
        }

        int image_size = esp_https_ota_get_image_len_read(ota_handle);
        ESP_LOGI(TAG, "SHA256: verifying %d bytes from partition %s", image_size, update_part->label);
        report_progress(100, "verify");

        /* Read partition in 4KB chunks and feed to SHA256 */
        const size_t chunk_size = 4096;
        uint8_t *chunk = malloc(chunk_size);
        if (!chunk) {
            ESP_LOGE(TAG, "SHA256: malloc failed for read buffer");
            esp_https_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            return ESP_ERR_NO_MEM;
        }

        int offset = 0;
        while (offset < image_size) {
            size_t to_read = (image_size - offset) < (int)chunk_size
                           ? (size_t)(image_size - offset) : chunk_size;
            err = esp_partition_read(update_part, offset, chunk, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "SHA256: partition read failed at offset %d: %s",
                         offset, esp_err_to_name(err));
                free(chunk);
                esp_https_ota_abort(ota_handle);
                mbedtls_sha256_free(&sha_ctx);
                return err;
            }
            mbedtls_sha256_update(&sha_ctx, chunk, to_read);
            offset += to_read;
        }
        free(chunk);

        uint8_t digest[32];
        mbedtls_sha256_finish(&sha_ctx, digest);
        mbedtls_sha256_free(&sha_ctx);

        char computed_hex[65];
        sha256_to_hex(digest, computed_hex);

        ESP_LOGI(TAG, "SHA256 expected: %s", expected_sha256);
        ESP_LOGI(TAG, "SHA256 computed: %s", computed_hex);

        if (strcasecmp(computed_hex, expected_sha256) != 0) {
            ESP_LOGE(TAG, "SHA256 MISMATCH — aborting OTA! Firmware may be corrupted or tampered.");
            esp_https_ota_abort(ota_handle);
            return ESP_ERR_INVALID_CRC;
        }

        ESP_LOGI(TAG, "SHA256 verified OK");
    } else {
        mbedtls_sha256_free(&sha_ctx);
    }

    /* Finalize — validates image and sets boot partition */
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 2 seconds...");
    report_progress(100, "reboot");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  /* unreachable */
}

esp_err_t tab5_ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not get partition state: %s", esp_err_to_name(err));
        return err;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Firmware marked valid — rollback cancelled");
        } else {
            ESP_LOGE(TAG, "Failed to mark valid: %s", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG, "Firmware already valid (state=%d)", state);
    return ESP_OK;
}

const char *tab5_ota_current_partition(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "unknown";
}

/* ── Wave 10 #77 extended-use fix: OTA_PENDING pattern ──────────────────
 *
 * Direct tab5_ota_apply() from long-running sessions fails when the DMA-
 * capable internal SRAM has drifted below the esp_https_ota TLS/HTTP
 * buffer requirement (observed: ~10 KB or less after 30+ min of normal
 * chat use). The reactive DMA watchdog catches the hang but doesn't get
 * the firmware updated.
 *
 * Schedule-and-reboot pattern: caller stores url + sha in NVS, sets a
 * pending flag, then reboots immediately. The fresh boot picks up the
 * flag very early (before voice/camera/LVGL allocate their share of
 * DMA) and calls tab5_ota_apply() with a pristine heap — OTA succeeds.
 */

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"

#define OTA_NVS_NS     "ota_pending"
#define OTA_NVS_FLAG   "pending"
#define OTA_NVS_URL    "url"
#define OTA_NVS_SHA    "sha256"

esp_err_t tab5_ota_schedule(const char *url, const char *expected_sha256)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(OTA_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tab5_ota_schedule: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, OTA_NVS_URL, url);
    if (err == ESP_OK && expected_sha256 && expected_sha256[0]) {
        err = nvs_set_str(h, OTA_NVS_SHA, expected_sha256);
    } else if (err == ESP_OK) {
        /* Clear any stale SHA from a previous schedule. */
        nvs_erase_key(h, OTA_NVS_SHA);  /* ignore error if key absent */
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, OTA_NVS_FLAG, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tab5_ota_schedule: NVS write failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA scheduled for next boot: url=%s (sha=%.16s...)", url,
             expected_sha256 ? expected_sha256 : "none");
    ESP_LOGI(TAG, "Rebooting in 2 seconds to apply on fresh heap...");
    /* Small delay so the debug HTTP response can flush to the caller. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;  /* unreachable */
}

esp_err_t tab5_ota_apply_pending_if_any(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(OTA_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;  /* no NS yet == nothing pending */

    uint8_t pending = 0;
    err = nvs_get_u8(h, OTA_NVS_FLAG, &pending);
    if (err != ESP_OK || pending == 0) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read url + sha, clear flag + keys BEFORE applying so a crash
     * during apply doesn't loop the device forever on the same bad URL. */
    char url[256] = {0};
    char sha[65] = {0};
    size_t sz = sizeof(url);
    err = nvs_get_str(h, OTA_NVS_URL, url, &sz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tab5_ota_apply_pending_if_any: url missing from NVS");
        nvs_erase_key(h, OTA_NVS_FLAG);
        nvs_commit(h);
        nvs_close(h);
        return err;
    }
    sz = sizeof(sha);
    esp_err_t sha_err = nvs_get_str(h, OTA_NVS_SHA, sha, &sz);
    if (sha_err != ESP_OK) sha[0] = 0;  /* no SHA supplied — warn path */

    nvs_erase_key(h, OTA_NVS_FLAG);
    nvs_erase_key(h, OTA_NVS_URL);
    nvs_erase_key(h, OTA_NVS_SHA);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "OTA pending from NVS — applying now (fresh heap)");
    esp_err_t apply_err = tab5_ota_apply(url, sha[0] ? sha : NULL);
    /* tab5_ota_apply esp_restarts on success; if it returns we failed. */
    if (apply_err != ESP_OK) {
        ESP_LOGE(TAG, "Pending OTA apply failed: %s — continuing boot", esp_err_to_name(apply_err));
    }
    return apply_err;
}
