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
#include <math.h>
#include "esp_heap_caps.h"

static const char *TAG = "tab5_mic";

// ---------------------------------------------------------------------------
// ES7210 I2C
// ---------------------------------------------------------------------------
#define ES7210_ADDR         0x40
#define I2C_TIMEOUT_MS      50

// ES7210 register addresses (from esp_codec_dev v1.5.6)
#define ES7210_RESET          0x00
#define ES7210_CLK_ON_OFF     0x01
#define ES7210_MCLK_CTL       0x02
#define ES7210_MST_CLK_CTL    0x03
#define ES7210_MST_LRCKH      0x04
#define ES7210_MST_LRCKL      0x05
#define ES7210_DIGITAL_PDN    0x06
#define ES7210_ADC_OSR        0x07
#define ES7210_MODE_CFG       0x08
#define ES7210_TIME_CTL0      0x09
#define ES7210_TIME_CTL1      0x0A
#define ES7210_SDP_INT1_CFG   0x11
#define ES7210_SDP_INT2_CFG   0x12
#define ES7210_ADC_AUTOMUTE   0x13
#define ES7210_ADC34_MUTE     0x14
#define ES7210_ADC12_MUTE     0x15
#define ES7210_ADC34_HPF2     0x20
#define ES7210_ADC34_HPF1     0x21
#define ES7210_ADC12_HPF1     0x22
#define ES7210_ADC12_HPF2     0x23
#define ES7210_ANALOG_SYS     0x40
#define ES7210_MIC12_BIAS     0x41
#define ES7210_MIC34_BIAS     0x42
#define ES7210_MIC1_GAIN      0x43
#define ES7210_MIC2_GAIN      0x44
#define ES7210_MIC3_GAIN      0x45
#define ES7210_MIC4_GAIN      0x46
#define ES7210_MIC1_POWER     0x47
#define ES7210_MIC2_POWER     0x48
#define ES7210_MIC3_POWER     0x49
#define ES7210_MIC4_POWER     0x4A
#define ES7210_MIC12_POWER    0x4B
#define ES7210_MIC34_POWER    0x4C

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static i2c_master_dev_handle_t s_es7210 = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;  // From audio.c shared bus
static uint8_t s_mic_gain = 30;  // dB, default (matches esp_codec_dev)
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

