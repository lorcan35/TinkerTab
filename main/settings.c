/**
 * TinkerTab — NVS-backed persistent settings
 *
 * Each getter reads from NVS; if the key is missing it returns the
 * compile-time default from config.h / sdkconfig so first boot behaves
 * identically to the previous hard-coded approach.
 *
 * Each setter writes to NVS immediately (nvs_commit after every write).
 */

#include "settings.h"
#include "config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "settings";

/* NVS namespace — max 15 chars */
#define NVS_NS "settings"

/* NVS keys — max 15 chars each */
#define KEY_WIFI_SSID   "wifi_ssid"
#define KEY_WIFI_PASS   "wifi_pass"
#define KEY_DRAGON_HOST "dragon_host"
#define KEY_DRAGON_PORT "dragon_port"
/* TT #328 Wave 8 — Dragon REST API bearer token (separate from voice WS
 * auth which is a different mechanism).  Required for Tab5 to call
 * gated endpoints like /api/v1/tools, /api/v1/sessions, etc.  Empty by
 * default; user provisions via POST /settings or Settings UI.
 * NVS key length cap is 15 chars total — "dragon_tok" leaves room. */
#define KEY_DRAGON_TOK "dragon_tok"
/* TT #328 Wave 11 — comma-separated list of starred (pinned) skill
 * names.  Empty by default.  Used by ui_skills to sort starred tools
 * to the top of the catalog and stamp a star glyph on the card.
 * Single-string format keeps the NVS schema small (no per-skill keys
 * to enumerate).  Cap at ~256 chars: tool names are typically 8-16
 * chars, so 256 holds 16-32 stars — well above any sane user need. */
#define KEY_STAR_SKILLS "star_skills"
/* TT #370 — OpenRouter (vmode=5 SOLO_DIRECT) NVS keys.  All ≤15 chars.
 * Empty `or_key` means solo mode is unconfigured. */
#define KEY_OR_KEY     "or_key"
#define KEY_OR_MDL_LLM "or_mdl_llm"
#define KEY_OR_MDL_STT "or_mdl_stt"
#define KEY_OR_MDL_TTS "or_mdl_tts"
#define KEY_OR_MDL_EMB "or_mdl_emb"
#define KEY_OR_VOICE   "or_voice"
#define KEY_BRIGHTNESS  "brightness"
#define KEY_VOLUME      "volume"
#define KEY_DEVICE_ID   "device_id"
#define KEY_SESSION_ID  "session_id"

/* Compile-time defaults */
#define DEFAULT_BRIGHTNESS  80
#define DEFAULT_VOLUME      70

static nvs_handle_t s_nvs = 0;
static bool         s_inited = false;

/* v4·D audit P0 fix: serialize every NVS call.
 * tab5_budget_accumulate runs on the WS rx task, tab5_settings_set_* is
 * called from the LVGL thread, and the heap watchdog on Core 1 reads
 * counters too.  The nvs_handle_t is not thread-safe -- concurrent
 * nvs_set_u32 + nvs_commit on the same handle is UB and has been
 * observed producing ESP_ERR_NVS_INVALID_HANDLE under stress.  A
 * plain FreeRTOS mutex is enough; NVS ops are short. */
static SemaphoreHandle_t s_nvs_mutex = NULL;
/* Wave 13 H10: bounded NVS mutex timeout.
 *
 * Previously NVS_LOCK() used portMAX_DELAY. A single long-running NVS write
 * (observed up to ~800 ms on a freshly-erased partition after corruption)
 * blocked the heap watchdog task (Core 1) when it tried to read the
 * nvs_write counter, and since the watchdog's own WDT is 5s, the sequence
 * LVGL-write + Core-1 read could have tipped the device over.  2000 ms is
 * 2.5x the worst observed write, so contended callers get a clear error
 * back instead of silently stalling.  Returns true if the lock was taken,
 * false on timeout. */
