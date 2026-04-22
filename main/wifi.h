#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t tab5_wifi_init(void);
esp_err_t tab5_wifi_wait_connected(int timeout_ms);
bool tab5_wifi_connected(void);

/* Force a fresh Wi-Fi association: disconnect + connect.  Used by the
 * link-probe task to recover from "zombie association" — radio still
 * reports associated but the AP has stopped routing Tab5's traffic
 * (client isolation, bridge-table flush, 802.11 deauth-without-notify).
 * No-op if we're currently not associated (the normal retry path will
 * handle it). */
void tab5_wifi_kick(void);
