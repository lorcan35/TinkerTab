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
// Dragon — voice WS server
// ---------------------------------------------------------------------------
#ifndef TAB5_DRAGON_HOST
#define TAB5_DRAGON_HOST     CONFIG_TAB5_DRAGON_HOST
#endif

#ifndef TAB5_DRAGON_PORT
#define TAB5_DRAGON_PORT     CONFIG_TAB5_DRAGON_PORT
#endif

/* Wave 14 W14-C04: Dragon API bearer token, sent as `Authorization: Bearer`
 * on the /ws/voice upgrade request.  Must match server.api_token on Dragon
 * (populated from DRAGON_API_TOKEN in /home/radxa/.env).  Default is the
 * placeholder string so an un-provisioned Tab5 makes the failure obvious —
 * real tokens belong in sdkconfig.local. */
#ifndef TAB5_DRAGON_TOKEN
#define TAB5_DRAGON_TOKEN    CONFIG_TAB5_DRAGON_TOKEN
#endif

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

// ── v4·C chat fonts (Fraunces italic + JetBrains Mono) ───────────────
#ifdef LV_FONT_DECLARE
LV_FONT_DECLARE(fraunces_italic_32);
LV_FONT_DECLARE(fraunces_italic_22);
LV_FONT_DECLARE(jetbrains_mono_medium_14);
/* closes #139: FONT_CHAT_TITLE was fraunces_italic_32.  Coredump
 * captured in draw_letter for letter 'C' while painting the chat
 * header title ('Chat' / 'Conversations' — both start with 'C').
 * Intermittent, reproducible under long sessions.  Swap to the
 * well-tested Montserrat Bold 28 (FONT_TITLE) for stability.
 * fraunces stays available for inline emphasis via FONT_CHAT_EMPH. */
#define FONT_CHAT_TITLE  FONT_TITLE              /* was &fraunces_italic_32 */
#define FONT_CHAT_EMPH   (&fraunces_italic_22)   /* AI bubble <em> emphasis */
#define FONT_CHAT_MONO   (&jetbrains_mono_medium_14)  /* kickers, timestamps, chip sub */
#endif

// ---------------------------------------------------------------------------
// DPI-Aware Sizing — scales pixel values for different display densities
// Base: 160 DPI (standard). Tab5 at 218 DPI scales up ~36%.
// Usage: DPI_SCALE(44) = 60px on Tab5 (7mm touch target)
// ---------------------------------------------------------------------------
#define DPI_SCALE(px)   ((px) * BSP_DISPLAY_DPI / 160)
#define TOUCH_MIN       DPI_SCALE(44)   /* Minimum touch target (7mm at 160 DPI) */