static void es7210_dump_regs(void)
{
    static const uint8_t regs[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x11, 0x12, 0x14, 0x15,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
        0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C
    };
    ESP_LOGI(TAG, "ES7210 register dump:");
    for (int i = 0; i < sizeof(regs); i++) {
        uint8_t val = 0;
        if (es7210_read(regs[i], &val) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X = 0x%02X", regs[i], val);
        } else {
            ESP_LOGW(TAG, "  0x%02X = READ FAIL", regs[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// ES7210 ADC initialization — 4-channel TDM mode
// ---------------------------------------------------------------------------
static esp_err_t es7210_adc_init(void)
{
    ESP_LOGI(TAG, "ES7210 ADC init (4-ch TDM, slave mode)");

    // --- Step 1: Reset ---
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_RESET, 0xFF), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_RESET, 0x41), TAG, "reset release failed");

    // --- Step 2: Clock & timing ---
    // Bits 0-3 = clock gate for MIC1-4 (1=OFF, 0=ON). Must clear to enable.
    // Start with 0x3F (all off), will enable after mic select in step 8.
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_CLK_ON_OFF, 0x3F), TAG, "clk init failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_TIME_CTL0, 0x30), TAG, "time0 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_TIME_CTL1, 0x30), TAG, "time1 failed");

    // --- Step 3: High-pass filter (removes DC offset) ---
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC12_HPF2, 0x2A), TAG, "hpf12_2 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC12_HPF1, 0x0A), TAG, "hpf12_1 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC34_HPF2, 0x0A), TAG, "hpf34_2 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC34_HPF1, 0x2A), TAG, "hpf34_1 failed");

    // --- Step 4: SLAVE mode (ESP32-P4 provides MCLK/BCLK/WS) ---
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MODE_CFG, 0x00), TAG, "slave mode failed");

    // --- Step 5: Analog power + MCLK config ---
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ANALOG_SYS, 0x43), TAG, "analog sys failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC12_BIAS, 0x70), TAG, "mic12 bias failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC34_BIAS, 0x70), TAG, "mic34 bias failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC_OSR, 0x20), TAG, "osr failed");
    // MCLK: DLL=1, doubler=1, adc_div=1 (for 12.288MHz MCLK @ 48kHz)
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MCLK_CTL, 0xC1), TAG, "mclk ctl failed");

    // --- Step 6: SDP config — I2S format, 16-bit ---
    // Bits[1:0]=0x00 (I2S Philips), bits[7:5]=0x60 (16-bit)
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_SDP_INT1_CFG, 0x60), TAG, "sdp1 failed");
    // TDM mode ENABLED (0x02)
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_SDP_INT2_CFG, 0x02), TAG, "sdp2 TDM failed");

    // --- Step 7: Mic select — follows reference driver es7210_mic_select() ---
    // Must power-down then power-up each mic pair, and set clock gates per-pair.
    uint8_t gain_val = (s_mic_gain > 37) ? 0x0E : (s_mic_gain / 3);

    // First: disable PGA on all channels, power down mic pairs
    for (int i = 0; i < 4; i++) {
        uint8_t reg = ES7210_MIC1_GAIN + i;
        uint8_t cur = 0;
        es7210_read(reg, &cur);
        ESP_RETURN_ON_ERROR(es7210_write(reg, cur & ~0x10), TAG, "pga disable failed");
    }
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC12_POWER, 0xFF), TAG, "mic12 pwr down failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC34_POWER, 0xFF), TAG, "mic34 pwr down failed");

    // MIC1+MIC2: enable clock bits 0,1,3 (mask 0x0b) via read-modify-write
    {
        uint8_t clk = 0;
        es7210_read(ES7210_CLK_ON_OFF, &clk);
        clk = (clk & ~0x0b) | (0x0b & 0x00);  // Clear bits 0,1,3
        ESP_RETURN_ON_ERROR(es7210_write(ES7210_CLK_ON_OFF, clk), TAG, "clk mic12 failed");
    }
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC12_POWER, 0x00), TAG, "mic12 pwr up failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_GAIN, 0x10 | gain_val), TAG, "mic1 gain failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_GAIN, 0x10 | gain_val), TAG, "mic2 gain failed");

    // MIC3+MIC4: enable clock bits 0,2,4 (mask 0x15) via read-modify-write
    {
        uint8_t clk = 0;
        es7210_read(ES7210_CLK_ON_OFF, &clk);
        clk = (clk & ~0x15) | (0x15 & 0x00);  // Clear bits 0,2,4
        ESP_RETURN_ON_ERROR(es7210_write(ES7210_CLK_ON_OFF, clk), TAG, "clk mic34 failed");
    }
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC34_POWER, 0x00), TAG, "mic34 pwr up failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC3_GAIN, 0x10 | gain_val), TAG, "mic3 gain failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC4_GAIN, 0x10 | gain_val), TAG, "mic4 gain failed");

    // --- Step 8: Power up individual mic channels ---
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_POWER, 0x08), TAG, "mic1 pwr failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_POWER, 0x08), TAG, "mic2 pwr failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC3_POWER, 0x08), TAG, "mic3 pwr failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC4_POWER, 0x08), TAG, "mic4 pwr failed");

    // After mic select: CLK_ON_OFF = 0x20 (bit 5 = MCLK gated).
    // With MONO TDM, ESP32-P4 generates correct MCLK=12.288MHz.
    // Enable ALL clocks including MCLK (bit 5=0) so ES7210 uses external MCLK
    // directly instead of DLL from BCLK, which may cause 25% duty cycle.
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_CLK_ON_OFF, 0x00), TAG, "clk all-on failed");
    {
        uint8_t clk_verify = 0xFF;
        es7210_read(ES7210_CLK_ON_OFF, &clk_verify);
        ESP_LOGI(TAG, "CLK_ON_OFF final = 0x%02X (expect 0x00)", clk_verify);
    }

    // --- Step 9: Enable digital, unmute ---
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_DIGITAL_PDN, 0x00), TAG, "digital pdn failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC12_MUTE, 0x00), TAG, "unmute 12 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_ADC34_MUTE, 0x00), TAG, "unmute 34 failed");

    // --- Step 10: Soft reset to latch all config ---
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_RESET, 0x71), TAG, "soft reset failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_RESET, 0x41), TAG, "reset release2 failed");

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ES7210 initialized (4-ch TDM slave, gain=%ddB pga=0x%02X)",
             s_mic_gain, (uint8_t)(0x10 | gain_val));
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

    // Dump register state to verify writes
    es7210_dump_regs();

    // Acquire I2S RX handle (already initialized+enabled by audio.c)
    ESP_RETURN_ON_ERROR(i2s_rx_acquire(), TAG, "i2s rx acquire failed");

    // Restart I2S RX so it picks up data from newly-configured ES7210
    i2s_channel_disable(s_i2s_rx);
    vTaskDelay(pdMS_TO_TICKS(10));
    i2s_channel_enable(s_i2s_rx);

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
    if (gain_db > 37) gain_db = 37;
    s_mic_gain = gain_db;

    if (!s_es7210) {
        return ESP_OK;
    }

    // ES7210 gain: bits[3:0] = gain step, bit4 = PGA enable
    // Steps: 0=0dB, 1=3dB, 2=6dB ... 0x0A=30dB, 0x0E=37.5dB
    uint8_t gain_val = (gain_db / 3);
    if (gain_val > 0x0E) gain_val = 0x0E;
    uint8_t gain_reg = 0x10 | gain_val;  // PGA enable + gain

    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC1_GAIN, gain_reg), TAG, "set gain ch1 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC2_GAIN, gain_reg), TAG, "set gain ch2 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC3_GAIN, gain_reg), TAG, "set gain ch3 failed");
    ESP_RETURN_ON_ERROR(es7210_write(ES7210_MIC4_GAIN, gain_reg), TAG, "set gain ch4 failed");

    ESP_LOGI(TAG, "Mic gain set to %d dB (reg=0x%02X)", gain_db, gain_reg);
    return ESP_OK;
}

