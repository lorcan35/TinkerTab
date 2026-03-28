/**
 * TinkerClaw Tab5 — MJPEG Stream Consumer (Optimized)
 *
 * Connects to Dragon's streaming server via HTTP, reads the MJPEG stream
 * (multipart/x-mixed-replace), extracts JPEG frames, and pushes them
 * to the display via hardware JPEG decoder.
 *
 * Optimizations:
 *  - DMA-aligned JPEG buffer in PSRAM
 *  - 16KB read buffer for fewer syscalls
 *  - Frame age tracking — skip stale frames (>50ms old)
 *  - Runs on Core 1 at high priority, separate from LVGL on Core 0
 */

#include "mjpeg_stream.h"
#include "config.h"
#include "settings.h"
#include "display.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "tab5_mjpeg";

/* Read buffer: larger = fewer read() calls = less overhead */
#define READ_BUF_SIZE  16384

/* Max frame age before we skip decode (microseconds) */
#define FRAME_MAX_AGE_US  50000

static float s_fps = 0.0f;
static uint32_t s_frame_count = 0;
static uint32_t s_drop_count = 0;
static int64_t s_last_fps_time = 0;
static volatile bool s_running = false;
static volatile bool s_stop_flag = false;
static TaskHandle_t s_task_handle = NULL;
static mjpeg_disconnect_cb_t s_disconnect_cb = NULL;

/* JPEG frame buffer in PSRAM (DMA-aligned) */
static uint8_t *s_jpeg_buf = NULL;

static void mjpeg_stream_task(void *arg)
{
    ESP_LOGI(TAG, "MJPEG task started");

    /* Allocate JPEG buffer — DMA-aligned in PSRAM for hardware decoder */
    s_jpeg_buf = (uint8_t *)heap_caps_aligned_alloc(
        64,  /* cache line alignment for ESP32-P4 */
        TAB5_JPEG_BUF_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_jpeg_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG buffer (%d bytes)", TAB5_JPEG_BUF_SIZE);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Read buffer — PSRAM to preserve internal DMA RAM for SDIO WiFi (#18) */
    uint8_t *read_buf = (uint8_t *)heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!read_buf) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        heap_caps_free(s_jpeg_buf);
        s_jpeg_buf = NULL;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Build URL from NVS-backed settings */
    char dragon_host[64];
    tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
    uint16_t dragon_port = tab5_settings_get_dragon_port();

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             dragon_host, dragon_port, TAB5_STREAM_PATH);

    while (!s_stop_flag) {
        ESP_LOGI(TAG, "Connecting to %s", url);

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = TAB5_FRAME_TIMEOUT_MS,
            .buffer_size = 4096,   /* Smaller HTTP buffer to save internal DMA RAM (#18) */
            .buffer_size_tx = 256,
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

        /* Stream connected — read JPEG frames by SOI/EOI markers */
        size_t jpeg_pos = 0;
        bool in_jpeg = false;
        int64_t frame_start_time = 0;
        s_last_fps_time = esp_timer_get_time();
        s_frame_count = 0;
        s_drop_count = 0;

        while (!s_stop_flag) {
            int read_len = esp_http_client_read(client, (char *)read_buf, READ_BUF_SIZE);
            if (read_len <= 0) {
                if (read_len == 0) {
                    ESP_LOGW(TAG, "Stream ended");
                } else {
                    ESP_LOGE(TAG, "Read error: %d", read_len);
                }
                break;
            }

            /* Scan for JPEG SOI (FF D8) and EOI (FF D9) */
            for (int i = 0; i < read_len; i++) {
                if (!in_jpeg) {
                    if (i + 1 < read_len && read_buf[i] == 0xFF && read_buf[i + 1] == 0xD8) {
                        in_jpeg = true;
                        jpeg_pos = 0;
                        frame_start_time = esp_timer_get_time();

                        /* Count every detected frame for FPS (regardless of decode) */
                        s_frame_count++;
                        int64_t now_soi = esp_timer_get_time();
                        int64_t elapsed_soi = now_soi - s_last_fps_time;
                        if (elapsed_soi >= 1000000) {
                            s_fps = (float)s_frame_count * 1000000.0f / (float)elapsed_soi;
                            s_frame_count = 0;
                            s_last_fps_time = now_soi;
                        }
                        s_jpeg_buf[jpeg_pos++] = 0xFF;
                        s_jpeg_buf[jpeg_pos++] = 0xD8;
                        i++;
                    }
                } else {
                    if (jpeg_pos < TAB5_JPEG_BUF_SIZE) {
                        s_jpeg_buf[jpeg_pos++] = read_buf[i];
                    }

                    /* Found EOI — complete frame */
                    if (jpeg_pos >= 2 &&
                        s_jpeg_buf[jpeg_pos - 2] == 0xFF &&
                        s_jpeg_buf[jpeg_pos - 1] == 0xD9) {

                        int64_t now = esp_timer_get_time();
                        int64_t frame_age = now - frame_start_time;

                        /* Skip stale frames — always decode the freshest */
                        if (frame_age > FRAME_MAX_AGE_US) {
                            s_drop_count++;
                            in_jpeg = false;
                            jpeg_pos = 0;
                            continue;
                        }

                        /* Skip decode when JPEG rendering is disabled (avoids log spam) */
                        if (!tab5_display_is_jpeg_enabled()) {
                            s_drop_count++;
                            in_jpeg = false;
                            jpeg_pos = 0;
                            continue;
                        }

                        /* Decode and render */
                        esp_err_t ret = tab5_display_draw_jpeg(s_jpeg_buf, jpeg_pos);
                        if (ret == ESP_OK) {
                            if (s_drop_count > 0) {
                                ESP_LOGI(TAG, "%.1f FPS (dropped %lu stale)", s_fps, (unsigned long)s_drop_count);
                                s_drop_count = 0;
                            }
                        } else {
                            ESP_LOGW(TAG, "Decode failed (size=%zu): %s", jpeg_pos, esp_err_to_name(ret));
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

        /* Stream disconnected — retry after delay instead of exiting.
         * vTaskDelete from Core 1 can crash IDLE1 if heap is fragmented (#18). */
        ESP_LOGW(TAG, "Stream disconnected, retrying in 2s...");
        if (s_disconnect_cb) {
            s_disconnect_cb();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;  /* Retry connection */
    }

    /* Cleanup */
    if (read_buf) {
        heap_caps_free(read_buf);
    }
    if (s_jpeg_buf) {
        heap_caps_free(s_jpeg_buf);
        s_jpeg_buf = NULL;
    }
    s_fps = 0.0f;
    s_running = false;
    s_task_handle = NULL;

    /* Disconnect callback already called in retry loop if stream dropped.
     * Only call here if we're stopping cleanly without prior disconnect. */

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
        16384,     /* 16KB stack — HTTP client + JPEG parsing + retry loop (#18) */
        NULL,
        configMAX_PRIORITIES - 2,  /* high priority on Core 1 */
        &s_task_handle,
        1          /* Core 1 — away from LVGL on Core 0 */
    );
}

void tab5_mjpeg_stop(void)
{
    if (!s_running) return;
    s_stop_flag = true;
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
