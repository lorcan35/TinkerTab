/**
 * TinkerClaw Tab5 — Audio Subsystem (ES8388 DAC + ES7210 ADC)
 *
 * Matches M5Stack Tab5 BSP reference exactly:
 *   TX (speaker): STD Philips mode, 48kHz 16-bit MONO → ES8388 DAC
 *   RX (mic):     TDM 4-slot mode, 48kHz 16-bit STEREO → ES7210 quad-mic
 *
 * ES8388 is initialized via esp_codec_dev library (es8388_codec_new) which
 * sets standard I2S mode (DACCONTROL1=0x18). Previous custom register init
 * used DSP/PCM mode (0x1E) which never produced audio — the CONTROL1/CONTROL2
 * values and missing DACCONTROL24/25 registers were also wrong.
 *
 * Speaker amp (NS4150B) controlled via IO expander PI4IOE1 bit P1.
 */

#include "audio.h"
#include "config.h"
#include "io_expander.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "tab5_audio";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static i2s_chan_handle_t         s_i2s_tx = NULL;
static i2s_chan_handle_t         s_i2s_rx = NULL;
static const audio_codec_data_if_t *s_data_if = NULL;
static esp_codec_dev_handle_t   s_play_dev = NULL;   // esp_codec_dev playback device
static uint8_t                  s_volume = 70;        // 0-100
static bool                     s_initialized = false;
static bool                     s_amp_on = false;

// ---------------------------------------------------------------------------
// I2S bus init — STD TX + TDM RX full-duplex on I2S_NUM_1
// Matches M5Stack Tab5 BSP: bsp_audio_init() in m5stack_tab5.c
// ---------------------------------------------------------------------------
static esp_err_t i2s_bus_init(void)
{
    ESP_LOGI(TAG, "I2S bus init (I2S_NUM_%d, TX=STD RX=TDM full-duplex)", TAB5_I2S_NUM);

    // Create both TX and RX on the same I2S port
    // Reduce DMA buffer sizes from defaults (6×240) to save internal SRAM for WiFi SDIO.
    // Default: 6 descriptors × 240 frames × 2 bytes × 4 slots = ~11.5KB per direction.
    // Reduced: 4 descriptors × 160 frames = ~5.1KB per direction, saving ~12KB total.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(TAB5_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 160;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx),
        TAG, "i2s new channel failed"
    );

    // --- TX: STD Philips, 48kHz, 16-bit, MONO ---
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TAB5_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = TAB5_I2S_MCLK_GPIO,
            .bclk = TAB5_I2S_BCK_GPIO,
            .ws   = TAB5_I2S_WS_GPIO,
            .dout = TAB5_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_i2s_tx, &tx_std_cfg),
        TAG, "i2s TX STD init failed"
    );

    // --- RX: TDM 4-slot, STEREO — ES7210 quad-mic ---
    i2s_tdm_config_t rx_tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz  = TAB5_AUDIO_SAMPLE_RATE,
            .clk_src         = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple   = I2S_MCLK_MULTIPLE_256,
            .bclk_div        = 8,
        },
        .slot_cfg = {
            .data_bit_width  = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width  = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode       = I2S_SLOT_MODE_STEREO,
            .slot_mask       = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
            .ws_width        = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol          = false,
            .bit_shift       = true,
            .left_align      = false,
            .big_endian      = false,
            .bit_order_lsb   = false,
            .skip_mask       = false,
            .total_slot      = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = TAB5_I2S_MCLK_GPIO,
            .bclk = TAB5_I2S_BCK_GPIO,
            .ws   = TAB5_I2S_WS_GPIO,
            .dout = TAB5_I2S_DOUT_GPIO,
            .din  = TAB5_I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_tdm_mode(s_i2s_rx, &rx_tdm_cfg),
        TAG, "i2s RX TDM init failed"
    );

    // Enable TX first, then RX
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "i2s RX enable failed");

    // Create shared I2S data interface for esp_codec_dev
    audio_codec_i2s_cfg_t codec_i2s_cfg = {
        .port      = TAB5_I2S_NUM,
        .tx_handle = s_i2s_tx,
        .rx_handle = s_i2s_rx,
    };
    s_data_if = audio_codec_new_i2s_data(&codec_i2s_cfg);
    if (!s_data_if) {
        ESP_LOGE(TAG, "Failed to create I2S codec data interface");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2S TX(STD)+RX(TDM) enabled (%dHz full-duplex)", TAB5_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Create ES8388 playback device via esp_codec_dev library.
// This replaces our old custom register init — the library's es8388_open()
// sets all registers correctly (DACCONTROL1=0x18 for standard I2S mode,
// CONTROL1=0x12, CONTROL2=0x50, DACCONTROL24/25=0x1E, internal DLL, etc.)
// ---------------------------------------------------------------------------
static esp_err_t codec_dev_init(i2c_master_bus_handle_t i2c_bus)
{
    // I2C control interface — 8-bit address 0x20 (7-bit 0x10)
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = TAB5_I2C_NUM,
        .addr       = ES8388_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_FAIL;
    }

    // ES8388 codec — DAC only, slave mode, PA controlled externally
    es8388_codec_cfg_t es_cfg = {
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .ctrl_if     = ctrl_if,
        .pa_pin      = -1,
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "es8388_codec_new failed");
        return ESP_FAIL;
    }

    // Wrap codec + I2S data interface into an esp_codec_dev playback device
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = s_data_if,
    };
    s_play_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_play_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    // Open at 48kHz mono 16-bit — configures I2S clocks and enables DAC
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = TAB5_AUDIO_SAMPLE_RATE,
        .channel         = 1,
        .bits_per_sample = 16,
    };
    esp_err_t err = esp_codec_dev_open(s_play_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %s", esp_err_to_name(err));
        esp_codec_dev_delete(s_play_dev);
        s_play_dev = NULL;
        return err;
    }

    // Set initial volume
    esp_codec_dev_set_out_vol(s_play_dev, s_volume);

    ESP_LOGI(TAG, "ES8388 codec device ready (48kHz mono 16-bit, vol=%d%%)", s_volume);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t tab5_audio_init(i2c_master_bus_handle_t i2c_bus)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing audio (ES8388 via esp_codec_dev + STD/TDM I2S)");

    // Init I2S bus (STD TX + TDM RX)
    ESP_RETURN_ON_ERROR(i2s_bus_init(), TAG, "i2s bus init failed");

    // Init ES8388 via esp_codec_dev library
    ESP_RETURN_ON_ERROR(codec_dev_init(i2c_bus), TAG, "codec dev init failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Audio initialized");
    return ESP_OK;
}

