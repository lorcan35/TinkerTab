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

#include <string.h>
#include <stdio.h>

static const char *TAG = "settings";

/* NVS namespace — max 15 chars */
#define NVS_NS "settings"

/* NVS keys — max 15 chars each */
#define KEY_WIFI_SSID   "wifi_ssid"
#define KEY_WIFI_PASS   "wifi_pass"
#define KEY_DRAGON_HOST "dragon_host"
#define KEY_DRAGON_PORT "dragon_port"
#define KEY_BRIGHTNESS  "brightness"
#define KEY_VOLUME      "volume"
#define KEY_DEVICE_ID   "device_id"
#define KEY_SESSION_ID  "session_id"

/* Compile-time defaults */
#define DEFAULT_BRIGHTNESS  80
#define DEFAULT_VOLUME      70

static nvs_handle_t s_nvs = 0;
static bool         s_inited = false;

/* ── Init ─────────────────────────────────────────────────────────────── */

esp_err_t tab5_settings_init(void)
{
    if (s_inited) return ESP_OK;

    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NS, esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "NVS namespace '%s' opened", NVS_NS);
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
    esp_err_t err = nvs_get_str(s_nvs, key, buf, &required);
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

    esp_err_t err = nvs_set_str(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static uint8_t get_u8(const char *key, uint8_t def)
{
    if (!s_inited) return def;

    uint8_t val = def;
    esp_err_t err = nvs_get_u8(s_nvs, key, &val);
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

    esp_err_t err = nvs_set_u8(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static uint16_t get_u16(const char *key, uint16_t def)
{
    if (!s_inited) return def;

    uint16_t val = def;
    esp_err_t err = nvs_get_u16(s_nvs, key, &val);
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

    esp_err_t err = nvs_set_u16(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u16(%s) failed: %s", key, esp_err_to_name(err));
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
    return get_u8("vmode", 0);  /* 0=local, 1=hybrid, 2=cloud */
}

esp_err_t tab5_settings_set_voice_mode(uint8_t mode)
{
    if (mode > 2) mode = 0;
    return set_u8("vmode", mode);
}

esp_err_t tab5_settings_get_llm_model(char *buf, size_t len)
{
    return get_str("llm_mdl", buf, len, "anthropic/claude-3-haiku");
}

esp_err_t tab5_settings_set_llm_model(const char *model)
{
    return set_str("llm_mdl", model);
}

/* ── Wake word ──────────────────────────────────────────────────────── */

uint8_t tab5_settings_get_wake_word(void)
{
    return get_u8("wake", 0);
}

esp_err_t tab5_settings_set_wake_word(uint8_t enabled)
{
    return set_u8("wake", enabled ? 1 : 0);
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