void tab5_mic_diag(void)
{
    printf("=== ES7210 Mic Diagnostic ===\n");

    if (!s_es7210) {
        printf("ES7210 not initialized!\n");
        return;
    }

    // Register dump
    es7210_dump_regs();

    if (!s_i2s_rx) {
        printf("I2S RX not available!\n");
        return;
    }

    // Read raw TDM data — 100ms worth = 4800 frames × 4 ch = 19200 samples
    const int frames = 4800;
    const int channels = 4;
    const int total_samples = frames * channels;
    int16_t *buf = heap_caps_malloc(total_samples * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        printf("Alloc failed\n");
        return;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_i2s_rx, buf, total_samples * sizeof(int16_t),
                                      &bytes_read, pdMS_TO_TICKS(2000));
    printf("i2s_channel_read: %s, bytes=%zu/%zu\n",
           esp_err_to_name(err), bytes_read, total_samples * sizeof(int16_t));

    if (err == ESP_OK && bytes_read > 0) {
        int actual_samples = bytes_read / sizeof(int16_t);

        // Per-channel stats
        for (int ch = 0; ch < channels; ch++) {
            int64_t sum = 0;
            int16_t mn = 32767, mx = -32768;
            int nonzero = 0;
            int count = 0;
            for (int i = ch; i < actual_samples; i += channels) {
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
        int show = (actual_samples < 64) ? actual_samples : 64;
        for (int i = 0; i < show; i++) {
            if (i > 0 && i % 4 == 0) printf("| ");
            printf("%04X ", (uint16_t)buf[i]);
        }
        printf("\n");
    }

    heap_caps_free(buf);
    printf("=== End Diagnostic ===\n");
}
