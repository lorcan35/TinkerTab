/**
 * TinkerClaw Tab5 — Audio Subsystem (ES8388 DAC + ES7210 ADC)
 *
 * Single shared I2S bus (I2S_NUM_1) for both playback and recording:
 *   TX (speaker): Standard I2S mode, 48kHz 16-bit stereo
 *   RX (mic):     TDM mode, 48kHz 16-bit, 4 slots (MIC1, AEC, MIC2, MIC-HP)
 *
 * GPIO pins verified against M5Stack Tab5 BSP reference.
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

// ---------------------------------------------------------------------------
// State — shared I2S bus handles exposed to mic.c via getters
// ---------------------------------------------------------------------------
static i2c_master_dev_handle_t s_es8388 = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;  // Created here, used by mic.c
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

// ---------------------------------------------------------------------------
// Volume mapping: 0-100 -> ES8388 DAC volume register (0x00=0dB, 0xC0=-96dB)
// ---------------------------------------------------------------------------
static uint8_t volume_to_reg(uint8_t vol)
{
    if (vol >= 100) return 0x00;
    if (vol == 0)   return 0xC0;
    return (uint8_t)((100 - vol) * 192 / 100);
}

// ---------------------------------------------------------------------------
// ES8388 codec init — DAC output only (slave mode)
// ---------------------------------------------------------------------------
static esp_err_t es8388_codec_init(void)
{
    ESP_LOGI(TAG, "ES8388 codec init");

    // Reset
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_MASTERMODE, 0x00), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    // All power down first
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_CHIPPOWER, 0xFF), TAG, "power down failed");

    // Slave mode, no clock doubler
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_MASTERMODE, 0x00), TAG, "slave mode failed");

    // Power up analog and bias
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_CONTROL1, 0x36), TAG, "ctrl1 failed");
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_CONTROL2, 0x72), TAG, "ctrl2 failed");

    // DAC power: power up DAC L+R, enable LOUT1/ROUT1
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACPOWER, 0x3C), TAG, "dac power failed");

    // DAC Control 1: I2S 16-bit
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL1, 0x00), TAG, "dac ctrl1 failed");

    // DAC Control 2: single speed, no de-emphasis
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL2, 0x02), TAG, "dac ctrl2 failed");

    // DAC unmute
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL3, 0x00), TAG, "unmute failed");

    // DAC volume L+R
    uint8_t vol_reg = volume_to_reg(s_volume);
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL4, vol_reg), TAG, "vol L failed");
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL5, vol_reg), TAG, "vol R failed");

    // Mixer: DAC L->LOUT1, DAC R->ROUT1
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL16, 0x00), TAG, "mix L failed");
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL17, 0xB8), TAG, "mix L vol failed");
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL20, 0x00), TAG, "mix R failed");
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_DACCONTROL21, 0xB8), TAG, "mix R vol failed");

    // Power up
    ESP_RETURN_ON_ERROR(es8388_write(ES8388_CHIPPOWER, 0x00), TAG, "power up failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ES8388 initialized (vol=%d%%)", s_volume);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2S init — Separate ports for TX and RX to fix 12.5% mic duty cycle (#23).
//
// Full-duplex on a single port causes internal clock path interference:
// RX slave clock calculation (MCLK = BCLK × bclk_div) conflicts with
// TX master MCLK generation, resulting in only 1-in-8 frames having data.
//
// Solution: TX-only on I2S_NUM_1 (master, drives MCLK/BCLK/WS/DOUT),
//           RX-only on I2S_NUM_0 (slave, reads same BCLK/WS + DIN).
// Two peripherals can share GPIOs via the GPIO matrix (output vs input).
// ---------------------------------------------------------------------------
static esp_err_t i2s_bus_init(void)
{
    ESP_LOGI(TAG, "I2S init: TX on I2S%d (master), RX on I2S%d (slave)",
             TAB5_I2S_TX_NUM, TAB5_I2S_RX_NUM);

    // --- TX-only channel on port 1 (master) ---
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(TAB5_I2S_TX_NUM, I2S_ROLE_MASTER);
    tx_chan_cfg.auto_clear = true;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&tx_chan_cfg, &s_i2s_tx, NULL),
        TAG, "i2s TX channel create failed"
    );

    // --- RX-only channel on port 0 (slave) ---
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(TAB5_I2S_RX_NUM, I2S_ROLE_SLAVE);
    rx_chan_cfg.auto_clear = true;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&rx_chan_cfg, NULL, &s_i2s_rx),
        TAG, "i2s RX channel create failed"
    );

    // Shared clock config
    i2s_tdm_clk_config_t clk_cfg = {
        .sample_rate_hz  = TAB5_AUDIO_SAMPLE_RATE,
        .clk_src         = I2S_CLK_SRC_DEFAULT,
        .ext_clk_freq_hz = 0,
        .mclk_multiple   = I2S_MCLK_MULTIPLE_256,
        .bclk_div        = 8,
    };

    // --- TX TDM: master, drives MCLK/BCLK/WS, DOUT to ES8388 ---
    // MONO mode: active_slot=1, total_slot=4. BCLK=48k×4×16=3.072MHz.
    // ES8388 reads slot 0, ignores TDM slots 1-3.
    i2s_tdm_config_t tx_tdm_cfg = {
        .clk_cfg  = clk_cfg,
        .slot_cfg = {
            .data_bit_width  = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width  = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode       = I2S_SLOT_MODE_MONO,
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
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_tdm_mode(s_i2s_tx, &tx_tdm_cfg),
        TAG, "i2s TX TDM init failed"
    );

    // --- RX TDM: slave, reads BCLK/WS from TX master + DIN from ES7210 ---
    // STEREO mode with 4 TDM slots: DMA delivers 4 int16_t per frame.
    // MCLK unused (TX master drives it). DOUT unused (RX only).
    i2s_tdm_config_t rx_tdm_cfg = {
        .clk_cfg  = clk_cfg,
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
            .mclk = I2S_GPIO_UNUSED,     // TX master drives MCLK
            .bclk = TAB5_I2S_BCK_GPIO,   // Input: reads TX's BCLK
            .ws   = TAB5_I2S_WS_GPIO,    // Input: reads TX's WS
            .dout = I2S_GPIO_UNUSED,      // RX only
            .din  = TAB5_I2S_DIN_GPIO,    // ES7210 data
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_tdm_mode(s_i2s_rx, &rx_tdm_cfg),
        TAG, "i2s RX TDM init failed"
    );

    // Enable TX first (starts clocks), then RX (reads those clocks)
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "i2s RX enable failed");

    ESP_LOGI(TAG, "I2S TX(port%d master) + RX(port%d slave) enabled (%dHz 16-bit TDM 4-slot)",
             TAB5_I2S_TX_NUM, TAB5_I2S_RX_NUM, TAB5_AUDIO_SAMPLE_RATE);
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

    ESP_LOGI(TAG, "Initializing audio (ES8388 + shared I2S bus)");

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

    // Init shared I2S bus (TX + RX channels)
    ESP_RETURN_ON_ERROR(i2s_bus_init(), TAG, "i2s bus init failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Audio initialized");
    return ESP_OK;
}

i2s_chan_handle_t tab5_audio_get_i2s_rx(void)
{
    return s_i2s_rx;
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
        return ESP_OK;
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
    ESP_LOGI(TAG, "Speaker amp %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}
