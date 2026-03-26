/**
 * TinkerClaw Tab5 — MIPI DSI Display Driver (ST7123)
 *
 * Drives the 720x1280 ST7123 panel via 2-lane MIPI DSI on ESP32-P4.
 * Based on M5Stack Tab5 BSP display initialization.
 *
 * Init sequence:
 *   1. Backlight PWM setup (GPIO 22)
 *   2. MIPI DSI PHY power via LDO channel 3
 *   3. Create DSI bus (2 lanes, 965 Mbps)
 *   4. Create DBI IO for commands
 *   5. Create ST7123 panel with vendor init data
 *   6. Reset, init, turn on
 */

#include "display.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7123.h"
#include "esp_ldo_regulator.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cache.h"
#include "driver/jpeg_decode.h"

static const char *TAG = "tab5_display";

static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_initialized = false;
static jpeg_decoder_handle_t s_jpeg_decoder = NULL;

#define LCD_LEDC_CH LEDC_CHANNEL_1

// ---------------------------------------------------------------------------
// ST7123 vendor init commands — from M5Stack BSP
// ---------------------------------------------------------------------------
static const st7123_lcd_init_cmd_t s_st7123_init_cmds[] = {
    {0x60, (uint8_t[]){0x71, 0x23, 0xa2}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa3}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa4}, 3, 0},
    {0xA4, (uint8_t[]){0x31}, 1, 0},
    {0xD7, (uint8_t[]){0x10, 0x0A, 0x10, 0x2A, 0x80, 0x80}, 6, 0},
    {0x90, (uint8_t[]){0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09}, 7, 0},
    {0xA3, (uint8_t[]){0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00,
                       0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x4F, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
                       0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x6F, 0x58, 0x00, 0x00, 0x00, 0xFF},
     40, 0},
    {0xA6, (uint8_t[]){0x03, 0x00, 0x24, 0x55, 0x36, 0x00, 0x39, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24,
                       0x55, 0x38, 0x00, 0x37, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x11, 0x00, 0x00,
                       0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0xEC, 0x11, 0x00, 0x03, 0x00, 0x03, 0x6E,
                       0x6E, 0xFF, 0xFF, 0x00, 0x08, 0x80, 0x08, 0x80, 0x06, 0x00, 0x00, 0x00, 0x00},
     55, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x80, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x44, 0x03, 0x6E, 0x6E, 0x91, 0xFF,
                       0x08, 0x80, 0x64, 0x40, 0x25, 0x34, 0x40, 0x00, 0x02, 0x01, 0x6E, 0x6E, 0x91, 0xFF, 0x08,
                       0x80, 0x64, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80,
                       0x64, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x6E, 0x6E, 0x84, 0xFF, 0x08, 0x80, 0x44},
     60, 0},
    {0xAC, (uint8_t[]){0x03, 0x19, 0x19, 0x18, 0x18, 0x06, 0x13, 0x13, 0x11, 0x11, 0x08, 0x08, 0x0A, 0x0A, 0x1C,
                       0x1C, 0x07, 0x07, 0x00, 0x00, 0x02, 0x02, 0x01, 0x19, 0x19, 0x18, 0x18, 0x06, 0x12, 0x12,
                       0x10, 0x10, 0x09, 0x09, 0x0B, 0x0B, 0x1C, 0x1C, 0x07, 0x07, 0x03, 0x03, 0x01, 0x01},
     44, 0},
    {0xAD, (uint8_t[]){0xF0, 0x00, 0x46, 0x00, 0x03, 0x50, 0x50, 0xFF, 0xFF, 0xF0, 0x40, 0x06, 0x01,
                       0x07, 0x42, 0x42, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
     25, 0},
    {0xAE, (uint8_t[]){0xFE, 0x3F, 0x3F, 0xFE, 0x3F, 0x3F, 0x00}, 7, 0},
    {0xB2, (uint8_t[]){0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E, 0x5C, 0xD2, 0xFF, 0x10, 0x20, 0xFD, 0x20, 0xC0, 0x00},
     17, 0},
    {0xE8, (uint8_t[]){0x20, 0x6F, 0x04, 0x97, 0x97, 0x3E, 0x04, 0xDC, 0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E}, 15, 0},
    {0x75, (uint8_t[]){0x03, 0x04}, 2, 0},
    {0xE7, (uint8_t[]){0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A, 0xF0, 0xB1, 0x50, 0x00,
                       0x50, 0xB1, 0x50, 0xB1, 0x50, 0xD8, 0x00, 0x55, 0x00, 0xB1, 0x00, 0x45,
                       0xC9, 0x6A, 0xFF, 0x5A, 0xD8, 0x18, 0x88, 0x15, 0xB1, 0x01, 0x01, 0x77},
     36, 0},
    {0xEA, (uint8_t[]){0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C}, 8, 0},
    {0xB0, (uint8_t[]){0x22, 0x43, 0x11, 0x61, 0x25, 0x43, 0x43}, 7, 0},
    {0xb7, (uint8_t[]){0x00, 0x00, 0x73, 0x73}, 0x04, 0},
    {0xBF, (uint8_t[]){0xA6, 0xAA}, 2, 0},
    {0xA9, (uint8_t[]){0x00, 0x00, 0x73, 0xFF, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03}, 10, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0xC9, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    // Exact BSP order — only change is 120ms SLPOUT delay (GitHub #18083 fix)
    {0x36, (uint8_t[]){0x00}, 1, 0},    // MADCTL
    {0x11, (uint8_t[]){0x00}, 1, 120},  // SLPOUT — 120ms delay (was 100ms in BSP)
    {0x29, (uint8_t[]){0x00}, 1, 0},    // DISPON
    {0x35, (uint8_t[]){0x00}, 1, 100},  // TE on
};

// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------
static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = TAB5_LCD_BACKLIGHT_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "Backlight timer config failed");

    const ledc_channel_config_t ch_cfg = {
        .gpio_num   = TAB5_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LCD_LEDC_CH,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "Backlight channel config failed");

    return ESP_OK;
}

