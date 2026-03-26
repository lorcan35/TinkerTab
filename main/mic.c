/**
 * TinkerClaw Tab5 — ES7210 Dual Microphone Driver
 *
 * I2C address 0x40. Drives I2S RX for 16-bit 16kHz dual-channel mic input.
 * Uses I2S_NUM_1 to avoid conflict with ES8388 codec on I2S_NUM_0.
 */

#include "audio.h"
#include "config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
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
#define ES7210_TCT0_CHPINI  0x09
#define ES7210_TCT1_CHLDF   0x0A
#define ES7210_CH_FADC_FLG  0x0B
#define ES7210_SDP_INT1_CFG 0x11
#define ES7210_SDP_INT2_CFG 0x12
#define ES7210_ADC_AUTOMUTE 0x13
#define ES7210_ADC34_MUTE   0x14
#define ES7210_ADC12_MUTE   0x15
#define ES7210_ALC_COM_CFG1 0x16
#define ES7210_ALC_COM_CFG2 0x17
#define ES7210_ADC1_MAX_GAIN 0x1C
#define ES7210_ADC2_MAX_GAIN 0x1D
#define ES7210_ADC3_MAX_GAIN 0x1E
#define ES7210_ADC4_MAX_GAIN 0x1F
#define ES7210_ADC1_DIRECT_DB 0x1E   // Direct gain register for ch1
#define ES7210_ADC2_DIRECT_DB 0x1F   // Direct gain register for ch2
#define ES7210_MIC1_GAIN    0x43
#define ES7210_MIC2_GAIN    0x44
#define ES7210_MIC3_GAIN    0x45
#define ES7210_MIC4_GAIN    0x46
#define ES7210_MIC1_POWER   0x4B
#define ES7210_MIC2_POWER   0x4C
#define ES7210_MIC3_POWER   0x4D
#define ES7210_MIC4_POWER   0x4E
#define ES7210_MIC12_PDN    0x4B
#define ES7210_MIC34_PDN    0x4C
#define ES7210_ANALOG_SYS   0x40
#define ES7210_MISC_CTL     0x47
#define ES7210_POWER_DOWN   0x06

// I2S RX pin assignments — from config.h (verify against schematic)
#define TAB5_MIC_SAMPLE_RATE    16000
#define TAB5_MIC_CHANNELS       2     // dual mic = stereo

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static i2c_master_dev_handle_t s_es7210 = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;
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

static esp_err_t es7210_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_es7210, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// ES7210 initialization sequence
// ---------------------------------------------------------------------------
static esp_err_t es7210_adc_init(void)
{
    ESP_LOGI(TAG, "ES7210 ADC init");

    // Software reset
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_RESET, 0xFF), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_RESET, 0x41), TAG, "reset release failed");

    // Clock config: enable all clocks
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_CLK_ON_OFF, 0x3F), TAG, "clk on failed");

    // MCLK config: from pad, no divide
    // TODO: Verify MCLK source — may need adjustment for Tab5 clock routing
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MCLK_CTL, 0x00), TAG, "mclk ctl failed");

    // Master clock divider for 16kHz sample rate
    // Assuming MCLK = 256 * Fs = 4.096MHz for 16kHz
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MST_CLK_CTL, 0x03), TAG, "mst clk failed");

    // ADC oversampling ratio
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC_OSR, 0x20), TAG, "osr failed");

    // SDP (serial data port) config: I2S format, 16-bit word length
    //   Bits [7:6] = 00 (I2S), Bits [4:2] = 011 (16-bit), Bits [1:0] = 00
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_SDP_INT1_CFG, 0x0C), TAG, "sdp1 failed");

    // Channel enable: enable ADC1 + ADC2 (channels 1 and 2)
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_SDP_INT2_CFG, 0x00), TAG, "sdp2 failed");

    // Mode config: TDM mode off, DSP mode off
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MODE_CFG, 0x00), TAG, "mode failed");

    // Analog system config
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ANALOG_SYS, 0x43), TAG, "analog sys failed");

    // Mic gain for channels 1 and 2
    // Gain values: 0x00=0dB, 0x01=3dB, ... 0x0C=36dB
    uint8_t gain_reg = (s_mic_gain > 36) ? 0x0C : (s_mic_gain / 3);
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_GAIN, gain_reg), TAG, "mic1 gain failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_GAIN, gain_reg), TAG, "mic2 gain failed");

    // Power up mic 1 + mic 2 (bias + PGA)
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_POWER, 0x70), TAG, "mic1 pwr failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_POWER, 0x70), TAG, "mic2 pwr failed");
    // Keep mic 3+4 powered down
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC3_POWER, 0xFF), TAG, "mic3 pdn failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC4_POWER, 0xFF), TAG, "mic4 pdn failed");

    // Enable digital: clear power-down bits for ADC1+2
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_DIGITAL_PDN, 0x00), TAG, "digital pdn failed");

    // Unmute ADC 1+2
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC12_MUTE, 0x00), TAG, "unmute 12 failed");
    // Mute ADC 3+4 (not used)
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC34_MUTE, 0x03), TAG, "mute 34 failed");

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ES7210 ADC initialized (gain=%d dB)", s_mic_gain);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2S RX init
// ---------------------------------------------------------------------------
static esp_err_t i2s_rx_init(void)
{
    ESP_LOGI(TAG, "I2S RX init (I2S_NUM_1)");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx),
        TAG, "i2s new rx channel failed"
    );

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TAB5_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = TAB5_I2S_MIC_MCLK_GPIO,  // TODO: Verify MCLK pin
            .bclk = TAB5_I2S_MIC_BCK_GPIO,    // TODO: Verify BCK pin
            .ws   = TAB5_I2S_MIC_WS_GPIO,     // TODO: Verify WS pin
            .dout = -1,                         // RX only
            .din  = TAB5_I2S_MIC_DIN_GPIO,    // TODO: Verify DIN pin
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_i2s_rx, &std_cfg),
        TAG, "i2s rx std mode init failed"
    );

    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_i2s_rx),
        TAG, "i2s rx enable failed"
    );

    ESP_LOGI(TAG, "I2S RX enabled (16kHz 16-bit stereo, dual mic)");
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

    ESP_LOGI(TAG, "Initializing mic (ES7210 + I2S RX)");

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

    // Init ADC registers
    ESP_RETURN_ON_ERROR(es7210_adc_init(), TAG, "adc init failed");

    // Init I2S RX
    ESP_RETURN_ON_ERROR(i2s_rx_init(), TAG, "i2s rx init failed");

    s_mic_initialized = true;
    ESP_LOGI(TAG, "Mic initialized");
    return ESP_OK;
}

esp_err_t tab5_mic_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    if (!s_mic_initialized || !s_i2s_rx) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_to_read = samples * sizeof(int16_t);
    size_t bytes_read = 0;

    ESP_RETURN_ON_ERROR(
        i2s_channel_read(s_i2s_rx, buf, bytes_to_read, &bytes_read,
                         pdMS_TO_TICKS(timeout_ms)),
        TAG, "i2s read failed"
    );

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
        return ESP_OK;  // Will apply when initialized
    }

    // Gain register: 0x00=0dB, 0x01=3dB, ..., 0x0C=36dB (3dB steps)
    uint8_t gain_reg = gain_db / 3;
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_GAIN, gain_reg), TAG, "set gain ch1 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_GAIN, gain_reg), TAG, "set gain ch2 failed");

    ESP_LOGI(TAG, "Mic gain set to %d dB (reg=0x%02X)", gain_db, gain_reg);
    return ESP_OK;
}