#define NVS_LOCK_TIMEOUT_MS 2000
static inline bool nvs_try_lock(uint32_t timeout_ms)
{
    if (!s_nvs_mutex) return true;  /* pre-init: no other tasks yet */
    return xSemaphoreTake(s_nvs_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
#define NVS_LOCK()    nvs_try_lock(NVS_LOCK_TIMEOUT_MS)
#define NVS_UNLOCK()  do { if (s_nvs_mutex) xSemaphoreGive(s_nvs_mutex); } while (0)

/* Forward declarations for init-time seeding */
static esp_err_t get_str(const char *key, char *buf, size_t len, const char *def);
static esp_err_t set_str(const char *key, const char *val);

/* US-HW17: NVS write counter — incremented on every nvs_commit() for wear monitoring.
 * Atomic type not needed: all NVS writes go through set_str/set_u8/set_u16 which
 * are only called from the LVGL task (Core 0) or settings init. The counter is
 * read from the heap watchdog task (Core 1) but a torn read of a uint32_t on
 * ESP32-P4 (32-bit aligned) is not possible — single-instruction load. */
static uint32_t s_nvs_write_count = 0;

/* ── Init ─────────────────────────────────────────────────────────────── */

esp_err_t tab5_settings_init(void)
{
    if (s_inited) return ESP_OK;

    if (!s_nvs_mutex) {
        s_nvs_mutex = xSemaphoreCreateMutex();
        if (!s_nvs_mutex) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NS, esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "NVS namespace '%s' opened", NVS_NS);

    /* Auto-seed WiFi credentials from compile-time defaults if NVS is empty.
     * This ensures WiFi works after NVS erase even when sdkconfig.defaults
     * has placeholder values (SEC02 — creds not committed to public git). */
    {
        char ssid[64] = {0};
        get_str(KEY_WIFI_SSID, ssid, sizeof(ssid), "");
        if (ssid[0] == '\0' && strlen(TAB5_WIFI_SSID) > 0
            && strcmp(TAB5_WIFI_SSID, "YOUR_WIFI_SSID") != 0) {
            ESP_LOGI(TAG, "Seeding WiFi creds from compile-time defaults");
            set_str(KEY_WIFI_SSID, TAB5_WIFI_SSID);
            set_str(KEY_WIFI_PASS, TAB5_WIFI_PASS);
        }
    }

    return ESP_OK;
}

/* ── Internal helpers ─────────────────────────────────────────────────── */

/**
 * Read a string from NVS.  On any failure (including key-not-found)
 * copy `def` into buf and return ESP_ERR_NVS_NOT_FOUND.
 */
static esp_err_t get_str(const char *key, char *buf, size_t len, const char *def)
{
    if (!s_inited || !buf || len == 0) {
        if (buf && len > 0) {
            strncpy(buf, def, len);
            buf[len - 1] = '\0';
        }
        return ESP_ERR_INVALID_STATE;
    }

    size_t required = len;
    if (!NVS_LOCK()) {
        ESP_LOGW(TAG, "nvs_get_str(%s): mutex timeout, using default", key);
        strncpy(buf, def, len); buf[len - 1] = '\0';
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_get_str(s_nvs, key, buf, &required);
    NVS_UNLOCK();
    if (err != ESP_OK) {
        /* Key not found or buffer too small — use default */
        strncpy(buf, def, len);
        buf[len - 1] = '\0';
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "'%s' not in NVS, using default", key);
        } else {
            ESP_LOGW(TAG, "nvs_get_str(%s): %s, using default", key, esp_err_to_name(err));
        }
    }
    return err;
}

static esp_err_t set_str(const char *key, const char *val)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    if (!NVS_LOCK()) {
        ESP_LOGE(TAG, "nvs_set_str(%s): mutex timeout", key);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_str(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
        if (err == ESP_OK) s_nvs_write_count++;
    }
    NVS_UNLOCK();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static uint8_t get_u8(const char *key, uint8_t def)
{
    if (!s_inited) return def;

    uint8_t val = def;
    if (!NVS_LOCK()) {
        ESP_LOGW(TAG, "nvs_get_u8(%s): mutex timeout, using default %d", key, def);
        return def;
    }
    esp_err_t err = nvs_get_u8(s_nvs, key, &val);
    NVS_UNLOCK();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "'%s' not in NVS, using default %d", key, def);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u8(%s): %s, using default", key, esp_err_to_name(err));
    }
    return val;
}

