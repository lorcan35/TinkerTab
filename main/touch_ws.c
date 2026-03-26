/**
 * TinkerTab — WebSocket Touch Forwarder
 *
 * Maintains a WebSocket connection to Dragon and sends touch events
 * as JSON messages for remote control of the Dragon's desktop.
 */

#include "touch_ws.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "esp_timer.h"

static const char *TAG = "tab5_ws";

static esp_transport_handle_t s_ws = NULL;
static bool s_connected = false;

static esp_transport_handle_t ws_create(void)
{
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    esp_transport_ws_set_path(ws, TAB5_TOUCH_WS_PATH);
    return ws;
}

static void touch_ws_task(void *arg)
{
    ESP_LOGI(TAG, "Touch WS task started (target: %s:%d%s)",
             TAB5_DRAGON_HOST, TAB5_DRAGON_PORT, TAB5_TOUCH_WS_PATH);

    while (1) {
        s_ws = ws_create();
        if (!s_ws) {
            ESP_LOGE(TAG, "Failed to create WS transport");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "Connecting to Dragon WebSocket...");
        int err = esp_transport_connect(s_ws, TAB5_DRAGON_HOST, TAB5_DRAGON_PORT, 5000);
        if (err < 0) {
            ESP_LOGW(TAG, "WS connect failed, retry in 5s");
            esp_transport_close(s_ws);
            esp_transport_destroy(s_ws);
            s_ws = NULL;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        s_connected = true;
        ESP_LOGI(TAG, "WebSocket connected to Dragon!");

        // Keep connection alive — read for server messages or detect disconnect
        while (s_connected) {
            int poll = esp_transport_poll_read(s_ws, 1000);
            if (poll < 0) {
                ESP_LOGW(TAG, "WS connection lost");
                s_connected = false;
                break;
            }
            if (poll > 0) {
                char buf[256];
                int len = esp_transport_read(s_ws, buf, sizeof(buf) - 1, 1000);
                if (len <= 0) {
                    ESP_LOGW(TAG, "WS read error, disconnecting");
                    s_connected = false;
                    break;
                }
                buf[len] = '\0';
                ESP_LOGI(TAG, "Dragon says: %s", buf);
            }
        }

        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = NULL;
        s_connected = false;
        ESP_LOGW(TAG, "Reconnecting in 2s...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void tab5_touch_ws_start(void)
{
    xTaskCreatePinnedToCore(touch_ws_task, "touch_ws", 4096, NULL, 4, NULL, 0);
}

void tab5_touch_ws_send(const tab5_touch_point_t *points, uint8_t count)
{
    if (!s_connected || !s_ws || count == 0) return;

    // Build compact JSON: {"t":[{"x":123,"y":456,"s":7},...]}
    char buf[256];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"t\":[");
    for (int i = 0; i < count && i < 5; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{\"x\":%d,\"y\":%d,\"s\":%d}",
                        points[i].x, points[i].y, points[i].strength);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

    esp_transport_ws_send_raw(s_ws,
        WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
        buf, pos, 100);
}

bool tab5_touch_ws_connected(void)
{
    return s_connected;
}
