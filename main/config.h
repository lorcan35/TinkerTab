/**
 * TinkerClaw Tab5 — Configuration
 *
 * Application-level settings: network, voice, OTA, fonts, firmware identity.
 * Hardware pin definitions live in bsp/tab5/bsp_config.h (BSP component).
 * Override network settings via menuconfig or sdkconfig.defaults.
 */
#pragma once

#include "bsp_config.h"  /* Hardware pin definitions from BSP */

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
#define TAB5_FIRMWARE_VER     "0.8.0"

// Voice modes (matches voice_mode in config_update protocol)
#define VOICE_MODE_LOCAL       0
#define VOICE_MODE_HYBRID      1
#define VOICE_MODE_CLOUD       2
#define VOICE_MODE_TINKERCLAW  3
#define VOICE_MODE_COUNT       4
#define TAB5_PLATFORM         "esp32p4-tab5"

// ---------------------------------------------------------------------------
// OTA — Over-the-air firmware updates via Dragon
// ---------------------------------------------------------------------------
#define TAB5_OTA_CHECK_PATH       "/api/ota/check"
#define TAB5_OTA_CHECK_INTERVAL_MS 3600000  /* 1 hour between auto-checks */
#define TAB5_OTA_PORT             3502      /* same as voice server */

// ---------------------------------------------------------------------------
// Voice — Dragon voice server
// ---------------------------------------------------------------------------
#define TAB5_VOICE_WS_PATH       "/ws/voice"
#define TAB5_VOICE_PORT           3502

// ngrok fallback for when Dragon is unreachable on LAN
#define TAB5_NGROK_HOST           "tinkerclaw-voice.ngrok.dev"
#define TAB5_NGROK_PORT           443
#define TAB5_VOICE_CHUNK_MS       20     // Audio chunk size in ms
#define TAB5_VOICE_SAMPLE_RATE    16000  // Rate sent to Dragon (downsampled from 48kHz)
#define TAB5_VOICE_PLAYBACK_BUF   131072 // Playback ring buffer bytes (~1.4s at 48kHz mono 16-bit, in PSRAM)

// ---------------------------------------------------------------------------
// Global Typography Scale (720x1280 display @ arm's length)
// ---------------------------------------------------------------------------
#define FONT_TITLE       &lv_font_montserrat_28   /* Screen titles, big numbers */
#define FONT_HEADING     &lv_font_montserrat_24   /* Section headers, card titles */
#define FONT_BODY        &lv_font_montserrat_20   /* Primary body text, messages, labels */
#define FONT_SECONDARY   &lv_font_montserrat_18   /* Secondary text, descriptions */
#define FONT_CAPTION     &lv_font_montserrat_16   /* Timestamps, badges, metadata */
#define FONT_SMALL       &lv_font_montserrat_14   /* Hints, placeholders, debug */
#define FONT_CLOCK       &lv_font_montserrat_48   /* Home screen clock */
#define FONT_DATE        &lv_font_montserrat_24   /* Home screen date */
#define FONT_KEY         &lv_font_montserrat_20   /* Keyboard keys */
#define FONT_NAV         &lv_font_montserrat_18   /* Nav bar labels */
