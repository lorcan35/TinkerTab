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

/* Hard reinit of the Wi-Fi stack: esp_wifi_stop() → esp_wifi_start().
 * Used when the soft kick fails to recover — e.g. the ESP-Hosted SDIO
 * host driver or MAC/PHY layer is wedged.  The full stop/start cycle
 * resets the host driver's internal state + re-handshakes with the
 * Wi-Fi chip, which the soft kick doesn't touch.  Expensive (~1-2 s
 * of downtime while the radio re-associates) but recovers classes of
 * failure that esp_wifi_disconnect() cannot. */
esp_err_t tab5_wifi_hard_kick(void);
