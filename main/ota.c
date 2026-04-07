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
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ota";

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

esp_err_t tab5_ota_apply(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    ESP_LOGI(TAG, "Current partition: %s", tab5_ota_current_partition());

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 60000,  /* 60s for large firmware download */
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

    /* Download loop with progress logging */
    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int total = esp_https_ota_get_image_size(ota_handle);
        int read = esp_https_ota_get_image_len_read(ota_handle);
        if (total > 0) {
            int pct = (int)((int64_t)read * 100 / total);
            if (pct / 10 != last_pct / 10) {
                ESP_LOGI(TAG, "OTA progress: %d%% (%d / %d bytes)", pct, read, total);
                last_pct = pct;
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        return err;
    }

    /* Finalize — validates image and sets boot partition */
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 2 seconds...");
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
