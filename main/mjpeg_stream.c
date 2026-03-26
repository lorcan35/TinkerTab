/**
 * TinkerClaw Tab5 — MJPEG Stream Consumer
 *
 * Connects to Dragon's streaming server via HTTP, reads the MJPEG stream
 * (multipart/x-mixed-replace), extracts JPEG frames, and pushes them
 * to the display via hardware JPEG decoder.
 */

#include "mjpeg_stream.h"
#include "config.h"
#include "display.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "tab5_mjpeg";

static float s_fps = 0.0f;
static uint32_t s_frame_count = 0;
static int64_t s_last_fps_time = 0;
static volatile bool s_running = false;
static volatile bool s_stop_flag = false;
static TaskHandle_t s_task_handle = NULL;
static mjpeg_disconnect_cb_t s_disconnect_cb = NULL;

// JPEG frame buffer in PSRAM
static uint8_t *s_jpeg_buf = NULL;

static void mjpeg_stream_task(void *arg)
{
    ESP_LOGI(TAG, "MJPEG task started");

    s_jpeg_buf = (uint8_t *)heap_caps_malloc(TAB5_JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_jpeg_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG buffer");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Build URL
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             TAB5_DRAGON_HOST, TAB5_DRAGON_PORT, TAB5_STREAM_PATH);

    while (!s_stop_flag) {
        ESP_LOGI(TAG, "Connecting to %s", url);

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = TAB5_FRAME_TIMEOUT_MS,
            .buffer_size = 16384,
            .buffer_size_tx = 1024,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "HTTP client init failed");
            break;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            break;
        }

        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Connected! Status: %d, Content-Length: %d", status, content_length);

        if (status != 200) {
            ESP_LOGE(TAG, "Bad status: %d", status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            break;
        }

        // Read stream — find JPEG frames by SOI/EOI markers
        size_t jpeg_pos = 0;
        bool in_jpeg = false;
        uint8_t read_buf[4096];
        s_last_fps_time = esp_timer_get_time();
        s_frame_count = 0;

        while (!s_stop_flag) {
            int read_len = esp_http_client_read(client, (char *)read_buf, sizeof(read_buf));
            if (read_len <= 0) {
                if (read_len == 0) {
                    ESP_LOGW(TAG, "Stream ended");
                } else {
                    ESP_LOGE(TAG, "Read error: %d", read_len);
                }
                break;
            }

            // Scan for JPEG SOI (FF D8) and EOI (FF D9)
            for (int i = 0; i < read_len; i++) {
                if (!in_jpeg) {
                    if (i + 1 < read_len && read_buf[i] == 0xFF && read_buf[i + 1] == 0xD8) {
                        in_jpeg = true;
                        jpeg_pos = 0;
                        s_jpeg_buf[jpeg_pos++] = 0xFF;
                        s_jpeg_buf[jpeg_pos++] = 0xD8;
                        i++;
                    }
                } else {
                    if (jpeg_pos < TAB5_JPEG_BUF_SIZE) {
                        s_jpeg_buf[jpeg_pos++] = read_buf[i];
                    }

                    if (jpeg_pos >= 2 &&
                        s_jpeg_buf[jpeg_pos - 2] == 0xFF &&
                        s_jpeg_buf[jpeg_pos - 1] == 0xD9) {
                        esp_err_t ret = tab5_display_draw_jpeg(s_jpeg_buf, jpeg_pos);
                        if (ret == ESP_OK) {
                            s_frame_count++;
                            int64_t now = esp_timer_get_time();
                            int64_t elapsed = now - s_last_fps_time;
                            if (elapsed >= 1000000) {
                                s_fps = (float)s_frame_count * 1000000.0f / (float)elapsed;
                                s_frame_count = 0;
                                s_last_fps_time = now;
                            }
                        } else {
                            ESP_LOGW(TAG, "Frame decode failed (size=%zu)", jpeg_pos);
                        }
                        in_jpeg = false;
                        jpeg_pos = 0;
                    }
                }
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (s_stop_flag) break;

        // Stream disconnected — notify dragon_link instead of retrying internally
        ESP_LOGW(TAG, "Stream disconnected");
        break;
    }

    // Cleanup
    if (s_jpeg_buf) {
        heap_caps_free(s_jpeg_buf);
        s_jpeg_buf = NULL;
    }
    s_fps = 0.0f;
    s_running = false;
    s_task_handle = NULL;

    // Notify dragon_link of disconnect (unless we were told to stop)
    if (!s_stop_flag && s_disconnect_cb) {
        s_disconnect_cb();
    }

    ESP_LOGI(TAG, "MJPEG task exiting");
    vTaskDelete(NULL);
}

void tab5_mjpeg_start(void)
{
    if (s_running) return;
    s_stop_flag = false;
    s_running = true;

    xTaskCreatePinnedToCore(
        mjpeg_stream_task,
        "mjpeg",
        8192,
        NULL,
        5,
        &s_task_handle,
        1   // Core 1
    );
}

void tab5_mjpeg_stop(void)
{
    if (!s_running) return;
    s_stop_flag = true;
    // Task will exit on next read timeout or loop iteration
}

bool tab5_mjpeg_is_running(void)
{
    return s_running;
}

float tab5_mjpeg_get_fps(void)
{
    return s_fps;
}

void tab5_mjpeg_set_disconnect_cb(mjpeg_disconnect_cb_t cb)
{
    s_disconnect_cb = cb;
}
