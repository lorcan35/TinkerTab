#pragma once
#include "esp_err.h"

esp_err_t tab5_wifi_init(void);
esp_err_t tab5_wifi_wait_connected(int timeout_ms);
