/**
 * TinkerTab — ESP-SR Audio Front End (AEC + Wake Word)
 *
 * Integrates Espressif's ESP-SR AFE with the Tab5 audio pipeline.
 * The Tab5 ES7210 captures 4 TDM channels: [MIC1, MIC2, AEC_REF, unused].
 * The AEC reference comes from the ES8388 DAC output routed back to the
 * ES7210 MIC3 input on the PCB (hardware loopback).
 */

#include "afe.h"
#include "config.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_vad.h"

static const char *TAG = "tab5_afe";

/* AFE state */
static const esp_afe_sr_iface_t *s_afe_iface = NULL;
static esp_afe_sr_data_t        *s_afe_data  = NULL;
static bool                      s_active    = false;
static const char               *s_wn_name   = "unknown";
static int                       s_feed_chunksize = 0;  /* samples per feed() call */

esp_err_t tab5_afe_init(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "AFE already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing ESP-SR AFE (AEC + WakeNet)...");

    /* Load SR models from partition/SPIFFS */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "Failed to init SR models — check model partition");
        return ESP_FAIL;
    }

    /* Find WakeNet model — prefer "hiesp" (English), fall back to "hilexin" */
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");
    if (!wn_name) {
        wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hilexin");
    }
    if (!wn_name) {
        wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    }
    if (!wn_name) {
        ESP_LOGE(TAG, "No WakeNet model found in model partition");
        return ESP_FAIL;
    }
    s_wn_name = wn_name;
    ESP_LOGI(TAG, "WakeNet model: %s", wn_name);

    /*
     * AFE configuration for Tab5:
     *   "MMR" = 2 microphones + 1 reference channel
     *
     * The TDM interleaved feed order must match the physical slot layout.
     * After Step 1 verification, adjust the format string if needed.
     * Options: "MMR", "MRM", "RMM" etc. — position of R matters.
     *
     * AFE_TYPE_SR = speech recognition (continuous listening + wake word)
     * AFE_MODE_HIGH_PERF = best quality AEC/NS (uses more CPU)
     */
    afe_config_t *cfg = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    cfg->wakenet_model_name = wn_name;
    cfg->vad_min_speech_ms  = 300;
    cfg->vad_min_noise_ms   = 600;
    cfg->memory_alloc_mode  = AFE_MEMORY_ALLOC_MORE_PSRAM;

    /* Create AFE handle — uses the new API (v2.4+) */
    s_afe_iface = esp_afe_handle_from_config(cfg);
    if (!s_afe_iface) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        return ESP_FAIL;
    }
    s_afe_data = s_afe_iface->create_from_config(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed — likely OOM");
        return ESP_ERR_NO_MEM;
    }

    s_active = true;
    s_feed_chunksize = s_afe_iface->get_feed_chunksize(s_afe_data);

    ESP_LOGI(TAG, "AFE ready — wake word: %s, AEC: on, VAD: on", wn_name);
    ESP_LOGI(TAG, "  feed_chunksize: %d samples (expected per feed() call)", s_feed_chunksize);
    ESP_LOGI(TAG, "  PSRAM free: %lu KB", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    ESP_LOGI(TAG, "  SRAM free:  %lu KB", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    return ESP_OK;
}

void tab5_afe_deinit(void)
{
    if (s_afe_iface && s_afe_data) {
        s_afe_iface->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    s_active = false;
    ESP_LOGI(TAG, "AFE shut down");
}

esp_err_t tab5_afe_feed(const int16_t *data, int len)
{
    if (!s_active || !s_afe_iface || !s_afe_data) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ESP-SR AFE feed expects interleaved [mic1, mic2, ref] × N samples */
    s_afe_iface->feed(s_afe_data, data);
    return ESP_OK;
}

esp_err_t tab5_afe_fetch(int16_t **out_data, int *out_samples,
                         bool *out_wake, bool *out_vad)
{
    if (!s_active || !s_afe_iface || !s_afe_data) {
        return ESP_ERR_INVALID_STATE;
    }

    afe_fetch_result_t *res = s_afe_iface->fetch(s_afe_data);
    if (!res) {
        return ESP_FAIL;
    }

    *out_data    = res->data;
    *out_samples = res->data_size / sizeof(int16_t);
    *out_wake    = (res->wakeup_state == WAKENET_DETECTED);
    *out_vad     = (res->vad_state == VAD_SPEECH);

    return ESP_OK;
}

bool tab5_afe_is_active(void)
{
    return s_active;
}

const char *tab5_afe_wake_word_name(void)
{
    return s_wn_name;
}

int tab5_afe_get_feed_chunksize(void)
{
    return s_feed_chunksize;
}