static esp_err_t set_u8(const char *key, uint8_t val)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    if (!NVS_LOCK()) {
        ESP_LOGE(TAG, "nvs_set_u8(%s): mutex timeout", key);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_u8(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
        if (err == ESP_OK) s_nvs_write_count++;
    }
    NVS_UNLOCK();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static uint16_t get_u16(const char *key, uint16_t def)
{
    if (!s_inited) return def;

    uint16_t val = def;
    if (!NVS_LOCK()) {
        ESP_LOGW(TAG, "nvs_get_u16(%s): mutex timeout, using default %d", key, def);
        return def;
    }
    esp_err_t err = nvs_get_u16(s_nvs, key, &val);
    NVS_UNLOCK();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "'%s' not in NVS, using default %d", key, def);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u16(%s): %s, using default", key, esp_err_to_name(err));
    }
    return val;
}

static esp_err_t set_u16(const char *key, uint16_t val)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    if (!NVS_LOCK()) {
        ESP_LOGE(TAG, "nvs_set_u16(%s): mutex timeout", key);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_u16(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
        if (err == ESP_OK) s_nvs_write_count++;
    }
    NVS_UNLOCK();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u16(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static uint32_t get_u32(const char *key, uint32_t def)
{
    if (!s_inited) return def;
    uint32_t val = def;
    if (!NVS_LOCK()) {
        ESP_LOGW(TAG, "nvs_get_u32(%s): mutex timeout, using default %lu", key, (unsigned long)def);
        return def;
    }
    esp_err_t err = nvs_get_u32(s_nvs, key, &val);
    NVS_UNLOCK();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "'%s' not in NVS, using default %lu", key, (unsigned long)def);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u32(%s): %s, using default", key, esp_err_to_name(err));
    }
    return val;
}

