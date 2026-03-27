/**
 * TinkerTab — mDNS Discovery
 *
 * Uses ESP-IDF mDNS component to:
 * 1. Register Tab5 as "_tinkertab._tcp" so Dragon can find it
 * 2. Query for Dragon as "_tinkerclaw._tcp" before connecting
 * 3. Fall back to hardcoded CONFIG values if mDNS query fails
 */

#include "mdns_discovery.h"
#include "config.h"
#include "settings.h"

#include <string.h>
#include "mdns.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "mdns_disc";

// mDNS service type Dragon advertises
#define DRAGON_SERVICE_TYPE  "_tinkerclaw"
#define DRAGON_SERVICE_PROTO "_tcp"

// mDNS service type Tab5 advertises
#define TAB5_SERVICE_TYPE    "_tinkertab"
#define TAB5_SERVICE_PROTO   "_tcp"
#define TAB5_SERVICE_PORT    80

esp_err_t tab5_mdns_init(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Set hostname so Tab5 is reachable as "tinkertab.local"
    err = mdns_hostname_set("tinkertab");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
    }

    err = mdns_instance_name_set("TinkerClaw Tab5");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
    }

    // Register Tab5 as a discoverable service
    mdns_txt_item_t txt_items[] = {
        { "fw",     "1.0.0" },
        { "device", "tab5"  },
    };

    err = mdns_service_add("TinkerClaw Tab5", TAB5_SERVICE_TYPE, TAB5_SERVICE_PROTO,
                           TAB5_SERVICE_PORT, txt_items,
                           sizeof(txt_items) / sizeof(txt_items[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Registered mDNS: TinkerClaw Tab5 (%s.%s port %d)",
                 TAB5_SERVICE_TYPE, TAB5_SERVICE_PROTO, TAB5_SERVICE_PORT);
    }

    return ESP_OK;
}

esp_err_t tab5_mdns_discover_dragon(mdns_discovery_result_t *result, uint32_t timeout_ms)
{
    if (!result) return ESP_ERR_INVALID_ARG;

    // Default to fallback (NVS-backed, falls back to compile-time default)
    tab5_settings_get_dragon_host(result->host, sizeof(result->host));
    result->port = tab5_settings_get_dragon_port();
    result->from_mdns = false;

    ESP_LOGI(TAG, "Querying mDNS for %s.%s (timeout %lums)...",
             DRAGON_SERVICE_TYPE, DRAGON_SERVICE_PROTO, (unsigned long)timeout_ms);

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(DRAGON_SERVICE_TYPE, DRAGON_SERVICE_PROTO,
                                   timeout_ms, 1, &results);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS query failed: %s — using fallback %s:%d",
                 esp_err_to_name(err), result->host, result->port);
        return ESP_OK;
    }

    if (!results) {
        ESP_LOGW(TAG, "mDNS: no %s.%s service found — using fallback %s:%d",
                 DRAGON_SERVICE_TYPE, DRAGON_SERVICE_PROTO,
                 result->host, result->port);
        return ESP_OK;
    }

    // Walk the result to find one with an IPv4 address
    mdns_result_t *r = results;
    bool found = false;

    while (r && !found) {
        if (r->addr) {
            mdns_ip_addr_t *addr = r->addr;
            while (addr && !found) {
                if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                    snprintf(result->host, sizeof(result->host),
                             IPSTR, IP2STR(&addr->addr.u_addr.ip4));
                    result->port = r->port;
                    result->from_mdns = true;
                    found = true;

                    ESP_LOGI(TAG, "mDNS discovered Dragon: %s:%d (instance: %s)",
                             result->host, result->port,
                             r->instance_name ? r->instance_name : "?");
                }
                addr = addr->next;
            }
        }
        r = r->next;
    }

    mdns_query_results_free(results);

    if (!found) {
        ESP_LOGW(TAG, "mDNS: found service but no IPv4 address — using fallback %s:%d",
                 result->host, result->port);
    }

    return ESP_OK;
}
