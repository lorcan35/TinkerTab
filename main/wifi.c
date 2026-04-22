/**
 * TinkerClaw Tab5 — WiFi module
 * Connects to the home WiFi network (same LAN as Dragon).
 */

#include "wifi.h"
#include "config.h"
#include "settings.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "tab5_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

/* Wave 15 W15-H07: NO latching failure bit.  The old code gave up forever
 * after 30 consecutive disassociations and then NEVER retried Wi-Fi — a
 * reboot was the only recovery.  That's the "keeps connecting but can't
 * hold a connection" symptom reported by the user: a single rough period
 * (router reboot, AP channel hop, PMF desync) exhausted the 30-slot
 * counter, after which the Wi-Fi layer was dead but the voice WS kept
 * hammering against a dead netif with 20-s select() timeouts.
 *
 * New policy: retry forever.  `esp_wifi_connect()` is non-blocking and
 * the Wi-Fi driver has its own internal scan/auth backoff, so calling
 * it on every DISCONNECTED event is cheap and correct for a tethered
 * assistant.  We keep the counter only for log rate-limiting. */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        s_retry_count++;
        /* Log every disconnect for the first 5, then every 20th so we
         * don't spam the UART during a long outage (AP down for hours). */
        if (s_retry_count <= 5 || (s_retry_count % 20) == 0) {
            ESP_LOGI(TAG, "Retrying WiFi (attempt=%d, reason=%d)",
                     s_retry_count, disconn->reason);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR " (after %d retry attempts)",
                 IP2STR(&event->ip_info.ip), s_retry_count);
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t tab5_wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable  = true,
                .required = false,
            },
        },
    };

    tab5_settings_get_wifi_ssid((char *)wifi_config.sta.ssid,
                                sizeof(wifi_config.sta.ssid));
    tab5_settings_get_wifi_pass((char *)wifi_config.sta.password,
                                sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* v4·D connectivity audit -- ROOT CAUSE FIX for "WS drops even
     * though the network is fine".
     *
     * Default ESP32 WiFi power-save is WIFI_PS_MIN_MODEM, which parks
     * the radio between beacon intervals to save ~40 mA.  On a
     * plugged-in voice-assistant tablet that's the wrong trade: the
     * radio sleep window introduces 100-300 ms latency spikes AND can
     * cause the AP to drop the association on certain routers (Asus,
     * Ubiquiti UniFi with fast-roaming) if Tab5 misses a beacon window
     * while replying to a TCP ACK.  That's why the "always-healthy"
     * connection was randomly going dead -- WiFi was quietly
     * half-losing the association without either TCP peer noticing
     * until the next write.
     *
     * WIFI_PS_NONE keeps the radio always on.  Slightly higher idle
     * current (~60 mA more) but the device is tethered to USB-C
     * anyway.  Stability > power for this form factor. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi init done, connecting to '%s' (pass len=%d)",
             (char *)wifi_config.sta.ssid, (int)strlen((char *)wifi_config.sta.password));
    return ESP_OK;
}

esp_err_t tab5_wifi_wait_connected(int timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

bool tab5_wifi_connected(void)
{
    if (!s_wifi_event_group) return false;
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

void tab5_wifi_kick(void)
{
    /* Wave 15 W15-H08: force a fresh association.  Used by the link
     * probe when both LAN and ngrok TCP connects fail while the radio
     * still reports associated — i.e. the AP has silently stopped
     * passing our traffic.  esp_wifi_disconnect() synthesises a
     * WIFI_EVENT_STA_DISCONNECTED which our handler already routes
     * into esp_wifi_connect(), so one call is enough. */
    if (!s_wifi_event_group) return;
    ESP_LOGW(TAG, "Kicking Wi-Fi (forcing re-association)");
    esp_err_t r = esp_wifi_disconnect();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(r));
    }
}