esp_err_t tab5_display_set_brightness(int percent)
{
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    uint32_t duty = (4095 * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty), TAG, "Set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH), TAG, "Update duty failed");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// MIPI DSI PHY power
// ---------------------------------------------------------------------------
static esp_err_t enable_dsi_phy_power(void)
{
    static esp_ldo_channel_handle_t phy_pwr = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id     = TAB5_DSI_PHY_LDO_CHAN,
        .voltage_mv = TAB5_DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr), TAG, "LDO acquire failed");
    ESP_LOGI(TAG, "MIPI DSI PHY powered on (LDO ch%d, %dmV)", TAB5_DSI_PHY_LDO_CHAN, TAB5_DSI_PHY_LDO_VOLTAGE_MV);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Display init
// ---------------------------------------------------------------------------
esp_err_t tab5_display_init(void)
{
    ESP_LOGI(TAG, "Initializing %dx%d MIPI DSI display (ST7123)", TAB5_DISPLAY_WIDTH, TAB5_DISPLAY_HEIGHT);

    // Step 1: Backlight PWM
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "Backlight init failed");

    // Step 2: MIPI DSI PHY power
    ESP_RETURN_ON_ERROR(enable_dsi_phy_power(), TAG, "DSI PHY power failed");

    // Step 3: Create MIPI DSI bus
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = TAB5_DSI_LANE_NUM,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = TAB5_DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus), TAG, "DSI bus create failed");
    ESP_LOGI(TAG, "DSI bus created: %d lanes @ %d Mbps", TAB5_DSI_LANE_NUM, TAB5_DSI_LANE_BITRATE_MBPS);

    // Step 4: DBI panel IO for commands
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_panel_io), TAG, "DBI IO create failed");

    // Step 5: DPI panel config (video mode) — ST7123 timing
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = TAB5_DSI_DPI_CLK_MHZ,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = TAB5_DISPLAY_WIDTH,
            .v_size            = TAB5_DISPLAY_HEIGHT,
            .hsync_back_porch  = TAB5_DSI_HBP,
            .hsync_pulse_width = TAB5_DSI_HSYNC_WIDTH,
            .hsync_front_porch = TAB5_DSI_HFP,
            .vsync_back_porch  = TAB5_DSI_VBP,
            .vsync_pulse_width = TAB5_DSI_VSYNC_WIDTH,
            .vsync_front_porch = TAB5_DSI_VFP,
        },
        .flags.use_dma2d = true,
    };

    // Step 6: ST7123 panel with vendor init commands
    st7123_vendor_config_t vendor_cfg = {
        .init_cmds      = s_st7123_init_cmds,
        .init_cmds_size = sizeof(s_st7123_init_cmds) / sizeof(s_st7123_init_cmds[0]),
        .mipi_config = {
            .dsi_bus    = s_dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = TAB5_DSI_LANE_NUM,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 24,  // BSP uses 24 (RGB888 for panel dev config)
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE,
        .reset_gpio_num = -1,  // Reset via IO expander
        .vendor_config  = &vendor_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7123(s_panel_io, &panel_cfg, &s_panel), TAG, "ST7123 panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "Display on failed");

    // Let panel stabilize before turning on backlight
    vTaskDelay(pdMS_TO_TICKS(200));

    // Turn on backlight
    ESP_RETURN_ON_ERROR(tab5_display_set_brightness(80), TAG, "Backlight on failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Display initialized: %dx%d MIPI DSI ST7123", TAB5_DISPLAY_WIDTH, TAB5_DISPLAY_HEIGHT);
    return ESP_OK;
}

void tab5_display_fill_color(uint16_t color_rgb565)
{
    if (!s_initialized || !s_panel) return;

    // Get direct access to the DPI panel's framebuffer
    void *fb = NULL;
    esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb);
    if (ret == ESP_OK && fb) {
        // Write directly to the DPI framebuffer
        uint16_t *pixels = (uint16_t *)fb;
        size_t fb_size = TAB5_DISPLAY_WIDTH * TAB5_DISPLAY_HEIGHT * sizeof(uint16_t);
        for (int i = 0; i < TAB5_DISPLAY_WIDTH * TAB5_DISPLAY_HEIGHT; i++) {
            pixels[i] = color_rgb565;
        }
        // Flush CPU cache → PSRAM so DPI DMA sees the updated pixels
        esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        ESP_LOGI(TAG, "Filled DPI framebuffer with color 0x%04X (direct+cache_flush)", color_rgb565);
    } else {
        // Fallback: allocate and use draw_bitmap
        ESP_LOGW(TAG, "Direct FB access failed (%s), using draw_bitmap", esp_err_to_name(ret));
        size_t fb_size = TAB5_DISPLAY_WIDTH * TAB5_DISPLAY_HEIGHT * sizeof(uint16_t);
        uint16_t *buf = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate fill buffer");
            return;
        }
        for (int i = 0; i < TAB5_DISPLAY_WIDTH * TAB5_DISPLAY_HEIGHT; i++) {
            buf[i] = color_rgb565;
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, TAB5_DISPLAY_WIDTH, TAB5_DISPLAY_HEIGHT, buf);
        free(buf);
        ESP_LOGI(TAG, "Filled display with color 0x%04X (draw_bitmap)", color_rgb565);
    }
}

