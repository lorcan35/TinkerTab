/**
 * TinkerTab — mDNS Discovery
 *
 * Discovers Dragon Q6A on the local network via mDNS/DNS-SD.
 * Queries for _tinkerclaw._tcp service, falls back to hardcoded config.
 * Also registers Tab5 itself as _tinkertab._tcp so Dragon can find it.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/** Discovery result — holds resolved host and port. */
typedef struct {
    char host[64];       // IP address string
    uint16_t port;       // Service port
    bool from_mdns;      // true if discovered via mDNS, false if fallback
} mdns_discovery_result_t;

/**
 * Initialize mDNS subsystem and register Tab5 as _tinkertab._tcp.
 * Call once after WiFi is connected.
 */
esp_err_t tab5_mdns_init(void);

/**
 * Query for Dragon via mDNS (_tinkerclaw._tcp).
 * Blocks up to timeout_ms. If found, fills result with discovered values.
 * If not found, fills result with hardcoded fallback from config.
 *
 * @param result    Output: discovered or fallback host/port
 * @param timeout_ms  Max time to wait for mDNS response (e.g. 5000)
 * @return ESP_OK on success (even if using fallback)
 */
esp_err_t tab5_mdns_discover_dragon(mdns_discovery_result_t *result, uint32_t timeout_ms);
