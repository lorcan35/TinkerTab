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
// Audio — Shared I2S bus for ES8388 (DAC) + ES7210 (ADC)
// Full-duplex on I2S_NUM_1: TX and RX both use TDM 4-slot mode so BCLK
// is consistent (48k × 4 × 16 = 3.072 MHz). Previous [MUSIC PLAYING]
// issue was caused by TX=STD (2-slot BCLK=1.536M) vs RX=TDM (4-slot
// BCLK=3.072M) mismatch on the same bus.
// ---------------------------------------------------------------------------
#define TAB5_I2S_NUM          1     // Both TX and RX on I2S_NUM_1
#define TAB5_I2S_MCLK_GPIO   30    // Master clock (shared)
#define TAB5_I2S_BCK_GPIO    27    // Bit clock (shared)
#define TAB5_I2S_WS_GPIO     29    // Word select / LRCK (shared)
#define TAB5_I2S_DOUT_GPIO   26    // Data out -> ES8388 DSIN
#define TAB5_I2S_DIN_GPIO    28    // Data in  <- ES7210 DOUT
#define TAB5_AUDIO_SAMPLE_RATE 48000  // ES7210/ES8388 native rate

// ---------------------------------------------------------------------------
// Camera — SC202CS (SC2356) via MIPI-CSI
// ---------------------------------------------------------------------------
#define TAB5_CAM_XCLK_GPIO      36
#define TAB5_CAM_XCLK_FREQ_HZ   24000000
#define TAB5_CAM_SCCB_ADDR      0x36   // SC202CS at 0x36 (NOT 0x30)
#define TAB5_CAM_CSI_LANES       2

// ---------------------------------------------------------------------------
// IMU — BMI270
// ---------------------------------------------------------------------------
#define TAB5_IMU_I2C_ADDR        0x68

// ---------------------------------------------------------------------------
// RTC — RX8130CE
// ---------------------------------------------------------------------------
#define TAB5_RTC_I2C_ADDR        0x32

// ---------------------------------------------------------------------------
// Battery Monitor — INA226
// ---------------------------------------------------------------------------
#define TAB5_INA226_I2C_ADDR     0x40

// ---------------------------------------------------------------------------
// Audio Codec — ES8388
// ---------------------------------------------------------------------------
#define TAB5_ES8388_I2C_ADDR     0x10

// ---------------------------------------------------------------------------
// Mic ADC — ES7210
// ---------------------------------------------------------------------------
#define TAB5_ES7210_I2C_ADDR     0x40

// ---------------------------------------------------------------------------
// MJPEG / JPEG streaming
// ---------------------------------------------------------------------------
#define TAB5_JPEG_BUF_SIZE    (100 * 1024)
#define TAB5_FRAME_TIMEOUT_MS 5000
#define TAB5_UDP_STREAM_PORT  5000

// ---------------------------------------------------------------------------
// Dragon Link — connection lifecycle
// ---------------------------------------------------------------------------
#define TAB5_DRAGON_HEALTH_PATH        "/health"
#define TAB5_DRAGON_HANDSHAKE_PATH     "/api/handshake"
#define TAB5_DRAGON_HEARTBEAT_MS       5000
#define TAB5_DRAGON_HEALTH_TIMEOUT_MS  3000
#define TAB5_DRAGON_RECONNECT_BASE_MS  2000
#define TAB5_DRAGON_RECONNECT_MAX_MS   30000

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#define TAB5_FIRMWARE_VER     "0.5.0"
#define TAB5_PLATFORM         "esp32p4-tab5"

// ---------------------------------------------------------------------------
// Voice — Dragon voice server
// ---------------------------------------------------------------------------
#define TAB5_VOICE_WS_PATH       "/ws/voice"
#define TAB5_VOICE_PORT           3502

// ngrok fallback for when Dragon is unreachable on LAN
#define TAB5_NGROK_HOST           "tinkerbox.ngrok.dev"
#define TAB5_NGROK_PORT           443
#define TAB5_VOICE_CHUNK_MS       20     // Audio chunk size in ms
#define TAB5_VOICE_SAMPLE_RATE    16000  // Rate sent to Dragon (downsampled from 48kHz)
#define TAB5_VOICE_PLAYBACK_BUF   131072 // Playback ring buffer bytes (~1.4s at 48kHz mono 16-bit, in PSRAM)
