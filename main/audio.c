/**
 * TinkerClaw Tab5 — ES8388 Codec Driver (Playback)
 *
 * I2C address 0x10. Drives I2S TX for 16-bit 16kHz audio output.
 * Speaker amplifier (NS4150B) enabled via PI4IOE1 bit P1.
 *
 * I2S port: I2S_NUM_0 (TX only for codec output)
 */

#include "audio.h"
#include "config.h"
#include "io_expander.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "tab5_audio";

// ---------------------------------------------------------------------------
// ES8388 I2C
// ---------------------------------------------------------------------------
#define ES8388_ADDR         0x10
#define I2C_TIMEOUT_MS      50

// ES8388 register addresses
#define ES8388_CONTROL1     0x00
#define ES8388_CONTROL2     0x01
#define ES8388_CHIPPOWER    0x02
#define ES8388_ADCPOWER     0x03
#define ES8388_DACPOWER     0x04
#define ES8388_CHIPLOPOW1   0x05
#define ES8388_CHIPLOPOW2   0x06
#define ES8388_ANAVOLMANAG  0x07
#define ES8388_MASTERMODE   0x08
#define ES8388_ADCCONTROL1  0x09
#define ES8388_ADCCONTROL2  0x0A
#define ES8388_ADCCONTROL3  0x0B
#define ES8388_ADCCONTROL4  0x0C
#define ES8388_ADCCONTROL5  0x0D
#define ES8388_ADCCONTROL6  0x0E
#define ES8388_ADCCONTROL7  0x0F
#define ES8388_ADCCONTROL8  0x10
#define ES8388_ADCCONTROL9  0x11
#define ES8388_ADCCONTROL10 0x12
#define ES8388_ADCCONTROL11 0x13
#define ES8388_ADCCONTROL12 0x14
#define ES8388_ADCCONTROL13 0x15
#define ES8388_ADCCONTROL14 0x16
#define ES8388_DACCONTROL1  0x17
#define ES8388_DACCONTROL2  0x18
#define ES8388_DACCONTROL3  0x19
#define ES8388_DACCONTROL4  0x1A
#define ES8388_DACCONTROL5  0x1B
#define ES8388_DACCONTROL6  0x1C
#define ES8388_DACCONTROL7  0x1D
#define ES8388_DACCONTROL16 0x26
#define ES8388_DACCONTROL17 0x27
#define ES8388_DACCONTROL20 0x2A
#define ES8388_DACCONTROL21 0x2B
#define ES8388_DACCONTROL23 0x2D
#define ES8388_DACCONTROL24 0x2E
#define ES8388_DACCONTROL25 0x2F
#define ES8388_DACCONTROL26 0x30
#define ES8388_DACCONTROL27 0x31

// I2S TX pin assignments — from config.h (verify against schematic)
#define TAB5_AUDIO_SAMPLE_RATE  16000
#define TAB5_AUDIO_CHANNELS     1   // mono output (set 2 for stereo)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static i2c_master_dev_handle_t s_es8388 = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static uint8_t s_volume = 70;  // 0-100
static bool s_initialized = false;

// ---------------------------------------------------------------------------
// ES8388 I2C helpers
// ---------------------------------------------------------------------------
static esp_err_t es8388_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es8388, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t es8388_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_es8388, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// Volume mapping: 0-100 -> ES8388 DAC volume register (0x00=0dB, 0xC0=-96dB)
// Lower register value = louder. We map 100->0x00, 0->0xC0.
// ---------------------------------------------------------------------------
static uint8_t volume_to_reg(uint8_t vol)
{
    if (vol >= 100) return 0x00;
    if (vol == 0)   return 0xC0;
    // Linear map: 100 -> 0, 0 -> 192 (0xC0)
    return (uint8_t)((100 - vol) * 192 / 100);
}