static esp_err_t set_u32(const char *key, uint32_t val)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    /* Wave 13 H10: set_u32 was missing the NVS mutex entirely while every
     * other setter had it -- races with set_u8 et al. on the same handle
     * were possible.  Lock it, with the same bounded timeout. */
    if (!NVS_LOCK()) {
        ESP_LOGE(TAG, "nvs_set_u32(%s): mutex timeout", key);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_u32(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
        if (err == ESP_OK) s_nvs_write_count++;
    }
    NVS_UNLOCK();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

/* ── WiFi ─────────────────────────────────────────────────────────────── */

esp_err_t tab5_settings_get_wifi_ssid(char *buf, size_t len)
{
    return get_str(KEY_WIFI_SSID, buf, len, TAB5_WIFI_SSID);
}

esp_err_t tab5_settings_set_wifi_ssid(const char *ssid)
{
    return set_str(KEY_WIFI_SSID, ssid);
}

esp_err_t tab5_settings_get_wifi_pass(char *buf, size_t len)
{
    return get_str(KEY_WIFI_PASS, buf, len, TAB5_WIFI_PASS);
}

esp_err_t tab5_settings_set_wifi_pass(const char *pass)
{
    return set_str(KEY_WIFI_PASS, pass);
}

/* ── Dragon ───────────────────────────────────────────────────────────── */

esp_err_t tab5_settings_get_dragon_host(char *buf, size_t len)
{
    return get_str(KEY_DRAGON_HOST, buf, len, TAB5_DRAGON_HOST);
}

esp_err_t tab5_settings_set_dragon_host(const char *host)
{
    return set_str(KEY_DRAGON_HOST, host);
}

uint16_t tab5_settings_get_dragon_port(void)
{
    return get_u16(KEY_DRAGON_PORT, (uint16_t)TAB5_DRAGON_PORT);
}

esp_err_t tab5_settings_set_dragon_port(uint16_t port)
{
    return set_u16(KEY_DRAGON_PORT, port);
}

/* TT #328 Wave 8 — Dragon REST API bearer token.  Default empty so
 * shipping firmware doesn't carry a token; user provisions via
 * POST /settings or Settings UI.  Anyone reading the NVS dump can
 * see it (same as wifi_pass), so security relies on Tab5 not being
 * physically compromised — which is true of the device anyway. */
esp_err_t tab5_settings_get_dragon_api_token(char *buf, size_t len) { return get_str(KEY_DRAGON_TOK, buf, len, ""); }

/* TT #328 Wave 11 — starred skills.  Comma-separated list of tool
 * names (the same `name` field the Dragon /api/v1/tools endpoint
 * returns).  Empty by default; user toggles via tap on a skill card
 * in ui_skills. */
esp_err_t tab5_settings_get_starred_skills(char *buf, size_t len) {
    return get_str(KEY_STAR_SKILLS, buf, len, "");
}

esp_err_t tab5_settings_set_starred_skills(const char *list) {
    return set_str(KEY_STAR_SKILLS, list ? list : "");
}

esp_err_t tab5_settings_set_dragon_api_token(const char *token) { return set_str(KEY_DRAGON_TOK, token ? token : ""); }

/* ── OpenRouter (vmode=5 SOLO_DIRECT) ──────────────────────────────────
 *
 * Defaults match docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md.
 * Empty key disables solo mode at runtime; voice_modes_route_text
 * surfaces VOICE_MODES_ROUTE_SOLO_NO_KEY so the UI can prompt for a
 * QR scan.  `~latest` aliases let users avoid string-tweak churn as
 * OpenRouter ships new model versions. */

esp_err_t tab5_settings_get_or_key(char *buf, size_t len) {
    return get_str(KEY_OR_KEY, buf, len, "");
}
esp_err_t tab5_settings_set_or_key(const char *key) {
    return set_str(KEY_OR_KEY, key ? key : "");
}

esp_err_t tab5_settings_get_or_mdl_llm(char *buf, size_t len) {
    return get_str(KEY_OR_MDL_LLM, buf, len, "~anthropic/claude-haiku-latest");
}
esp_err_t tab5_settings_set_or_mdl_llm(const char *model) {
    return set_str(KEY_OR_MDL_LLM, model ? model : "");
}

esp_err_t tab5_settings_get_or_mdl_stt(char *buf, size_t len) {
    return get_str(KEY_OR_MDL_STT, buf, len, "whisper-1");
}
esp_err_t tab5_settings_set_or_mdl_stt(const char *model) {
    return set_str(KEY_OR_MDL_STT, model ? model : "");
}

esp_err_t tab5_settings_get_or_mdl_tts(char *buf, size_t len) {
    return get_str(KEY_OR_MDL_TTS, buf, len, "tts-1");
}
esp_err_t tab5_settings_set_or_mdl_tts(const char *model) {
    return set_str(KEY_OR_MDL_TTS, model ? model : "");
}

esp_err_t tab5_settings_get_or_mdl_emb(char *buf, size_t len) {
    return get_str(KEY_OR_MDL_EMB, buf, len, "text-embedding-3-small");
}
esp_err_t tab5_settings_set_or_mdl_emb(const char *model) {
    return set_str(KEY_OR_MDL_EMB, model ? model : "");
}

esp_err_t tab5_settings_get_or_voice(char *buf, size_t len) {
    return get_str(KEY_OR_VOICE, buf, len, "alloy");
}
esp_err_t tab5_settings_set_or_voice(const char *voice) {
    return set_str(KEY_OR_VOICE, voice ? voice : "");
}

/* ── Display ──────────────────────────────────────────────────────────── */

uint8_t tab5_settings_get_brightness(void)
{
    return get_u8(KEY_BRIGHTNESS, DEFAULT_BRIGHTNESS);
}

esp_err_t tab5_settings_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    return set_u8(KEY_BRIGHTNESS, pct);
}

/* ── Audio ────────────────────────────────────────────────────────────── */

uint8_t tab5_settings_get_volume(void)
{
    return get_u8(KEY_VOLUME, DEFAULT_VOLUME);
}

esp_err_t tab5_settings_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    return set_u8(KEY_VOLUME, vol);
}

/* ── Voice mode (three-tier) ──────────────────────────────────────────── */

uint8_t tab5_settings_get_voice_mode(void)
{
   /* TT #317 P5: 0=local, 1=hybrid, 2=cloud, 3=tinkerclaw, 4=local_onboard (K144). */
   return get_u8("vmode", 0);
}

esp_err_t tab5_settings_set_voice_mode(uint8_t mode)
{
   if (mode > 4) mode = 0;
   return set_u8("vmode", mode);
}

