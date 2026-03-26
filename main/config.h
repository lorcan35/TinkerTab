/**
 * TinkerClaw Tab5 — Configuration
 *
 * M5Stack Tab5 (ESP32-P4) pin definitions, display, and network settings.
 * Override network settings via menuconfig or sdkconfig.defaults.
 */
#pragma once

// ---------------------------------------------------------------------------
// Display — MIPI DSI ST7123 720x1280
// ---------------------------------------------------------------------------
#define TAB5_DISPLAY_WIDTH   720
#define TAB5_DISPLAY_HEIGHT  1280
#define TAB5_LCD_BPP         16   // RGB565 (DPI pixel format)
#define TAB5_LCD_BPP_PANEL   24   // RGB888 (panel dev config)

// MIPI DSI parameters (ST7123)
#define TAB5_DSI_LANE_NUM          2
#define TAB5_DSI_LANE_BITRATE_MBPS 965
#define TAB5_DSI_DPI_CLK_MHZ       70

// MIPI DSI timing (ST7123)
#define TAB5_DSI_HSYNC_WIDTH  2
#define TAB5_DSI_HBP          40
#define TAB5_DSI_HFP          40
#define TAB5_DSI_VSYNC_WIDTH  2
#define TAB5_DSI_VBP          8
#define TAB5_DSI_VFP          220

// MIPI DSI PHY LDO
#define TAB5_DSI_PHY_LDO_CHAN       3
#define TAB5_DSI_PHY_LDO_VOLTAGE_MV 2500

// ---------------------------------------------------------------------------
// I2C — System bus
// ---------------------------------------------------------------------------
#define TAB5_I2C_NUM     0
#define TAB5_I2C_SDA     31
#define TAB5_I2C_SCL     32
#define TAB5_I2C_FREQ_HZ 400000

// ---------------------------------------------------------------------------
// IO Expanders — PI4IOE5V6416
// ---------------------------------------------------------------------------
#define TAB5_PI4IOE1_ADDR 0x43   // LCD reset, touch reset, speaker, ext5v, cam
#define TAB5_PI4IOE2_ADDR 0x44   // WiFi power, USB 5V, charging

// ---------------------------------------------------------------------------
// Backlight — PWM on GPIO 22
// ---------------------------------------------------------------------------
#define TAB5_LCD_BACKLIGHT_GPIO  22
#define TAB5_LCD_BACKLIGHT_FREQ  5000

// ---------------------------------------------------------------------------
// Touch — GT911 on system I2C
// ---------------------------------------------------------------------------
#define TAB5_TOUCH_INT_GPIO  23

// ---------------------------------------------------------------------------
// WiFi — ESP32-C6 via SDIO (ESP-Hosted)
// ---------------------------------------------------------------------------
#define TAB5_SDIO_CLK   12
#define TAB5_SDIO_CMD   13
#define TAB5_SDIO_D0    11
#define TAB5_SDIO_D1    10
#define TAB5_SDIO_D2    9
#define TAB5_SDIO_D3    8
#define TAB5_SDIO_RST   15

// ---------------------------------------------------------------------------
// SD Card — SDMMC
// ---------------------------------------------------------------------------
#define TAB5_SD_CLK  43
#define TAB5_SD_CMD  44
#define TAB5_SD_D0   39
#define TAB5_SD_D1   40
#define TAB5_SD_D2   41
#define TAB5_SD_D3   42

// ---------------------------------------------------------------------------
// Dragon streaming server
// ---------------------------------------------------------------------------
#ifndef TAB5_DRAGON_HOST
#define TAB5_DRAGON_HOST     CONFIG_TAB5_DRAGON_HOST
#endif

#ifndef TAB5_DRAGON_PORT
#define TAB5_DRAGON_PORT     CONFIG_TAB5_DRAGON_PORT
#endif

#define TAB5_STREAM_PATH     "/stream"
#define TAB5_TOUCH_WS_PATH   "/ws/touch"

// ---------------------------------------------------------------------------
// WiFi credentials
// ---------------------------------------------------------------------------
#ifndef TAB5_WIFI_SSID
#define TAB5_WIFI_SSID       CONFIG_TAB5_WIFI_SSID
#endif

#ifndef TAB5_WIFI_PASS
#define TAB5_WIFI_PASS       CONFIG_TAB5_WIFI_PASS
#endif

// ---------------------------------------------------------------------------
// MJPEG
// ---------------------------------------------------------------------------
#define TAB5_JPEG_BUF_SIZE    (100 * 1024)
#define TAB5_FRAME_TIMEOUT_MS 5000
