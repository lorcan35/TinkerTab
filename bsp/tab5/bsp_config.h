/**
 * TinkerTab BSP — Tab5 Hardware Configuration
 *
 * M5Stack Tab5 (ESP32-P4) pin definitions, I2C addresses, and hardware
 * timing parameters.  This file belongs to the BSP component and should
 * contain ONLY hardware-specific constants (pins, addresses, timings).
 *
 * Application-level settings (fonts, network paths, firmware version,
 * voice modes, OTA) remain in main/config.h.
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
#define TAB5_INA226_I2C_ADDR     0x41   // M5Stack routes A0 high to avoid ES7210 clash at 0x40

// ---------------------------------------------------------------------------
// Audio Codec — ES8388
// ---------------------------------------------------------------------------
#define TAB5_ES8388_I2C_ADDR     0x10

// ---------------------------------------------------------------------------
// Mic ADC — ES7210
// ---------------------------------------------------------------------------
#define TAB5_ES7210_I2C_ADDR     0x40

// ---------------------------------------------------------------------------
// Chat UI — pool and history sizing
// ---------------------------------------------------------------------------
#define BSP_CHAT_MAX_MESSAGES   100   /* Per-mode message history depth */
#define BSP_CHAT_POOL_SIZE      12    /* Visible recycled message slots */

// ---------------------------------------------------------------------------
// Display DPI — for portable touch target sizing
// ---------------------------------------------------------------------------
#define BSP_DISPLAY_DPI         218   /* 720px / 3.3 inches */