esp_err_t tab5_settings_get_llm_model(char *buf, size_t len)
{
    return get_str("llm_mdl", buf, len, "anthropic/claude-3.5-haiku");
}

esp_err_t tab5_settings_set_llm_model(const char *model)
{
    return set_str("llm_mdl", model);
}

/* ── Mic mute ───────────────────────────────────────────────────────── */

uint8_t tab5_settings_get_mic_mute(void)
{
    return get_u8("mic_mute", 0);
}

esp_err_t tab5_settings_set_mic_mute(uint8_t muted)
{
    return set_u8("mic_mute", muted ? 1 : 0);
}

/* ── Quiet hours (do-not-disturb) ──────────────────────────────────── */

uint8_t tab5_settings_get_quiet_on(void)    { return get_u8("quiet_on", 0); }
esp_err_t tab5_settings_set_quiet_on(uint8_t on) { return set_u8("quiet_on", on ? 1 : 0); }

uint8_t tab5_settings_get_quiet_start(void) { return get_u8("quiet_start", 22); }
esp_err_t tab5_settings_set_quiet_start(uint8_t h)
{
    if (h > 23) h = 22;
    return set_u8("quiet_start", h);
}
uint8_t tab5_settings_get_quiet_end(void)   { return get_u8("quiet_end", 7); }
esp_err_t tab5_settings_set_quiet_end(uint8_t h)
{
    if (h > 23) h = 7;
    return set_u8("quiet_end", h);
}

/* ── Auto-rotate (audit U2 / TinkerTab #206) ─────────────────────────── */
uint8_t tab5_settings_get_auto_rotate(void) { return get_u8("auto_rot", 0); }
esp_err_t tab5_settings_set_auto_rotate(uint8_t on) { return set_u8("auto_rot", on ? 1 : 0); }

/* ── Camera rotation (#260) ───────────────────────────────────────────── */
/* #286: default 1 (90 CW).  Empirically: the SC2336 sensor is mounted
 * such that holding Tab5 in its natural USB-down portrait orientation,
 * the raw 1280x720 frame needs 90 CW to appear right-side-up.  My
 * first guess of 3 looked plausible from a static screenshot but
 * actually showed the scene upside-down (user feedback while wearing
 * the device).  Users who want a different orientation flip it via
 * Settings → Camera rotation. */
uint8_t tab5_settings_get_cam_rotation(void) { return get_u8("cam_rot", 1); }
esp_err_t tab5_settings_set_cam_rotation(uint8_t rot) { return set_u8("cam_rot", rot & 0x03); }

bool tab5_settings_quiet_active(int hour_local)
{
    if (!tab5_settings_get_quiet_on()) return false;
    if (hour_local < 0 || hour_local > 23) return false;
    int s = tab5_settings_get_quiet_start();
    int e = tab5_settings_get_quiet_end();
    if (s == e) return false;
    if (s < e) {
        /* Range doesn't wrap — e.g. 13..18 */
        return (hour_local >= s && hour_local < e);
    }
    /* Wraps midnight — e.g. 22..7 means [22..23] or [0..6] */
    return (hour_local >= s || hour_local < e);
}

/* ── Connection mode ────────────────────────────────────────────────── */

uint8_t tab5_settings_get_connection_mode(void)
{
    return get_u8("conn_m", 0);  /* 0=auto, 1=local, 2=remote — key changed from "conn_mode" to reset stale value */
}

esp_err_t tab5_settings_set_connection_mode(uint8_t mode)
{
    if (mode > 2) mode = 0;
    return set_u8("conn_m", mode);
}

/* ── v4·D Sovereign Halo mode dials ─────────────────────────────────── */

/* Three orthogonal dials replace the 4-mode pill. Tab5-side resolver
 * turns tiers into the legacy voice_mode + llm_model that Dragon expects,
 * so no backend protocol change is needed for the initial UX ship. */

uint8_t tab5_settings_get_int_tier(void)  { return get_u8("int_tier", 0); }  /* 0=fast 1=balanced 2=smart */
uint8_t tab5_settings_get_voi_tier(void)  { return get_u8("voi_tier", 0); }  /* 0=local 1=neutral 2=studio */
uint8_t tab5_settings_get_aut_tier(void)  { return get_u8("aut_tier", 0); }  /* 0=ask 1=agent */