// ---------------------------------------------------------------------------
// ES8388 initialization sequence
// ---------------------------------------------------------------------------
static esp_err_t es8388_codec_init(void)
{
    ESP_LOGI(TAG, "ES8388 codec init");

    // Reset
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_MASTERMODE, 0x00), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    // All power down first
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_CHIPPOWER, 0xFF), TAG, "power down failed");

    // Set chip to slave mode, no clock doubler
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_MASTERMODE, 0x00), TAG, "slave mode failed");

    // Power up analog and bias
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_CONTROL1, 0x36), TAG, "ctrl1 failed");
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_CONTROL2, 0x72), TAG, "ctrl2 failed");

    // DAC power: power up DAC L+R, enable LOUT1/ROUT1
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACPOWER, 0x3C), TAG, "dac power failed");

    // DAC Control 1: I2S 16-bit word length
    //   Bits [4:3] = 00 (16-bit), Bits [2:1] = 00 (left-justified I2S)
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL1, 0x00), TAG, "dac ctrl1 failed");

    // DAC Control 2: DACFsRatio = auto / MCLK ratio for 16kHz
    //   Single speed mode, no de-emphasis
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL2, 0x02), TAG, "dac ctrl2 failed");

    // DAC Control 3: unmute DAC
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL3, 0x00), TAG, "unmute failed");

    // DAC volume L+R
    uint8_t vol_reg = volume_to_reg(s_volume);
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL4, vol_reg), TAG, "vol L failed");
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL5, vol_reg), TAG, "vol R failed");

    // Mixer: DAC L to LOUT1, DAC R to ROUT1
    // DACCONTROL16: LOUT1 source = left DAC, LOUT1 vol = 0dB
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL16, 0x00), TAG, "mix L failed");
    // DACCONTROL17: LOUT1 mixer volume
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL17, 0xB8), TAG, "mix L vol failed");
    // DACCONTROL20: ROUT1 source = right DAC, ROUT1 vol = 0dB
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL20, 0x00), TAG, "mix R failed");
    // DACCONTROL21: ROUT1 mixer volume
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL21, 0xB8), TAG, "mix R vol failed");

    // Power up: clear chip power-down bits
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_CHIPPOWER, 0x00), TAG, "power up failed");

    // Small settle delay
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ES8388 codec initialized (vol=%d%%)", s_volume);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2S TX init
// ---------------------------------------------------------------------------
static esp_err_t i2s_tx_init(void)
{
    ESP_LOGI(TAG, "I2S TX init (I2S_NUM_0)");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL),
        TAG, "i2s new channel failed"
    );

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TAB5_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = TAB5_I2S_MCLK_GPIO,    // TODO: Verify MCLK pin
            .bclk = TAB5_I2S_BCK_GPIO,      // TODO: Verify BCK pin
            .ws   = TAB5_I2S_WS_GPIO,       // TODO: Verify WS pin
            .dout = TAB5_I2S_DOUT_GPIO,     // TODO: Verify DOUT pin
            .din  = -1,                      // TX only
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_i2s_tx, &std_cfg),
        TAG, "i2s std mode init failed"
    );

    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_i2s_tx),
        TAG, "i2s enable failed"
    );

    ESP_LOGI(TAG, "I2S TX enabled (16kHz 16-bit mono)");
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

    ESP_LOGI(TAG, "Initializing audio (ES8388 + I2S TX)");

    // Add ES8388 to I2C bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8388_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_es8388),
        TAG, "i2c add ES8388 failed"
    );

    // Init codec registers
    ESP_RETURN_ON_ERROR(es8388_codec_init(), TAG, "codec init failed");

    // Init I2S TX
    ESP_RETURN_ON_ERROR(i2s_tx_init(), TAG, "i2s tx init failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Audio initialized");
    return ESP_OK;
}

esp_err_t tab5_audio_play_raw(const int16_t *data, size_t samples)
{
    if (!s_initialized || !s_i2s_tx) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_to_write = samples * sizeof(int16_t);
    size_t bytes_written = 0;

    ESP_RETURN_ON_ERROR(
        i2s_channel_write(s_i2s_tx, data, bytes_to_write, &bytes_written, portMAX_DELAY),
        TAG, "i2s write failed"
    );

    if (bytes_written != bytes_to_write) {
        ESP_LOGW(TAG, "Partial write: %zu/%zu bytes", bytes_written, bytes_to_write);
    }

    return ESP_OK;
}

esp_err_t tab5_audio_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_volume = vol;

    if (!s_es8388) {
        return ESP_OK;  // Will apply when initialized
    }

    uint8_t reg_val = volume_to_reg(vol);
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL4, reg_val), TAG, "set vol L failed");
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL5, reg_val), TAG, "set vol R failed");

    ESP_LOGI(TAG, "Volume set to %d%% (reg=0x%02X)", vol, reg_val);
    return ESP_OK;
}

uint8_t tab5_audio_get_volume(void)
{
    return s_volume;
}

esp_err_t tab5_audio_speaker_enable(bool enable)
{
    // NS4150B amplifier is controlled via PI4IOE1 bit P1 (SPK_EN)
    // We use the IO expander's low-level I2C directly since io_expander.h
    // doesn't expose a speaker-specific function yet.
    //
    // From io_expander.c init: PI4IOE1 OUT_SET bit 1 = SPK_EN (set high on init)
    //
    // TODO: Add tab5_set_speaker_enable() to io_expander.h/c for cleaner access.
    //       For now, the IO expander init already sets SPK_EN high (bit P1 in 0b01110110).
    //       This function is a placeholder until we expose that control.

    ESP_LOGI(TAG, "Speaker amp %s", enable ? "enabled" : "disabled");

    // The IO expander already enables the speaker on init.
    // To properly toggle, add a function to io_expander.c:
    //   tab5_set_speaker_enable(enable);

    return ESP_OK;
}
