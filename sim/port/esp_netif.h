#pragma once
/* ESP-IDF esp_netif.h stub */
#include "esp_err.h"
#include <stdint.h>

typedef void *esp_netif_t;

typedef struct {
    uint32_t ip;
    uint32_t netmask;
    uint32_t gw;
} esp_netif_ip_info_t;

#define IPSTR        "%d.%d.%d.%d"
#define IP2STR(a)    0,0,0,0
#define IP4ADDR_STRLEN_MAX 16

static inline esp_err_t esp_netif_init(void) { return ESP_OK; }
static inline esp_netif_t *esp_netif_create_default_wifi_sta(void) { return NULL; }
static inline esp_netif_t *esp_netif_create_default_wifi_ap(void) { return NULL; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) {
    if (i) { i->ip = 0; i->netmask = 0; i->gw = 0; }
    return ESP_OK;
}
static inline esp_err_t esp_netif_get_hostname(esp_netif_t *n, const char **hn) { *hn = "tinkeros-sim"; return ESP_OK; }
static inline void esp_netif_destroy_default_wifi(esp_netif_t *n) { }
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { return NULL; }