esp_err_t tab5_settings_set_int_tier(uint8_t t) { if (t > 2) t = 0; return set_u8("int_tier", t); }
esp_err_t tab5_settings_set_voi_tier(uint8_t t) { if (t > 2) t = 0; return set_u8("voi_tier", t); }
esp_err_t tab5_settings_set_aut_tier(uint8_t t) { if (t > 1) t = 0; return set_u8("aut_tier", t); }

bool      tab5_settings_is_onboarded(void) { return get_u8("onboard", 0) != 0; }
esp_err_t tab5_settings_set_onboarded(bool done) { return set_u8("onboard", done ? 1 : 0); }

/* ── v4·D Phase 3c daily cloud spend accumulator ────────────────────── */

/* NVS keys are u32:
 *   "spent_mils"  — cumulative cost for today in mils (1/1000 USD cent)
 *   "spent_day"   — days-since-epoch when spent_mils was last written
 *   "cap_mils"    — per-day spending cap (default 100000 = 100 cents = $1.00)
 *
 * Using mils (not cents) keeps a whole day's spend precise even for very
 * small Haiku turns (~0.3 cents each).  Max u32 = 4.29e9 mils = $42,949
 * which is way more headroom than needed. */

static uint32_t days_since_epoch(void)
{
    /* v4·D audit P1 fix: previous version used (now / 86400) which
     * counts UTC days.  A Los Angeles user's "new day" therefore rolled
     * at 5 PM local, wiping their mid-afternoon budget mid-session.
     * Resolve the local wall-clock instead, giving a stable rollover at
     * local midnight.  Returns 0 if RTC isn't synced yet -- caller's
     * early-return keeps state intact. */
    time_t now = 0;
    time(&now);
    if (now <= 0) return 0;
    struct tm tm;
    localtime_r(&now, &tm);
    /* Compose a canonical date index: year*366 + day-of-year.  Not
     * strictly "days since epoch" but monotonic and unique per
     * calendar day in the user's current timezone, which is what we
     * actually want for budget rollover. */
    return (uint32_t)((tm.tm_year + 1900) * 366 + tm.tm_yday);
}

static void roll_if_new_day(void)
{
    uint32_t today = days_since_epoch();
    if (today == 0) return;  /* RTC not yet synced -- don't stomp state */
    uint32_t stored_day = get_u32("spent_day", 0);
    if (stored_day != today) {
        /* Fresh day: zero the accumulator. */
        set_u32("spent_mils", 0);
        set_u32("spent_day", today);
    }
}

uint32_t tab5_budget_get_today_mils(void)
{
    roll_if_new_day();
    return get_u32("spent_mils", 0);
}

uint32_t tab5_budget_get_cap_mils(void)
{
    return get_u32("cap_mils", 100000);  /* default $1.00/day cap */
}

esp_err_t tab5_budget_set_cap_mils(uint32_t cap_mils)
{
    return set_u32("cap_mils", cap_mils);
}

esp_err_t tab5_budget_accumulate(uint32_t mils)
{
    if (mils == 0) return ESP_OK;
    roll_if_new_day();
    uint32_t cur = get_u32("spent_mils", 0);
    /* Saturate at u32 max — won't happen in practice but don't wrap. */
    uint32_t next = (cur > 0xFFFFFFFFu - mils) ? 0xFFFFFFFFu : cur + mils;
    return set_u32("spent_mils", next);
}

esp_err_t tab5_budget_reset_spent(void)
{
    /* #148: actually zero today's spend.  The debug handler used to
     * advertise this via reset_spent_noop but had no implementation. */
    esp_err_t a = set_u32("spent_mils", 0);
    uint32_t today = days_since_epoch();
    esp_err_t b = (today != 0) ? set_u32("spent_day", today) : ESP_OK;
    return (a == ESP_OK && b == ESP_OK) ? ESP_OK : ESP_FAIL;
}

/* ──────────────────────────────────────────────────────────────────── */

