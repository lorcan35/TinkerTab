#pragma once
/* ESP-IDF esp_wifi.h stub — comprehensive types for desktop simulator */
#include "esp_err.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef enum {
    WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA
} wifi_mode_t;

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA2_ENTERPRISE,
    WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK,
    WIFI_AUTH_WAPI_PSK,
    WIFI_AUTH_MAX
} wifi_auth_mode_t;

typedef enum {
    WIFI_IF_STA = 0,
    WIFI_IF_AP,
} wifi_interface_t;

typedef enum {
    WIFI_SCAN_TYPE_ACTIVE = 0,
    WIFI_SCAN_TYPE_PASSIVE,
} wifi_scan_type_t;

typedef struct {
    uint16_t min;
    uint16_t max;
} wifi_scan_time_active_t;

typedef struct {
    wifi_scan_time_active_t active;
} wifi_active_scan_time_t;

typedef struct {
    uint8_t *ssid;
    uint8_t *bssid;
    uint8_t channel;
    bool    show_hidden;
    wifi_scan_type_t scan_type;
    wifi_active_scan_time_t scan_time;
    uint8_t home_chan_dwell_time;
} wifi_scan_config_t;

typedef struct {
    uint8_t          ssid[33];
    uint8_t          bssid[6];
    uint8_t          primary;
    int8_t           rssi;
    wifi_auth_mode_t authmode;
    uint8_t          phy_11b;
    uint8_t          phy_11g;
    uint8_t          phy_11n;
    uint8_t          phy_lr;
    uint8_t          phy_11ax;
    uint8_t          wps;
    uint8_t          ftm_responder;
    uint8_t          ftm_initiator;
    uint8_t          reserved;
    uint32_t         channel;
} wifi_ap_record_t;

typedef struct {
    wifi_auth_mode_t authmode;
    int8_t           rssi;
} wifi_scan_threshold_t;

typedef struct {
    uint8_t                ssid[32];
    uint8_t                password[64];
    wifi_auth_mode_t       authmode;
    wifi_scan_threshold_t  threshold;
    uint8_t                bssid[6];
    uint8_t                bssid_set;
    uint8_t                channel;
    uint8_t                listen_interval;
    uint8_t                sort_method;
    uint8_t                pmf_cfg;
    uint8_t                rm_enabled;
    uint8_t                btm_enabled;
    uint8_t                mbo_enabled;
    uint8_t                ft_enabled;
    uint8_t                owe_enabled;
    uint8_t                transition_disable;
    uint8_t                reserved;
    uint8_t                sae_pwe_h2e;
    uint8_t                sae_pk_mode;
    uint8_t                failure_retry_cnt;
    uint8_t                he_dcm_set;
    uint8_t                he_dcm_max_constellation_tx;
    uint8_t                he_dcm_max_constellation_rx;
    uint8_t                he_mcs9_enabled;
    uint8_t                he_su_beamformee_disabled;
    uint8_t                he_trig_su_bmforming_feedback_disabled;
    uint8_t                he_trig_mu_bmforming_partial_feedback_disabled;
    uint8_t                he_trig_cqi_feedback_disabled;
} wifi_sta_config_t;

typedef struct {
    uint8_t          ssid[32];
    uint8_t          password[64];
    uint8_t          ssid_len;
    uint8_t          channel;
    wifi_auth_mode_t authmode;
    uint8_t          ssid_hidden;
    uint8_t          max_connection;
    uint16_t         beacon_interval;
} wifi_ap_config_t;

typedef union {
    wifi_sta_config_t sta;
    wifi_ap_config_t  ap;
} wifi_config_t;

typedef void *esp_event_handler_instance_t;

/* WiFi event IDs */
#define WIFI_EVENT_WIFI_READY         0
#define WIFI_EVENT_SCAN_DONE          1
#define WIFI_EVENT_STA_START          2
#define WIFI_EVENT_STA_STOP           3
#define WIFI_EVENT_STA_CONNECTED      4
#define WIFI_EVENT_STA_DISCONNECTED   5
#define WIFI_EVENT_STA_AUTHMODE_CHANGE 6
#define WIFI_EVENT_AP_START           11
#define WIFI_EVENT_AP_STOP            12
#define WIFI_EVENT_AP_STACONNECTED    13
#define WIFI_EVENT_AP_STADISCONNECTED 14
#define IP_EVENT_STA_GOT_IP           0
#define IP_EVENT_STA_LOST_IP          1

static inline esp_err_t esp_wifi_init(void *cfg)                                { return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(wifi_mode_t m)                        { return ESP_OK; }
static inline esp_err_t esp_wifi_get_mode(wifi_mode_t *m)                       { *m = WIFI_MODE_STA; return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c){ return ESP_OK; }
static inline esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c){ return ESP_OK; }
static inline esp_err_t esp_wifi_start(void)                                    { return ESP_OK; }
static inline esp_err_t esp_wifi_stop(void)                                     { return ESP_OK; }
static inline esp_err_t esp_wifi_connect(void)                                  { return ESP_OK; }
static inline esp_err_t esp_wifi_disconnect(void)                               { return ESP_OK; }
static inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool b){ return ESP_OK; }
static inline esp_err_t esp_wifi_scan_stop(void)                                { return ESP_OK; }
static inline esp_err_t esp_wifi_scan_get_ap_num(uint16_t *n)                   { *n = 0; return ESP_OK; }
static inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r){ *n = 0; return ESP_OK; }
static inline esp_err_t esp_wifi_clear_ap_list(void)                            { return ESP_OK; }
static inline esp_err_t esp_wifi_restore(void)                                  { return ESP_OK; }
static inline esp_err_t esp_wifi_set_ps(int ps)                                 { return ESP_OK; }
static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info) { return -1; }
#define WIFI_EVENT ((esp_event_base_t)1)
