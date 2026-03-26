/**
 * TinkerClaw Tab5 — MJPEG Stream Consumer
 *
 * Connects to Dragon's streaming server via HTTP, reads the MJPEG stream
 * (multipart/x-mixed-replace), extracts JPEG frames, and pushes them
 * to the display.
 *
 * Protocol:
 *   GET /stream HTTP/1.1
 *   Response: multipart/x-mixed-replace; boundary=frame
 *   Each part: Content-Type: image/jpeg + JPEG data
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

// JPEG frame buffer in PSRAM
static uint8_t *s_jpeg_buf = NULL;

/**
 * Parse MJPEG multipart stream.
 *
 * MJPEG streams use multipart/x-mixed-replace:
 *   --frame\r\n
 *   Content-Type: image/jpeg\r\n
 *   Content-Length: 12345\r\n
 *   \r\n
 *   <JPEG data>
 *   \r\n--frame\r\n
 *   ...
 *
 * We look for JPEG SOI (0xFF 0xD8) and EOI (0xFF 0xD9) markers as a
 * simpler and more robust approach than parsing multipart headers.
 */
static void mjpeg_stream_task(void *arg)
{
    ESP_LOGI(TAG, "MJPEG task started");

    s_jpeg_buf = (uint8_t *)heap_caps_malloc(TAB5_JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_jpeg_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG buffer");
        vTaskDelete(NULL);
        return;
    }

    // Build URL
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             TAB5_DRAGON_HOST, TAB5_DRAGON_PORT, TAB5_STREAM_PATH);

    while (1) {
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
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Connected! Status: %d, Content-Length: %d", status, content_length);

        if (status != 200) {
            ESP_LOGE(TAG, "Bad status: %d", status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Read stream — find JPEG frames by SOI/EOI markers
        size_t jpeg_pos = 0;
        bool in_jpeg = false;
        uint8_t read_buf[4096];
        s_last_fps_time = esp_timer_get_time();
        s_frame_count = 0;

        while (1) {
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
                    // Look for SOI marker
                    if (i + 1 < read_len && read_buf[i] == 0xFF && read_buf[i + 1] == 0xD8) {
                        in_jpeg = true;
                        jpeg_pos = 0;
                        s_jpeg_buf[jpeg_pos++] = 0xFF;
                        s_jpeg_buf[jpeg_pos++] = 0xD8;
                        i++; // Skip second byte of SOI
                    }
                } else {
                    // Accumulate JPEG data
                    if (jpeg_pos < TAB5_JPEG_BUF_SIZE) {
                        s_jpeg_buf[jpeg_pos++] = read_buf[i];
                    }

                    // Check for EOI marker
                    if (jpeg_pos >= 2 &&
                        s_jpeg_buf[jpeg_pos - 2] == 0xFF &&
                        s_jpeg_buf[jpeg_pos - 1] == 0xD9) {
                        // Complete JPEG frame — decode and display
                        esp_err_t ret = tab5_display_draw_jpeg(s_jpeg_buf, jpeg_pos);
                        if (ret == ESP_OK) {
                            s_frame_count++;

                            // Update FPS every second
                            int64_t now = esp_timer_get_time();
                            int64_t elapsed = now - s_last_fps_time;
                            if (elapsed >= 1000000) {  // 1 second
                                s_fps = (float)s_frame_count * 1000000.0f / (float)elapsed;
                                s_frame_count = 0;
                                s_last_fps_time = now;
                            }
                        } else {
                            ESP_LOGW(TAG, "Frame decode failed (size=%d)", jpeg_pos);
                        }

                        in_jpeg = false;
                        jpeg_pos = 0;
                    }
                }
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        ESP_LOGW(TAG, "Stream disconnected, reconnecting in 2s...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void tab5_mjpeg_start(void)
{
    xTaskCreatePinnedToCore(
        mjpeg_stream_task,
        "mjpeg",
        8192,       // 8KB stack
        NULL,
        5,          // High priority
        NULL,
        1           // Pin to core 1 (core 0 for WiFi)
    );
}

float tab5_mjpeg_get_fps(void)
{
    return s_fps;
}