uint8_t tab5_mode_resolve(uint8_t int_tier, uint8_t voi_tier, uint8_t aut_tier,
                          char *out_model, size_t model_len)
{
    /* Autonomy wins: agent always routes via TinkerClaw gateway, regardless
     * of the other two dials. out_model is left untouched -- the gateway
     * picks its own model. */
    if (aut_tier >= 1) {
        return 3; /* VOICE_MODE_TINKERCLAW */
    }

    bool cloud_audio = (voi_tier >= 2);  /* studio only */
    bool cloud_llm   = (int_tier >= 2);  /* smart only */

    if (cloud_audio && cloud_llm) {
        if (out_model && model_len > 0) {
            /* "anthropic/claude-sonnet-4-20250514" rejected by OpenRouter
             * ("not a valid model ID").  Using the current valid Sonnet
             * ID so cloud mode actually works out of the box.  Users can
             * still pick any other model via /mode?model=... */
            snprintf(out_model, model_len, "anthropic/claude-3.5-sonnet");
        }
        return 2; /* Full Cloud */
    }
    if (cloud_audio || cloud_llm) {
        /* Hybrid covers "cloud voice + local brain" cleanly. The "local voice
         * + cloud brain" combo doesn't have a clean legacy mapping -- we
         * route it here too so the user still gets cloud STT for accuracy;
         * the resolver caller may later override llm_model if they want a
         * specific cloud model despite being mid-tier overall. */
        return 1; /* Hybrid */
    }
    return 0; /* Local */
}

/* ── Device identity ─────────────────────────────────────────────────── */

esp_err_t tab5_settings_get_device_id(char *buf, size_t len)
{
    if (!buf || len < 13) return ESP_ERR_INVALID_ARG;  /* need 12 hex chars + NUL */

    /* Try NVS first */
    esp_err_t err = get_str(KEY_DEVICE_ID, buf, len, "");
    if (err == ESP_OK && buf[0] != '\0') {
        return ESP_OK;
    }

    /* First boot — generate from MAC and persist */
    uint8_t mac[6];
    err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(buf, len, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    err = set_str(KEY_DEVICE_ID, buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist device_id to NVS");
    }
    ESP_LOGI(TAG, "Generated device_id: %s", buf);
    return ESP_OK;
}

esp_err_t tab5_settings_get_hardware_id(char *buf, size_t len)
{
    if (!buf || len < 18) return ESP_ERR_INVALID_ARG;  /* "AA:BB:CC:DD:EE:FF" + NUL */

    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

/* ── Session persistence ─────────────────────────────────────────────── */

esp_err_t tab5_settings_get_session_id(char *buf, size_t len)
{
    return get_str(KEY_SESSION_ID, buf, len, "");
}

esp_err_t tab5_settings_set_session_id(const char *session_id)
{
    return set_str(KEY_SESSION_ID, session_id ? session_id : "");
}

/* ── OTA rollback detection (TT #328 Wave 3 P0 #14) ──────────────────── */

#define KEY_OTA_ATTEMPTED "ota_try"

esp_err_t tab5_settings_get_ota_attempted(char *buf, size_t len) { return get_str(KEY_OTA_ATTEMPTED, buf, len, ""); }

esp_err_t tab5_settings_set_ota_attempted(const char *version) {
   return set_str(KEY_OTA_ATTEMPTED, version ? version : "");
}

/* ── First-launch UI hints (TT #328 Wave 9) ─────────────────────────── */

#define KEY_MODE_HINT "mode_hint"

bool tab5_settings_mode_hint_seen(void) { return get_u8(KEY_MODE_HINT, 0) != 0; }
esp_err_t tab5_settings_set_mode_hint_seen(bool seen) { return set_u8(KEY_MODE_HINT, seen ? 1 : 0); }

/* ── Auth token (debug server bearer auth) ───────────────────────────── */

esp_err_t tab5_settings_get_auth_token(char *buf, size_t len)
{
    return get_str("auth_tok", buf, len, "");
}

esp_err_t tab5_settings_set_auth_token(const char *token)
{
    return set_str("auth_tok", token);
}

/* ── NVS write counter (US-HW17) ────────────────────────────────────────── */

uint32_t tab5_settings_get_nvs_write_count(void)
{
    return s_nvs_write_count;
}
