/* esp_wifi.h — Linux target stub (hal_linux component) */
#pragma once
#include "esp_err.h"
#include "esp_event.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* Event base */
#define WIFI_EVENT ((esp_event_base_t)"WIFI_EVENT")
typedef enum {
    WIFI_EVENT_WIFI_READY = 0,
    WIFI_EVENT_SCAN_DONE,
    WIFI_EVENT_STA_START,
    WIFI_EVENT_STA_STOP,
    WIFI_EVENT_STA_CONNECTED,
    WIFI_EVENT_STA_DISCONNECTED,
} wifi_event_t;

typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP = 1 } wifi_interface_t;
typedef enum {
    WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WAPI_PSK, WIFI_AUTH_MAX
} wifi_auth_mode_t;

typedef enum { WIFI_SCAN_TYPE_ACTIVE = 0, WIFI_SCAN_TYPE_PASSIVE } wifi_scan_type_t;

typedef struct {
    uint16_t min;
    uint16_t max;
} wifi_active_scan_time_t;

typedef union {
    wifi_active_scan_time_t active;
    uint16_t passive;
} wifi_scan_time_t;

typedef struct {
    const uint8_t     *ssid;
    const uint8_t     *bssid;
    uint8_t            channel;
    bool               show_hidden;
    wifi_scan_type_t   scan_type;
    wifi_scan_time_t   scan_time;
} wifi_scan_config_t;

typedef struct {
    uint8_t ssid[33];
    uint8_t password[64];
    struct { wifi_auth_mode_t authmode; } threshold;
} wifi_sta_config_t;

typedef union { wifi_sta_config_t sta; } wifi_config_t;

typedef struct {
    uint8_t          ssid[33];
    uint8_t          bssid[6];
    int8_t           rssi;
    wifi_auth_mode_t authmode;
    uint8_t          primary;
} wifi_ap_record_t;

static inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool b) { (void)c; (void)b; return ESP_FAIL; }
static inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r) { if(n) *n=0; (void)r; return ESP_FAIL; }
static inline esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c) { (void)i; (void)c; return ESP_FAIL; }
static inline esp_err_t esp_wifi_connect(void) { return ESP_FAIL; }
static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *i) { (void)i; return ESP_FAIL; }