void tab5_display_show_status(const char *msg)
{
    ESP_LOGI(TAG, "Status: %s", msg);
}

void tab5_display_test_pattern(int type)
{
    if (!s_panel) {
        ESP_LOGE(TAG, "Panel not initialized");
        return;
    }
    mipi_dsi_pattern_type_t pat;
    switch (type) {
        case 1: pat = MIPI_DSI_PATTERN_BAR_VERTICAL; break;
        case 2: pat = MIPI_DSI_PATTERN_BAR_HORIZONTAL; break;
        case 3: pat = MIPI_DSI_PATTERN_BER_VERTICAL; break;
        default: pat = MIPI_DSI_PATTERN_NONE; break;
    }
    esp_err_t ret = esp_lcd_dpi_panel_set_pattern(s_panel, pat);
    ESP_LOGI(TAG, "Test pattern %d: %s", type, esp_err_to_name(ret));
}

// ---------------------------------------------------------------------------
// Hardware JPEG decoder
// ---------------------------------------------------------------------------
esp_err_t tab5_display_jpeg_init(void)
{
    jpeg_decode_engine_cfg_t cfg = {
        .timeout_ms = 100,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&cfg, &s_jpeg_decoder), TAG, "JPEG decoder init failed");
    ESP_LOGI(TAG, "Hardware JPEG decoder initialized");
    return ESP_OK;
}

esp_err_t tab5_display_draw_jpeg(const uint8_t *jpeg_data, uint32_t jpeg_size)
{
    if (!s_initialized || !s_panel || !s_jpeg_decoder) return ESP_ERR_INVALID_STATE;

    // Get DPI framebuffer — decode directly into it
    void *fb = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb),
        TAG, "Get framebuffer failed");

    size_t fb_size = TAB5_DISPLAY_WIDTH * TAB5_DISPLAY_HEIGHT * sizeof(uint16_t);

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t out_size = 0;
    esp_err_t ret = jpeg_decoder_process(
        s_jpeg_decoder,
        &decode_cfg,
        jpeg_data,
        jpeg_size,
        (uint8_t *)fb,
        fb_size,
        &out_size);

    if (ret != ESP_OK) return ret;

    // Flush CPU cache → PSRAM so DPI DMA sees the decoded pixels
    esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return ESP_OK;
}
