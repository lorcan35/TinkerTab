/**
 * TinkerClaw Tab5 — ES7210 Quad-Channel Microphone Driver (esp_codec_dev)
 *
 * Uses espressif/esp_codec_dev library for proper ES7210 initialization,
 * replacing hand-rolled register writes that had incorrect TDM slot mapping.
 *
 * The esp_codec_dev library (es7210_codec_new) handles:
 *   - Correct register init sequence for ES7210
 *   - Mic selection and TDM slot assignment
 *   - Clock and power sequencing
 *
 * Audio data flows through the shared I2S data interface created by audio.c.
 */

#include "audio.h"
#include "config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <math.h>

static const char *TAG = "tab5_mic";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static esp_codec_dev_handle_t s_mic_dev = NULL;
static bool s_mic_initialized = false;
static float s_mic_gain_db = 37.0f;  // near-max PGA gain for STT pickup

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t tab5_mic_init(i2c_master_bus_handle_t i2c_bus)
{
    if (s_mic_initialized) {
        ESP_LOGW(TAG, "Mic already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing mic (ES7210 via esp_codec_dev)");

    // Get shared I2S data interface from audio.c
    const audio_codec_data_if_t *data_if = tab5_audio_get_data_if();
    if (!data_if) {
        ESP_LOGE(TAG, "I2S data interface not available — call tab5_audio_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // Create I2C control interface for ES7210
    // esp_codec_dev uses 8-bit I2C addresses (shifted left by 1)
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = TAB5_I2C_NUM,
        .addr       = ES7210_CODEC_DEFAULT_ADDR,  // 0x80 (8-bit) = 0x40 (7-bit)
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "Failed to create I2C control interface for ES7210");
        return ESP_FAIL;
    }

    // Create ES7210 codec with ALL 4 mics selected (matches M5Stack UserDemo)
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = ctrl_if,
    };
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    const audio_codec_if_t *codec_if = es7210_codec_new(&es7210_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "Failed to create ES7210 codec interface");
        return ESP_FAIL;
    }

    // Create codec device (input/ADC type)
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_mic_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_mic_dev) {
        ESP_LOGE(TAG, "Failed to create mic codec device");
        return ESP_FAIL;
    }

    // Open codec: 48kHz, 16-bit, 4 channels (matches M5Stack bsp_codec_es7210_set(48000, 16, 4))
    esp_codec_dev_sample_info_t fs = {
        .sample_rate    = TAB5_AUDIO_SAMPLE_RATE,
        .channel        = 4,
        .bits_per_sample = 16,
    };
    int ret = esp_codec_dev_open(s_mic_dev, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to open mic codec: %d", ret);
        return ESP_FAIL;
    }

    // Set input gain
    esp_codec_dev_set_in_gain(s_mic_dev, s_mic_gain_db);

    s_mic_initialized = true;
    ESP_LOGI(TAG, "Mic initialized via esp_codec_dev (4-ch @ %dHz, gain=%.0fdB)",
             TAB5_AUDIO_SAMPLE_RATE, s_mic_gain_db);
    return ESP_OK;
}

esp_err_t tab5_mic_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    if (!s_mic_initialized || !s_mic_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    // esp_codec_dev_read blocks until data is available (no timeout param).
    // For real-time 48kHz mic stream this completes within the frame period.
    (void)timeout_ms;

    int bytes = (int)(samples * sizeof(int16_t));
    int ret = esp_codec_dev_read(s_mic_dev, buf, bytes);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_read failed: %d (requested %d bytes)", ret, bytes);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tab5_mic_set_gain(uint8_t gain_db)
{
    if (gain_db > 37) gain_db = 37;
    s_mic_gain_db = (float)gain_db;

    if (!s_mic_dev) {
        return ESP_OK;  // Will apply on next init
    }

    int ret = esp_codec_dev_set_in_gain(s_mic_dev, s_mic_gain_db);
    if (ret == 0) {
        ESP_LOGI(TAG, "Mic gain set to %.0f dB", s_mic_gain_db);
    }
    return ret;
}

void tab5_mic_diag(void)
{
    printf("=== ES7210 Mic Diagnostic (esp_codec_dev) ===\n");

    if (!s_mic_dev) {
        printf("Codec device not initialized!\n");
        return;
    }

    // Read 100ms of 4-channel data: 4800 frames × 4 ch = 19200 samples
    const int frames = 4800;
    const int channels = 4;
    const int total_samples = frames * channels;
    int16_t *buf = heap_caps_malloc(total_samples * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        printf("Alloc failed\n");
        return;
    }

    esp_err_t err = esp_codec_dev_read(s_mic_dev, buf, total_samples * sizeof(int16_t));
    printf("esp_codec_dev_read: %s, bytes=%d\n",
           esp_err_to_name(err), (int)(total_samples * sizeof(int16_t)));

    if (err == ESP_OK) {
        // Per-channel stats
        for (int ch = 0; ch < channels; ch++) {
            int64_t sum = 0;
            int16_t mn = 32767, mx = -32768;
            int nonzero = 0;
            int count = 0;
            for (int i = ch; i < total_samples; i += channels) {
                int16_t v = buf[i];
                sum += (int64_t)v * v;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
                if (v != 0) nonzero++;
                count++;
            }
            float rms = sqrtf((float)(sum / (count > 0 ? count : 1)));
            printf("  CH%d: min=%d max=%d rms=%.0f nonzero=%d/%d\n",
                   ch, mn, mx, rms, nonzero, count);
        }

        // Print first 16 frames raw hex
        printf("  First 16 frames (hex): ");
        int show = (total_samples < 64) ? total_samples : 64;
        for (int i = 0; i < show; i++) {
            if (i > 0 && i % 4 == 0) printf("| ");
            printf("%04X ", (uint16_t)buf[i]);
        }
        printf("\n");
    }

    heap_caps_free(buf);
    printf("=== End Diagnostic ===\n");
}
