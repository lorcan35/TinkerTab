/**
 * TinkerClaw Tab5 — ES7210 Quad-Channel Microphone Driver
 *
 * I2C address 0x40. Configures I2S RX in TDM mode for 4-channel capture.
 * Uses the shared I2S bus created by audio.c (I2S_NUM_1).
 *
 * TDM slot layout (from M5Stack BSP):
 *   Slot 0: MIC1 (left)
 *   Slot 1: AEC reference
 *   Slot 2: MIC2 (right)
 *   Slot 3: MIC headphone / high-pass
 */

#include "audio.h"
#include "config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_common.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "tab5_mic";

// ---------------------------------------------------------------------------
// ES7210 I2C
// ---------------------------------------------------------------------------
#define ES7210_ADDR         0x40
#define I2C_TIMEOUT_MS      50

// ES7210 register addresses
#define ES7210_RESET        0x00
#define ES7210_CLK_ON_OFF   0x01
#define ES7210_MCLK_CTL     0x02
#define ES7210_MST_CLK_CTL  0x03
#define ES7210_MST_LRCKH    0x04
#define ES7210_MST_LRCKL    0x05
#define ES7210_DIGITAL_PDN  0x06
#define ES7210_ADC_OSR      0x07
#define ES7210_MODE_CFG     0x08
#define ES7210_SDP_INT1_CFG 0x11
#define ES7210_SDP_INT2_CFG 0x12
#define ES7210_ADC_AUTOMUTE 0x13
#define ES7210_ADC34_MUTE   0x14
#define ES7210_ADC12_MUTE   0x15
#define ES7210_MIC1_GAIN    0x43
#define ES7210_MIC2_GAIN    0x44
#define ES7210_MIC3_GAIN    0x45
#define ES7210_MIC4_GAIN    0x46
#define ES7210_MIC1_POWER   0x4B
#define ES7210_MIC2_POWER   0x4C
#define ES7210_MIC3_POWER   0x4D
#define ES7210_MIC4_POWER   0x4E
#define ES7210_ANALOG_SYS   0x40

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static i2c_master_dev_handle_t s_es7210 = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;  // From audio.c shared bus
static uint8_t s_mic_gain = 24;  // dB, default
static bool s_mic_initialized = false;

// ---------------------------------------------------------------------------
// ES7210 I2C helpers
// ---------------------------------------------------------------------------
static esp_err_t es7210_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es7210, buf, 2, I2C_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// ES7210 ADC initialization — 4-channel TDM mode
// ---------------------------------------------------------------------------
static esp_err_t es7210_adc_init(void)
{
    ESP_LOGI(TAG, "ES7210 ADC init (4-ch TDM)");

    // Software reset
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_RESET, 0xFF), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_RESET, 0x41), TAG, "reset release failed");

    // Enable all clocks
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_CLK_ON_OFF, 0x3F), TAG, "clk on failed");

    // MCLK from pad, no divide
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MCLK_CTL, 0x00), TAG, "mclk ctl failed");

    // Master clock divider for 48kHz (MCLK=256*48kHz=12.288MHz)
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MST_CLK_CTL, 0x01), TAG, "mst clk failed");

    // ADC oversampling ratio
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC_OSR, 0x20), TAG, "osr failed");

    // SDP config: TDM mode, 16-bit word length
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_SDP_INT1_CFG, 0x0C), TAG, "sdp1 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_SDP_INT2_CFG, 0x00), TAG, "sdp2 failed");

    // Mode: enable TDM
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MODE_CFG, 0x01), TAG, "mode failed");

    // Analog system
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ANALOG_SYS, 0x43), TAG, "analog sys failed");

    // Mic gain for all 4 channels
    uint8_t gain_reg = (s_mic_gain > 36) ? 0x0C : (s_mic_gain / 3);
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_GAIN, gain_reg), TAG, "mic1 gain failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_GAIN, gain_reg), TAG, "mic2 gain failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC3_GAIN, gain_reg), TAG, "mic3 gain failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC4_GAIN, gain_reg), TAG, "mic4 gain failed");

    // Power up all 4 mics
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_POWER, 0x70), TAG, "mic1 pwr failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_POWER, 0x70), TAG, "mic2 pwr failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC3_POWER, 0x70), TAG, "mic3 pwr failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC4_POWER, 0x70), TAG, "mic4 pwr failed");

    // Enable digital: clear power-down bits
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_DIGITAL_PDN, 0x00), TAG, "digital pdn failed");

    // Unmute all ADC channels
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC12_MUTE, 0x00), TAG, "unmute 12 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC34_MUTE, 0x00), TAG, "unmute 34 failed");

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ES7210 initialized (4-ch TDM, gain=%d dB)", s_mic_gain);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2S RX handle — already initialized+enabled by audio.c (TX+RX together)
// ---------------------------------------------------------------------------
static esp_err_t i2s_rx_acquire(void)
{
    s_i2s_rx = tab5_audio_get_i2s_rx();
    if (!s_i2s_rx) {
        ESP_LOGE(TAG, "I2S RX handle is NULL — call tab5_audio_init() first!");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "I2S RX acquired (already running from audio.c)");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t tab5_mic_init(i2c_master_bus_handle_t i2c_bus)
{
    if (s_mic_initialized) {
        ESP_LOGW(TAG, "Mic already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing mic (ES7210 + I2S RX TDM)");

    // Add ES7210 to I2C bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES7210_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_es7210),
        TAG, "i2c add ES7210 failed"
    );

    // Init ADC registers (4-channel TDM)
    ESP_RETURN_ON_ERROR(es7210_adc_init(), TAG, "adc init failed");

    // Acquire I2S RX handle (already initialized+enabled by audio.c)
    ESP_RETURN_ON_ERROR(i2s_rx_acquire(), TAG, "i2s rx acquire failed");

    s_mic_initialized = true;
    ESP_LOGI(TAG, "Mic initialized (4-ch TDM @ %dHz)", TAB5_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t tab5_mic_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    if (!s_mic_initialized || !s_i2s_rx) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_to_read = samples * sizeof(int16_t);
    size_t bytes_read = 0;

    esp_err_t err = i2s_channel_read(s_i2s_rx, buf, bytes_to_read, &bytes_read,
                                     pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_read failed: %s (0x%x), requested %zu bytes, got %zu, timeout=%lums",
                 esp_err_to_name(err), err, bytes_to_read, bytes_read, (unsigned long)timeout_ms);
        return err;
    }

    if (bytes_read != bytes_to_read) {
        ESP_LOGW(TAG, "Partial read: %zu/%zu bytes", bytes_read, bytes_to_read);
    }

    return ESP_OK;
}

esp_err_t tab5_mic_set_gain(uint8_t gain_db)
{
    if (gain_db > 36) gain_db = 36;
    s_mic_gain = gain_db;

    if (!s_es7210) {
        return ESP_OK;
    }

    uint8_t gain_reg = gain_db / 3;
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_GAIN, gain_reg), TAG, "set gain ch1 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_GAIN, gain_reg), TAG, "set gain ch2 failed");

    ESP_LOGI(TAG, "Mic gain set to %d dB (reg=0x%02X)", gain_db, gain_reg);
    return ESP_OK;
}
