/**
 * TinkerTab v0.4.0 — Phase 2: WiFi + MJPEG + Touch Forwarding
 *
 * 1. Boot on ESP32-P4
 * 2. Initialize I2C, IO expanders, display (720x1280 ST7123), touch
 * 3. Connect to WiFi via ESP32-C6 co-processor (ESP-Hosted SDIO)
 * 4. Stream MJPEG from Dragon, decode with HW JPEG, display at ~20fps
 * 5. Forward touch events to Dragon via WebSocket
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

#include "config.h"
#include "io_expander.h"
#include "display.h"
#include "touch.h"
#include "wifi.h"
#include "mjpeg_stream.h"
#include "touch_ws.h"

static const char *TAG = "tab5";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static bool s_wifi_connected = false;
static bool s_touch_ok = false;

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = TAB5_I2C_NUM,
        .sda_io_num = TAB5_I2C_SDA,
        .scl_io_num = TAB5_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

// Background task: continuously read touch and forward via WebSocket
static void touch_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Touch poll task started");
    while (1) {
        tab5_touch_point_t pts[TAB5_TOUCH_MAX_POINTS];
        uint8_t cnt = 0;
        if (tab5_touch_read(pts, &cnt) && cnt > 0) {
            // Forward to Dragon if WebSocket is connected
            if (tab5_touch_ws_connected()) {
                tab5_touch_ws_send(pts, cnt);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));  // 50Hz touch polling
    }
}

void app_main(void)
{
    printf("\n\n");
    printf("========================================\n");
    printf("  TinkerTab v0.4.0 — Phase 2\n");
    printf("  ESP32-P4 | M5Stack Tab5\n");
    printf("  720x1280 ST7123 | WiFi | MJPEG\n");
    printf("========================================\n\n");

    // Print chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "ESP32-P4 rev %d.%d, %d cores", chip_info.revision / 100, chip_info.revision % 100, chip_info.cores);
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %lu bytes", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize I2C bus
    ESP_LOGI(TAG, "Initializing I2C (SDA=%d, SCL=%d)...", TAB5_I2C_SDA, TAB5_I2C_SCL);
    ret = init_i2c();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing without I2C peripherals");
    } else {
        ESP_LOGI(TAG, "I2C initialized");

        // Scan I2C bus
        ESP_LOGI(TAG, "Scanning I2C bus...");
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
                ESP_LOGI(TAG, "  I2C device found at 0x%02X", addr);
            }
        }

        // Initialize IO expanders
        ret = tab5_io_expander_init(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "IO expander init failed: %s", esp_err_to_name(ret));
        } else {
            tab5_reset_display_and_touch();
            vTaskDelay(pdMS_TO_TICKS(300));

            // Enable WiFi power via IO expander (powers up ESP32-C6)
            tab5_set_wifi_power(true);
            ESP_LOGI(TAG, "WiFi power enabled (C6 co-processor)");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // Initialize MIPI DSI display
    ESP_LOGI(TAG, "Initializing display...");
    ret = tab5_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Display initialized!");
        tab5_display_fill_color(0x001F);  // Blue = "booting"

        // Initialize hardware JPEG decoder
        ret = tab5_display_jpeg_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "JPEG decoder init failed: %s (MJPEG streaming will not work)", esp_err_to_name(ret));
        }
    }

    // Initialize touch
    if (s_i2c_bus) {
        ESP_LOGI(TAG, "Initializing touch...");
        ret = tab5_touch_init(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Touch init failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Touch initialized!");
            s_touch_ok = true;
        }
    }

    // Initialize WiFi (ESP-Hosted → ESP32-C6 over SDIO)
    ESP_LOGI(TAG, "Initializing WiFi (SSID: %s)...", TAB5_WIFI_SSID);
    tab5_display_fill_color(0xFFE0);  // Yellow = "connecting WiFi"
    ret = tab5_wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        tab5_display_fill_color(0xF800);  // Red = error
    } else {
        ESP_LOGI(TAG, "WiFi started, waiting for connection...");
        ret = tab5_wifi_wait_connected(15000);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected!");
            tab5_display_fill_color(0x07E0);  // Green = connected
            s_wifi_connected = true;
            vTaskDelay(pdMS_TO_TICKS(500));

            // Start MJPEG streaming from Dragon
            ESP_LOGI(TAG, "Starting MJPEG stream from %s:%d%s",
                     TAB5_DRAGON_HOST, TAB5_DRAGON_PORT, TAB5_STREAM_PATH);
            tab5_mjpeg_start();

            // Start WebSocket touch forwarding
            ESP_LOGI(TAG, "Starting touch WebSocket to %s:%d%s",
                     TAB5_DRAGON_HOST, TAB5_DRAGON_PORT, TAB5_TOUCH_WS_PATH);
            tab5_touch_ws_start();
        } else {
            ESP_LOGW(TAG, "WiFi connect failed/timeout — offline mode");
            tab5_display_fill_color(0xF800);  // Red
            vTaskDelay(pdMS_TO_TICKS(1000));
            tab5_display_fill_color(0x0000);  // Black
        }
    }

    // Start touch polling task (forwards to WebSocket when connected)
    if (s_touch_ok) {
        xTaskCreatePinnedToCore(touch_poll_task, "touch_poll", 4096, NULL, 3, NULL, 1);
    }

    ESP_LOGI(TAG, "TinkerTab Phase 2 running. WiFi=%s Touch=%s",
             s_wifi_connected ? "YES" : "NO", s_touch_ok ? "YES" : "NO");
    printf("\nTinkerTab ready. Commands: info, heap, red/green/blue/white/black, bright <0-100>, "
           "pattern [0-3], touch, touchdiag, stream, wifi, reboot, scan\n\n");

    // Serial command loop
    char cmd_buf[128];
    while (1) {
        int c = getchar();
        if (c != EOF) {
            static int pos = 0;
            if (c == '\n' || c == '\r') {
                cmd_buf[pos] = '\0';
                if (pos > 0) {
                    if (strcmp(cmd_buf, "info") == 0) {
                        printf("Chip: ESP32-P4 rev %d.%d\n", chip_info.revision / 100, chip_info.revision % 100);
                        printf("Cores: %d\n", chip_info.cores);
                        printf("Free heap: %lu\n", (unsigned long)esp_get_free_heap_size());
                        printf("Free PSRAM: %lu\n", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                        printf("Display: 720x1280 MIPI DSI ST7123\n");
                        printf("WiFi: %s\n", s_wifi_connected ? "connected" : "disconnected");
                        printf("Touch: %s\n", s_touch_ok ? "active" : "inactive");
                        printf("WS: %s\n", tab5_touch_ws_connected() ? "connected" : "disconnected");
                    } else if (strcmp(cmd_buf, "heap") == 0) {
                        printf("Heap: %lu / PSRAM: %lu\n",
                               (unsigned long)esp_get_free_heap_size(),
                               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                    } else if (strcmp(cmd_buf, "wifi") == 0) {
                        printf("WiFi: %s, SSID: %s\n",
                               s_wifi_connected ? "connected" : "disconnected", TAB5_WIFI_SSID);
                        printf("Dragon: %s:%d\n", TAB5_DRAGON_HOST, TAB5_DRAGON_PORT);
                        printf("WS touch: %s\n", tab5_touch_ws_connected() ? "connected" : "disconnected");
                    } else if (strcmp(cmd_buf, "stream") == 0) {
                        printf("MJPEG FPS: %.1f\n", tab5_mjpeg_get_fps());
                    } else if (strcmp(cmd_buf, "red") == 0) {
                        tab5_display_fill_color(0xF800);
                        printf("Display: red\n");
                    } else if (strcmp(cmd_buf, "green") == 0) {
                        tab5_display_fill_color(0x07E0);
                        printf("Display: green\n");
                    } else if (strcmp(cmd_buf, "blue") == 0) {
                        tab5_display_fill_color(0x001F);
                        printf("Display: blue\n");
                    } else if (strcmp(cmd_buf, "white") == 0) {
                        tab5_display_fill_color(0xFFFF);
                        printf("Display: white\n");
                    } else if (strcmp(cmd_buf, "black") == 0) {
                        tab5_display_fill_color(0x0000);
                        printf("Display: black\n");
                    } else if (strcmp(cmd_buf, "scan") == 0) {
                        printf("Scanning I2C bus...\n");
                        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
                            if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
                                printf("  Device at 0x%02X\n", addr);
                            }
                        }
                        printf("Scan complete.\n");
                    } else if (strncmp(cmd_buf, "bright ", 7) == 0) {
                        int val = atoi(cmd_buf + 7);
                        tab5_display_set_brightness(val);
                        printf("Brightness: %d%%\n", val);
                    } else if (strncmp(cmd_buf, "pattern ", 8) == 0) {
                        int val = atoi(cmd_buf + 8);
                        tab5_display_test_pattern(val);
                        printf("Pattern: %d\n", val);
                    } else if (strcmp(cmd_buf, "pattern") == 0) {
                        tab5_display_test_pattern(1);
                        printf("Pattern: vertical bars\n");
                    } else if (strcmp(cmd_buf, "touchdiag") == 0) {
                        printf("Touch diagnostics...\n");
                        tab5_touch_diag();
                        printf("Now polling 5s (verbose)...\n");
                        int64_t start = esp_timer_get_time();
                        int polls = 0, hits = 0;
                        while ((esp_timer_get_time() - start) < 5000000) {
                            tab5_touch_point_t pts[TAB5_TOUCH_MAX_POINTS];
                            uint8_t cnt = 0;
                            bool got = tab5_touch_read(pts, &cnt);
                            polls++;
                            if (got) {
                                hits++;
                                for (int i = 0; i < cnt; i++) {
                                    printf("  T%d: x=%d y=%d s=%d\n", i, pts[i].x, pts[i].y, pts[i].strength);
                                }
                            }
                            if (polls % 50 == 0) {
                                tab5_touch_diag();
                                printf("  [polls=%d hits=%d]\n", polls, hits);
                            }
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        printf("Done. polls=%d hits=%d\n", polls, hits);
                    } else if (strcmp(cmd_buf, "touch") == 0) {
                        printf("Touch test (5 seconds)...\n");
                        int64_t start = esp_timer_get_time();
                        while ((esp_timer_get_time() - start) < 5000000) {
                            tab5_touch_point_t pts[TAB5_TOUCH_MAX_POINTS];
                            uint8_t cnt = 0;
                            if (tab5_touch_read(pts, &cnt)) {
                                for (int i = 0; i < cnt; i++) {
                                    printf("  T%d: x=%d y=%d s=%d\n", i, pts[i].x, pts[i].y, pts[i].strength);
                                }
                            }
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        printf("Touch test done.\n");
                    } else if (strcmp(cmd_buf, "reboot") == 0) {
                        printf("Rebooting...\n");
                        vTaskDelay(pdMS_TO_TICKS(100));
                        esp_restart();
                    } else {
                        printf("Unknown: %s\n", cmd_buf);
                        printf("Commands: info, heap, wifi, stream, red/green/blue/white/black, "
                               "bright <0-100>, pattern [0-3], touch, touchdiag, reboot, scan\n");
                    }
                }
                pos = 0;
            } else if (pos < (int)sizeof(cmd_buf) - 1) {
                cmd_buf[pos++] = (char)c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
