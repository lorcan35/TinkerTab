#pragma once
/* ESP-IDF esp_err.h stub for desktop simulator */
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC     0x109
#define ESP_ERR_WIFI_BASE       0x3000
#define ESP_ERR_NETIF_BASE      0x5000

static inline const char *esp_err_to_name(esp_err_t code) {
    switch (code) {
    case ESP_OK:              return "ESP_OK";
    case ESP_FAIL:            return "ESP_FAIL";
    case ESP_ERR_NO_MEM:      return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_TIMEOUT:     return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    default:                  return "UNKNOWN";
    }
}

#define ESP_ERROR_CHECK(x) do { esp_err_t __r = (x); (void)__r; } while(0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) (x)
