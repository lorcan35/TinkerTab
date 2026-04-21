/**
 * TinkerTab — WebSocket Touch Forwarder
 *
 * Maintains a WebSocket connection to Dragon and sends touch events
 * as JSON messages for remote control of the Dragon's desktop.
 * Responds to heartbeat pings from Dragon.
 */

#include "touch_ws.h"
#include "touch.h"
#include "config.h"
#include "settings.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
static SemaphoreHandle_t s_ws_mutex = NULL;

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

    /* Outer retry loop — avoids vTaskDelete which causes TLSP crash on P4 (#18) */
    while (!s_stop_flag) {

    esp_transport_handle_t ws = ws_create();
    if (!ws) {
        ESP_LOGE(TAG, "Failed to create WS transport");
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
    }

    ESP_LOGI(TAG, "Connecting to Dragon WebSocket...");
    int err = esp_transport_connect(ws, dragon_host, dragon_port, 5000);
    if (err < 0) {
        ESP_LOGW(TAG, "WS connect failed, retrying in 2s...");
        esp_transport_close(ws);
        esp_transport_destroy(ws);
        if (s_disconnect_cb) s_disconnect_cb();
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
    }

    /* Publish transport under mutex */
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    s_ws = ws;
    s_connected = true;
    xSemaphoreGive(s_ws_mutex);

    ESP_LOGI(TAG, "WebSocket connected to Dragon!");

    int64_t last_heartbeat = esp_timer_get_time();
    bool was_touching = false;  // Track previous touch state for release detection

    // Keep connection alive — poll touch, read server messages, respond to pings
    while (!s_stop_flag) {
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        bool connected = s_connected;
        xSemaphoreGive(s_ws_mutex);
        if (!connected) break;

        // --- Poll touch and forward to Dragon ---
        tab5_touch_point_t points[TAB5_TOUCH_MAX_POINTS];
        uint8_t count = 0;
        bool touching = tab5_touch_read(points, &count);

        if (touching && count > 0) {
            tab5_touch_ws_send(points, count);
            was_touching = true;
        } else if (was_touching) {
            // Transition: touching → not touching — send release event once
            tab5_touch_ws_send_release();
            was_touching = false;
        }

        // --- Check for incoming messages (non-blocking, short timeout) ---
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        if (!s_ws || !s_connected) {
            xSemaphoreGive(s_ws_mutex);
            break;
        }
        int poll = esp_transport_poll_read(s_ws, 20);
        xSemaphoreGive(s_ws_mutex);

        if (poll < 0) {
            ESP_LOGW(TAG, "WS connection lost");
            xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
            s_connected = false;
            xSemaphoreGive(s_ws_mutex);
            break;
        }
        if (poll > 0) {
            char buf[256];
            xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
            if (!s_ws || !s_connected) {
                xSemaphoreGive(s_ws_mutex);
                break;
            }
            int len = esp_transport_read(s_ws, buf, sizeof(buf) - 1, 1000);
            xSemaphoreGive(s_ws_mutex);

            if (len <= 0) {
                ESP_LOGW(TAG, "WS read error, disconnecting");
                xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
                s_connected = false;
                xSemaphoreGive(s_ws_mutex);
                break;
            }
            buf[len] = '\0';

            // Check for heartbeat ping from Dragon
            if (strstr(buf, "\"ping\"")) {
                char pong[32];
                strcpy(pong, "{\"pong\":true}");
                xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
                if (s_ws && s_connected) {
                    int ret = esp_transport_ws_send_raw(s_ws,
                        WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                        pong, strlen(pong), 100);
                    if (ret < 0) {
                        ESP_LOGW(TAG, "Pong send failed");
                        s_connected = false;
                    }
                }
                xSemaphoreGive(s_ws_mutex);
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
            xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
            if (!s_ws || !s_connected) {
                xSemaphoreGive(s_ws_mutex);
                break;
            }
            int sent = esp_transport_ws_send_raw(s_ws,
                WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                hb, strlen(hb), 100);
            if (sent < 0) {
                ESP_LOGW(TAG, "Heartbeat send failed, disconnecting");
                s_connected = false;
            }
            xSemaphoreGive(s_ws_mutex);
            if (sent < 0) break;
            last_heartbeat = now;
        }

        // ~20ms poll interval for responsive touch forwarding
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* Cleanup under mutex — prevent sends on destroyed transport */
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    if (s_ws) {
        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = NULL;
    }
    s_connected = false;
    xSemaphoreGive(s_ws_mutex);

    if (s_stop_flag) break;

    ESP_LOGW(TAG, "Touch WS disconnected, retrying in 2s...");
    if (s_disconnect_cb) s_disconnect_cb();
    vTaskDelay(pdMS_TO_TICKS(2000));

    } /* end outer retry loop */

    s_running = false;
    s_task_handle = NULL;
    ESP_LOGI(TAG, "Touch WS task exiting (stop requested)");
    /* Do NOT call vTaskDelete(NULL) — TLSP cleanup crashes IDLE task on P4 (#18).
     * Instead, suspend forever. Task handle is NULL so it won't be re-stopped. */
    vTaskSuspend(NULL);
}

void tab5_touch_ws_start(void)
{
    if (s_running) return;

    if (!s_ws_mutex) {
        s_ws_mutex = xSemaphoreCreateMutex();
        configASSERT(s_ws_mutex);
    }

    s_stop_flag = false;
    s_running = true;

    xTaskCreatePinnedToCore(touch_ws_task, "touch_ws", 10240, NULL, 4, &s_task_handle, 1);
}

void tab5_touch_ws_stop(void)
{
    if (!s_running) return;

    /* Close and destroy the transport under mutex so no in-flight send
     * can use a stale handle.  The task will see s_ws==NULL and exit. */
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    s_stop_flag = true;
    s_connected = false;
    if (s_ws) {
        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = NULL;
    }
    xSemaphoreGive(s_ws_mutex);

    // Wait for task to actually exit
    for (int i = 0; i < 40 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_running) {
        ESP_LOGW("touch_ws", "Touch WS task did not exit in 2s");
    }
}

void tab5_touch_ws_send(const tab5_touch_point_t *points, uint8_t count)
{
    if (count == 0) return;

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    if (!s_ws || !s_connected) {
        xSemaphoreGive(s_ws_mutex);
        return;
    }

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

    int ret = esp_transport_ws_send_raw(s_ws,
        WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
        buf, pos, 100);
    if (ret < 0) {
        ESP_LOGW(TAG, "Touch send failed, marking disconnected");
        s_connected = false;
    }
    xSemaphoreGive(s_ws_mutex);
}

void tab5_touch_ws_send_release(void)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    if (!s_ws || !s_connected) {
        xSemaphoreGive(s_ws_mutex);
        return;
    }

    // Send empty touch array to signal release: {"t":[]}
    // Wave 14 W14-C01: esp_transport_ws_send_raw masks the buffer in place.
    // Passing a string literal here XORs into .rodata (PSRAM XIP on P4),
    // silently corrupting read-only memory on every touch release. Copy to
    // a stack buffer first — matches the pong/heartbeat paths above.
    char msg[16];
    strcpy(msg, "{\"t\":[]}");
    int ret = esp_transport_ws_send_raw(s_ws,
        WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
        msg, strlen(msg), 100);
    if (ret < 0) {
        ESP_LOGW(TAG, "Release send failed, marking disconnected");
        s_connected = false;
    }
    xSemaphoreGive(s_ws_mutex);
}

bool tab5_touch_ws_connected(void)
{
    return s_connected;
}

void tab5_touch_ws_set_disconnect_cb(touch_ws_disconnect_cb_t cb)
{
    s_disconnect_cb = cb;
}
