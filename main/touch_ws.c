/**
 * TinkerTab — WebSocket Touch Forwarder
 *
 * Maintains a WebSocket connection to Dragon and sends touch events
 * as JSON messages for remote control of the Dragon's desktop.
 * Responds to heartbeat pings from Dragon.
 */

#include "touch_ws.h"
#include "config.h"
#include "settings.h"

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
static volatile bool s_stop_flag = false;
static volatile bool s_running = false;
static TaskHandle_t s_task_handle = NULL;
static touch_ws_disconnect_cb_t s_disconnect_cb = NULL;

static esp_transport_handle_t ws_create(void)
{
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    esp_transport_ws_set_path(ws, TAB5_TOUCH_WS_PATH);
    return ws;
}

static void touch_ws_task(void *arg)
{
    char dragon_host[64];
    tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
    uint16_t dragon_port = tab5_settings_get_dragon_port();

    ESP_LOGI(TAG, "Touch WS task started (target: %s:%d%s)",
             dragon_host, dragon_port, TAB5_TOUCH_WS_PATH);

    s_ws = ws_create();
    if (!s_ws) {
        ESP_LOGE(TAG, "Failed to create WS transport");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Connecting to Dragon WebSocket...");
    int err = esp_transport_connect(s_ws, dragon_host, dragon_port, 5000);
    if (err < 0) {
        ESP_LOGW(TAG, "WS connect failed");
        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = NULL;
        s_running = false;

        if (!s_stop_flag && s_disconnect_cb) {
            s_disconnect_cb();
        }
        vTaskDelete(NULL);
        return;
    }

    s_connected = true;
    ESP_LOGI(TAG, "WebSocket connected to Dragon!");

    int64_t last_heartbeat = esp_timer_get_time();

    // Keep connection alive — read for server messages, respond to pings
    while (s_connected && !s_stop_flag) {
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

            // Check for heartbeat ping from Dragon
            if (strstr(buf, "\"ping\"")) {
                char pong[32];
                strcpy(pong, "{\"pong\":true}");
                esp_transport_ws_send_raw(s_ws,
                    WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                    pong, strlen(pong), 100);
                last_heartbeat = esp_timer_get_time();
            } else {
                ESP_LOGI(TAG, "Dragon says: %s", buf);
            }
        }

        // Send proactive heartbeat if no server ping in a while
        int64_t now = esp_timer_get_time();
        if ((now - last_heartbeat) > (TAB5_DRAGON_HEARTBEAT_MS * 1000LL * 2)) {
            char hb[32];
            strcpy(hb, "{\"heartbeat\":true}");
            int sent = esp_transport_ws_send_raw(s_ws,
                WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                hb, strlen(hb), 100);
            if (sent < 0) {
                ESP_LOGW(TAG, "Heartbeat send failed, disconnecting");
                s_connected = false;
                break;
            }
            last_heartbeat = now;
        }
    }

    esp_transport_close(s_ws);
    esp_transport_destroy(s_ws);
    s_ws = NULL;
    s_connected = false;
    s_running = false;
    s_task_handle = NULL;

    if (!s_stop_flag && s_disconnect_cb) {
        s_disconnect_cb();
    }

    ESP_LOGI(TAG, "Touch WS task exiting");
    vTaskDelete(NULL);
}

void tab5_touch_ws_start(void)
{
    if (s_running) return;
    s_stop_flag = false;
    s_running = true;

    xTaskCreatePinnedToCore(touch_ws_task, "touch_ws", 8192, NULL, 4, &s_task_handle, 1);
}

void tab5_touch_ws_stop(void)
{
    if (!s_running) return;
    s_stop_flag = true;
    s_connected = false;
    // Task will exit on next poll timeout
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

void tab5_touch_ws_set_disconnect_cb(touch_ws_disconnect_cb_t cb)
{
    s_disconnect_cb = cb;
}