i2s_chan_handle_t tab5_audio_get_i2s_rx(void)
{
    return s_i2s_rx;
}

const audio_codec_data_if_t *tab5_audio_get_data_if(void)
{
    return s_data_if;
}

esp_err_t tab5_audio_play_raw(const int16_t *data, size_t samples)
{
    if (!s_initialized || !s_play_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    // Write mono 16-bit PCM directly — esp_codec_dev handles I2S formatting
    esp_err_t err = esp_codec_dev_write(s_play_dev, (void *)data, samples * sizeof(int16_t));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_write failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t tab5_audio_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_volume = vol;

    if (s_play_dev) {
        esp_codec_dev_set_out_vol(s_play_dev, vol);
    }

    ESP_LOGI(TAG, "Volume set to %d%%", vol);
    return ESP_OK;
}

uint8_t tab5_audio_get_volume(void)
{
    return s_volume;
}

esp_err_t tab5_audio_speaker_enable(bool enable)
{
    if (enable == s_amp_on) return ESP_OK;
    tab5_set_speaker_enable(enable);
    s_amp_on = enable;
    ESP_LOGI(TAG, "Speaker amp %s", enable ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t tab5_audio_test_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "=== SPEAKER TEST: %lu Hz, %lu ms ===", freq_hz, duration_ms);

    if (!s_initialized || !s_play_dev) {
        ESP_LOGE(TAG, "Audio not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Enable speaker amp
    tab5_audio_speaker_enable(true);

    // Generate and play triangle wave through esp_codec_dev
    const size_t chunk_samples = 480;  // 10ms at 48kHz
    int16_t buf[480];
    uint32_t total_samples = (TAB5_AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    uint32_t phase = 0;
    uint32_t phase_inc = (65536UL * freq_hz) / TAB5_AUDIO_SAMPLE_RATE;
    esp_err_t err = ESP_OK;

    while (total_samples > 0) {
        size_t n = (total_samples > chunk_samples) ? chunk_samples : total_samples;
        for (size_t i = 0; i < n; i++) {
            uint16_t p = (uint16_t)(phase & 0xFFFF);
            int32_t val;
            if (p < 16384)       val = (int32_t)p;
            else if (p < 49152)  val = 32768 - (int32_t)p;
            else                 val = (int32_t)p - 65536;
            buf[i] = (int16_t)(val / 2);
            phase += phase_inc;
        }

        err = esp_codec_dev_write(s_play_dev, buf, n * sizeof(int16_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
            break;
        }
        total_samples -= n;
    }

    // Let DMA drain, then disable amp
    vTaskDelay(pdMS_TO_TICKS(200));
    tab5_audio_speaker_enable(false);

    ESP_LOGI(TAG, "=== SPEAKER TEST COMPLETE ===");
    return err;
}
