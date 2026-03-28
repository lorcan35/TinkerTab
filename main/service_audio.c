/**
 * TinkerTab Audio Service — ES8388 codec + ES7210 dual mic
 */

#include "service_registry.h"
#include "audio.h"
#include "settings.h"
#include "esp_log.h"

static const char *TAG = "svc_audio";
static bool s_codec_ok = false;
static bool s_mic_ok = false;

extern i2c_master_bus_handle_t tab5_get_i2c_bus(void);

esp_err_t audio_service_init(void)
{
    i2c_master_bus_handle_t bus = tab5_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "No I2C bus");
        return ESP_ERR_INVALID_STATE;
    }

    /* ES8388 codec + I2S bus */
    esp_err_t ret = tab5_audio_init(bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio codec init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_codec_ok = true;

    /* ES7210 dual mic (uses I2S RX from audio_init) */
    ret = tab5_mic_init(bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic init failed: %s", esp_err_to_name(ret));
    } else {
        s_mic_ok = true;
    }

    return ESP_OK;
}

esp_err_t audio_service_start(void)
{
    if (!s_codec_ok) return ESP_ERR_INVALID_STATE;

    /* Apply stored volume */
    tab5_audio_set_volume(tab5_settings_get_volume());
    return ESP_OK;
}

esp_err_t audio_service_stop(void)
{
    /* Mute speaker amp */
    if (s_codec_ok) {
        tab5_audio_speaker_enable(false);
    }
    return ESP_OK;
}

bool audio_service_codec_ok(void)
{
    return s_codec_ok;
}

bool audio_service_mic_ok(void)
{
    return s_mic_ok;
}
