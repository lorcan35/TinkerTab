/**
 * TinkerTab — NVS-backed persistent settings
 *
 * All getters try NVS first, falling back to compile-time defaults from
 * config.h / sdkconfig.  All setters write to NVS immediately.
 *
 * Call tab5_settings_init() once after nvs_flash_init() and before WiFi.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * Open the "settings" NVS namespace.  Safe to call repeatedly — only
 * opens once.  Returns ESP_OK on success.
 */
esp_err_t tab5_settings_init(void);

/* ── WiFi ─────────────────────────────────────────────────────────────── */

esp_err_t tab5_settings_get_wifi_ssid(char *buf, size_t len);
esp_err_t tab5_settings_set_wifi_ssid(const char *ssid);

esp_err_t tab5_settings_get_wifi_pass(char *buf, size_t len);
esp_err_t tab5_settings_set_wifi_pass(const char *pass);

/* ── Dragon host/port ─────────────────────────────────────────────────── */

esp_err_t tab5_settings_get_dragon_host(char *buf, size_t len);
esp_err_t tab5_settings_set_dragon_host(const char *host);

uint16_t  tab5_settings_get_dragon_port(void);
esp_err_t tab5_settings_set_dragon_port(uint16_t port);

/* ── Display ──────────────────────────────────────────────────────────── */

/** Returns 0-100 (default 80). */
uint8_t   tab5_settings_get_brightness(void);
esp_err_t tab5_settings_set_brightness(uint8_t pct);

/* ── Audio ────────────────────────────────────────────────────────────── */

/** Returns 0-100 (default 70). */
uint8_t   tab5_settings_get_volume(void);
esp_err_t tab5_settings_set_volume(uint8_t vol);

/* ── Device identity ─────────────────────────────────────────────────── */

/**
 * Get persistent device_id. Generated once from MAC on first boot.
 * Format: lowercase hex MAC without colons (e.g. "aabbccddeeff").
 */
esp_err_t tab5_settings_get_device_id(char *buf, size_t len);

/** Get hardware MAC address as "AA:BB:CC:DD:EE:FF". */
esp_err_t tab5_settings_get_hardware_id(char *buf, size_t len);

/* ── Session persistence ─────────────────────────────────────────────── */

/** Get last session_id (empty string if none). */
esp_err_t tab5_settings_get_session_id(char *buf, size_t len);
esp_err_t tab5_settings_set_session_id(const char *session_id);

/* ── Voice mode (three-tier) ──────────────────────────────────────────── */

/** 0 = local (default), 1 = hybrid (cloud STT+TTS), 2 = full cloud */
uint8_t   tab5_settings_get_voice_mode(void);
esp_err_t tab5_settings_set_voice_mode(uint8_t mode);

/** Backward compat alias */
#define tab5_settings_get_cloud_mode() tab5_settings_get_voice_mode()
#define tab5_settings_set_cloud_mode(v) tab5_settings_set_voice_mode(v)

/** Cloud LLM model ID (e.g. "anthropic/claude-3-haiku"). Empty = default. */
esp_err_t tab5_settings_get_llm_model(char *buf, size_t len);
esp_err_t tab5_settings_set_llm_model(const char *model);

/* ── Connection mode ────────────────────────────────────────────────── */

/** 0 = auto (ngrok first, LAN fallback — default)
 *  1 = local only (LAN direct, no ngrok)
 *  2 = remote only (ngrok only, no LAN fallback) */
uint8_t   tab5_settings_get_connection_mode(void);
esp_err_t tab5_settings_set_connection_mode(uint8_t mode);

/* ── Wake word ──────────────────────────────────────────────────────── */

/** 0 = PTT only (default), 1 = always-listening with wake word */
uint8_t   tab5_settings_get_wake_word(void);
esp_err_t tab5_settings_set_wake_word(uint8_t enabled);

/* ── Auth token (debug server bearer auth) ───────────────────────────── */

/** Read auth token from NVS. Returns ESP_OK if found and non-empty. */
esp_err_t tab5_settings_get_auth_token(char *buf, size_t len);
esp_err_t tab5_settings_set_auth_token(const char *token);
