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
#include <stdbool.h>

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

/** TT #328 Wave 8 — Dragon REST API bearer token (separate from the
 *  voice WS auth path).  Tab5 uses this to call gated endpoints like
 *  /api/v1/tools, /api/v1/sessions, /api/v1/memory, etc.  Empty by
 *  default; provisioned by the user via POST /settings.  Tab5 sends
 *  as "Authorization: Bearer <token>" on outbound HTTP requests; if
 *  empty, the request goes out without the header (Dragon will
 *  return 401 for gated routes — Tab5 surfaces that as a graceful
 *  fallback). */
esp_err_t tab5_settings_get_dragon_api_token(char *buf, size_t len);
esp_err_t tab5_settings_set_dragon_api_token(const char *token);

/** TT #328 Wave 11 — starred skills.  Comma-separated list of tool
 *  names that the user has pinned in the Skills catalog.  ui_skills
 *  reads this on render to (a) stamp a ★ glyph on starred cards and
 *  (b) sort starred entries to the top of the list.  Empty by
 *  default; tap toggles individual entries. */
esp_err_t tab5_settings_get_starred_skills(char *buf, size_t len);
esp_err_t tab5_settings_set_starred_skills(const char *list);

/* ── v4·D Sovereign Halo mode dials ──────────────────────────────────── */
/* Three orthogonal dials that replace the 4-mode pill.  Tab5 resolves the
 * triple into the legacy (voice_mode, llm_model) pair Dragon expects.
 * Defaults: 0 / 0 / 0 (fast + local + ask = free tier).
 *   Intelligence (int_tier): 0=fast   1=balanced 2=smart
 *   Voice       (voi_tier) : 0=local  1=neutral  2=studio
 *   Autonomy    (aut_tier) : 0=ask    1=agent                           */
uint8_t   tab5_settings_get_int_tier(void);
uint8_t   tab5_settings_get_voi_tier(void);
uint8_t   tab5_settings_get_aut_tier(void);
esp_err_t tab5_settings_set_int_tier(uint8_t t);
esp_err_t tab5_settings_set_voi_tier(uint8_t t);
esp_err_t tab5_settings_set_aut_tier(uint8_t t);

/* Onboarding gate (audit G / P0 UX).  False on first boot; set true by
 * ui_onboarding once the user finishes the intro carousel. */
bool      tab5_settings_is_onboarded(void);
esp_err_t tab5_settings_set_onboarded(bool done);

/* Resolve (int, voi, aut) -> voice_mode. When out_model is non-NULL and
 * the resolver picks a specific cloud LLM, writes the OpenRouter model
 * id into it (e.g. "anthropic/claude-sonnet-4-20250514"). Otherwise
 * out_model is left untouched. */
uint8_t   tab5_mode_resolve(uint8_t int_tier, uint8_t voi_tier, uint8_t aut_tier,
                            char *out_model, size_t model_len);

/* v4·D Phase 3c daily cloud-spend accumulator.
 * All values in mils (1/1000 USD cent).  Divide by 1000 for cents,
 * 100000 for dollars.  Day rollover is automatic on read/write when the
 * RTC shows a new days-since-epoch value. */
uint32_t  tab5_budget_get_today_mils(void);
uint32_t  tab5_budget_get_cap_mils(void);
esp_err_t tab5_budget_set_cap_mils(uint32_t cap_mils);
esp_err_t tab5_budget_accumulate(uint32_t mils);

/** Zero today's spend.  Dev/debug knob for the /settings POST handler. */
esp_err_t tab5_budget_reset_spent(void);

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

/* ── OTA rollback detection (TT #328 Wave 3 P0 #14) ─────────────────── */

/** Persist the version string we just attempted to OTA-flash.  Compared
 *  against the running TAB5_FIRMWARE_VER on next boot — mismatch means a
 *  rollback fired (image crashed before mark_app_valid_cancel_rollback). */
esp_err_t tab5_settings_get_ota_attempted(char *buf, size_t len);
esp_err_t tab5_settings_set_ota_attempted(const char *version);

/* ── First-launch UI hints (TT #328 Wave 9) ─────────────────────────── */

/** Returns true if the mode-chip discoverability hint has already fired
 *  on this device.  Used by ui_home to gate a single-shot toast that
 *  tells the user the chip is tappable. */
bool tab5_settings_mode_hint_seen(void);
esp_err_t tab5_settings_set_mode_hint_seen(bool seen);

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

/* ── Mic mute ───────────────────────────────────────────────────────── */

/** 0 = mic hot (default), 1 = silenced.  When set, voice_start_listening()
 *  returns ESP_ERR_INVALID_STATE and surfaces a toast on home. */
uint8_t   tab5_settings_get_mic_mute(void);
esp_err_t tab5_settings_set_mic_mute(uint8_t muted);

/* ── Quiet hours (do-not-disturb) ──────────────────────────────────── */

/** 0 = disabled (default), 1 = enabled.  When enabled + the current local
 *  hour is inside [start, end), home dims the orb + sys label shows
 *  'QUIET' and TTS playback is suppressed until quiet ends. */
uint8_t   tab5_settings_get_quiet_on(void);
esp_err_t tab5_settings_set_quiet_on(uint8_t on);

/** Hour-of-day (0..23) for the start / end of quiet hours. Default
 *  start=22 (10 PM), end=7 (7 AM); wraps past midnight correctly. */
uint8_t   tab5_settings_get_quiet_start(void);
esp_err_t tab5_settings_set_quiet_start(uint8_t hour);
uint8_t   tab5_settings_get_quiet_end(void);
esp_err_t tab5_settings_set_quiet_end(uint8_t hour);

/** True if quiet hours are currently active for the given hour (0..23).
 *  Convenience — does the wrap-past-midnight math for you. */
bool      tab5_settings_quiet_active(int hour_local);

/* ── Auto-rotate (audit U2 / TinkerTab #206) ─────────────────────────── */

/** Auto-rotate the display 180° when held upside-down (USB up).  IMU
 *  polled at ~1 Hz from the LVGL thread; toggling persists to NVS so
 *  the preference survives reboot.  0 = disabled (default), 1 = enabled. */
uint8_t   tab5_settings_get_auto_rotate(void);
esp_err_t tab5_settings_set_auto_rotate(uint8_t on);

/** Camera frame rotation, applied to the preview + saved photo. The
 *  SC2336 sensor is fixed-orientation 1280x720 landscape; this knob
 *  rotates the captured frame in software so the user can hold the
 *  device portrait, upside-down, or either landscape and see a
 *  natural viewfinder.  0 = 0deg (no rotation, default), 1 = 90 CW,
 *  2 = 180, 3 = 270 CW (= 90 CCW).  See issue #260. */
uint8_t   tab5_settings_get_cam_rotation(void);
esp_err_t tab5_settings_set_cam_rotation(uint8_t rot);

/* ── Auth token (debug server bearer auth) ───────────────────────────── */

/** Read auth token from NVS. Returns ESP_OK if found and non-empty. */
esp_err_t tab5_settings_get_auth_token(char *buf, size_t len);
esp_err_t tab5_settings_set_auth_token(const char *token);

/* ── NVS write counter (US-HW17) ────────────────────────────────────────── */

/** Return the number of successful nvs_commit() calls this session (wear monitoring). */
uint32_t tab5_settings_get_nvs_write_count(void);
