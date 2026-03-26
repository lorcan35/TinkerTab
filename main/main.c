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
#include <math.h>
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
#include "sdcard.h"
#include "camera.h"
#include "audio.h"
#include "imu.h"
#include "rtc.h"
#include "battery.h"
#include "bluetooth.h"

static const char *TAG = "tab5";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static bool s_wifi_connected = false;
static bool s_touch_ok = false;
static bool s_sd_ok = false;
static bool s_cam_ok = false;
static bool s_audio_ok = false;
static bool s_mic_ok = false;
static bool s_imu_ok = false;
static bool s_rtc_ok = false;
static bool s_bat_ok = false;

/** Public accessor for camera driver (and any driver needing the I2C bus). */
i2c_master_bus_handle_t tab5_get_i2c_bus(void)
{
    return s_i2c_bus;
}

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
    printf("  TinkerTab v1.0.0 — Full Hardware\n");
    printf("  ESP32-P4 | M5Stack Tab5\n");
    printf("  All Drivers | Standalone + Dragon\n");
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

    // Initialize SD card
    ESP_LOGI(TAG, "Initializing SD card...");
    ret = tab5_sdcard_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed: %s (continuing without SD)", esp_err_to_name(ret));
    } else {
        s_sd_ok = true;
        ESP_LOGI(TAG, "SD card mounted at %s (%.1f GB total, %.1f GB free)",
                 tab5_sdcard_mount_point(),
                 tab5_sdcard_total_bytes() / 1073741824.0,
                 tab5_sdcard_free_bytes() / 1073741824.0);
    }

    // Initialize camera (SC2336 MIPI-CSI)
    if (s_i2c_bus) {
        ESP_LOGI(TAG, "Initializing camera (SC2336)...");
        ret = tab5_camera_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Camera init failed: %s", esp_err_to_name(ret));
        } else {
            s_cam_ok = true;
            ESP_LOGI(TAG, "Camera initialized: %s", tab5_camera_info());
        }
    }

    // Initialize audio codec (ES8388) and speaker
    if (s_i2c_bus) {
        ESP_LOGI(TAG, "Initializing audio (ES8388)...");
        ret = tab5_audio_init(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(ret));
        } else {
            s_audio_ok = true;
            ESP_LOGI(TAG, "Audio codec initialized");
        }
    }

    // Initialize dual microphone (ES7210)
    if (s_i2c_bus) {
        ESP_LOGI(TAG, "Initializing mic (ES7210)...");
        ret = tab5_mic_init(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Mic init failed: %s", esp_err_to_name(ret));
        } else {
            s_mic_ok = true;
            ESP_LOGI(TAG, "Dual mic initialized");
        }
    }

    // Initialize IMU (BMI270)
    if (s_i2c_bus) {
        ESP_LOGI(TAG, "Initializing IMU (BMI270)...");
        ret = tab5_imu_init(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "IMU init failed: %s", esp_err_to_name(ret));
        } else {
            s_imu_ok = true;
            ESP_LOGI(TAG, "IMU initialized");
        }
    }

    // Initialize RTC (RX8130CE)
    if (s_i2c_bus) {
        ESP_LOGI(TAG, "Initializing RTC (RX8130CE)...");
        ret = tab5_rtc_init(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "RTC init failed: %s", esp_err_to_name(ret));
        } else {
            s_rtc_ok = true;
            tab5_rtc_time_t t;
            if (tab5_rtc_get_time(&t) == ESP_OK) {
                ESP_LOGI(TAG, "RTC time: 20%02d-%02d-%02d %02d:%02d:%02d",
                         t.year, t.month, t.day, t.hour, t.minute, t.second);
            }
        }
    }

    // Initialize battery monitor (INA226)
    if (s_i2c_bus) {
        ESP_LOGI(TAG, "Initializing battery monitor (INA226)...");
        ret = tab5_battery_init(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Battery monitor init failed: %s", esp_err_to_name(ret));
        } else {
            s_bat_ok = true;
            tab5_battery_info_t bi;
            if (tab5_battery_read(&bi) == ESP_OK) {
                ESP_LOGI(TAG, "Battery: %.2fV %d%% %s",
                         bi.voltage, bi.percent, bi.charging ? "(charging)" : "");
            }
        }
    }

    // Initialize BLE (via ESP32-C6 — stub until ESP-Hosted adds BLE forwarding)
    ESP_LOGI(TAG, "Initializing BLE...");
    ret = tab5_ble_init();
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "BLE not yet supported via ESP-Hosted (stub driver loaded)");
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed: %s", esp_err_to_name(ret));
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

    ESP_LOGI(TAG, "TinkerTab v1.0.0 running — WiFi=%s Touch=%s SD=%s Cam=%s Audio=%s Mic=%s IMU=%s RTC=%s Bat=%s",
             s_wifi_connected ? "Y" : "N", s_touch_ok ? "Y" : "N",
             s_sd_ok ? "Y" : "N", s_cam_ok ? "Y" : "N",
             s_audio_ok ? "Y" : "N", s_mic_ok ? "Y" : "N",
             s_imu_ok ? "Y" : "N", s_rtc_ok ? "Y" : "N", s_bat_ok ? "Y" : "N");
    printf("\nTinkerTab ready. Commands: info, heap, wifi, stream, scan,\n"
           "  red/green/blue/white/black, bright <0-100>, pattern [0-3],\n"
           "  touch, touchdiag, sd, cam, audio, mic, imu, rtc, bat, reboot\n\n");

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
                        printf("SD: %s\n", s_sd_ok ? "mounted" : "not mounted");
                        printf("Camera: %s\n", s_cam_ok ? "ready" : "not init");
                        printf("Audio: %s\n", s_audio_ok ? "ready" : "not init");
                        printf("Mic: %s\n", s_mic_ok ? "ready" : "not init");
                        printf("IMU: %s\n", s_imu_ok ? "ready" : "not init");
                        printf("RTC: %s\n", s_rtc_ok ? "ready" : "not init");
                        printf("Battery: %s\n", s_bat_ok ? "ready" : "not init");
                        printf("BLE: stub (waiting for ESP-Hosted support)\n");
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
                    } else if (strcmp(cmd_buf, "sd") == 0) {
                        if (s_sd_ok) {
                            printf("SD: mounted at %s\n", tab5_sdcard_mount_point());
                            printf("  Total: %.1f GB\n", tab5_sdcard_total_bytes() / 1073741824.0);
                            printf("  Free:  %.1f GB\n", tab5_sdcard_free_bytes() / 1073741824.0);
                        } else {
                            printf("SD: not mounted\n");
                        }
                    } else if (strcmp(cmd_buf, "cam") == 0) {
                        if (s_cam_ok) {
                            printf("Camera: %s\n", tab5_camera_info());
                            printf("Capturing frame...\n");
                            tab5_cam_frame_t frame;
                            if (tab5_camera_capture(&frame) == ESP_OK) {
                                printf("  %dx%d, %lu bytes\n", frame.width, frame.height, (unsigned long)frame.size);
                                if (s_sd_ok) {
                                    if (tab5_camera_save_jpeg(&frame, "/sdcard/capture.jpg") == ESP_OK) {
                                        printf("  Saved to /sdcard/capture.jpg\n");
                                    }
                                }
                            } else {
                                printf("  Capture failed\n");
                            }
                        } else {
                            printf("Camera: not initialized\n");
                        }
                    } else if (strcmp(cmd_buf, "audio") == 0) {
                        if (s_audio_ok) {
                            printf("Audio: ES8388 initialized, volume=%d%%\n", tab5_audio_get_volume());
                            printf("Playing test tone...\n");
                            // Generate 440Hz sine wave test tone (0.5s)
                            const int sample_rate = 16000;
                            const int duration_samples = sample_rate / 2;
                            int16_t *tone = heap_caps_malloc(duration_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                            if (tone) {
                                for (int i = 0; i < duration_samples; i++) {
                                    tone[i] = (int16_t)(16000 * sinf(2.0f * 3.14159f * 440.0f * i / sample_rate));
                                }
                                tab5_audio_speaker_enable(true);
                                tab5_audio_play_raw(tone, duration_samples);
                                tab5_audio_speaker_enable(false);
                                free(tone);
                                printf("Done.\n");
                            } else {
                                printf("  Failed to allocate tone buffer\n");
                            }
                        } else {
                            printf("Audio: not initialized\n");
                        }
                    } else if (strcmp(cmd_buf, "mic") == 0) {
                        if (s_mic_ok) {
                            printf("Mic: ES7210 initialized\n");
                            printf("Recording 1s...\n");
                            const int samples = 16000;
                            int16_t *buf = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                            if (buf) {
                                if (tab5_mic_read(buf, samples, 2000) == ESP_OK) {
                                    // Calculate RMS level
                                    int64_t sum = 0;
                                    for (int i = 0; i < samples; i++) {
                                        sum += (int64_t)buf[i] * buf[i];
                                    }
                                    float rms = sqrtf((float)(sum / samples));
                                    printf("  RMS level: %.0f\n", rms);
                                } else {
                                    printf("  Read failed\n");
                                }
                                free(buf);
                            }
                        } else {
                            printf("Mic: not initialized\n");
                        }
                    } else if (strcmp(cmd_buf, "imu") == 0) {
                        if (s_imu_ok) {
                            printf("IMU: BMI270\n");
                            tab5_imu_data_t d;
                            if (tab5_imu_read(&d) == ESP_OK) {
                                printf("  Accel: x=%.2fg y=%.2fg z=%.2fg\n", d.accel.x, d.accel.y, d.accel.z);
                                printf("  Gyro:  x=%.1f y=%.1f z=%.1f dps\n", d.gyro.x, d.gyro.y, d.gyro.z);
                            }
                            printf("  Orientation: %s\n",
                                tab5_imu_get_orientation() == TAB5_ORIENT_PORTRAIT ? "portrait" :
                                tab5_imu_get_orientation() == TAB5_ORIENT_LANDSCAPE ? "landscape" :
                                tab5_imu_get_orientation() == TAB5_ORIENT_PORTRAIT_INV ? "portrait-inv" : "landscape-inv");
                        } else {
                            printf("IMU: not initialized\n");
                        }
                    } else if (strcmp(cmd_buf, "rtc") == 0) {
                        if (s_rtc_ok) {
                            tab5_rtc_time_t t;
                            if (tab5_rtc_get_time(&t) == ESP_OK) {
                                printf("RTC: 20%02d-%02d-%02d %02d:%02d:%02d (day %d)\n",
                                       t.year, t.month, t.day, t.hour, t.minute, t.second, t.weekday);
                            } else {
                                printf("RTC: read failed\n");
                            }
                        } else {
                            printf("RTC: not initialized\n");
                        }
                    } else if (strcmp(cmd_buf, "ntp") == 0) {
                        if (s_rtc_ok && s_wifi_connected) {
                            printf("Syncing RTC from NTP...\n");
                            if (tab5_rtc_sync_from_ntp() == ESP_OK) {
                                tab5_rtc_time_t t;
                                tab5_rtc_get_time(&t);
                                printf("RTC synced: 20%02d-%02d-%02d %02d:%02d:%02d UTC\n",
                                       t.year, t.month, t.day, t.hour, t.minute, t.second);
                            } else {
                                printf("NTP sync failed\n");
                            }
                        } else {
                            printf("Need RTC + WiFi for NTP sync\n");
                        }
                    } else if (strcmp(cmd_buf, "bat") == 0) {
                        if (s_bat_ok) {
                            tab5_battery_info_t bi;
                            if (tab5_battery_read(&bi) == ESP_OK) {
                                printf("Battery: %.2fV %.2fA %.2fW %d%% %s\n",
                                       bi.voltage, bi.current, bi.power, bi.percent,
                                       bi.charging ? "(charging)" : "(discharging)");
                            } else {
                                printf("Battery: read failed\n");
                            }
                        } else {
                            printf("Battery: not initialized\n");
                        }
                    } else if (strcmp(cmd_buf, "reboot") == 0) {
                        printf("Rebooting...\n");
                        vTaskDelay(pdMS_TO_TICKS(100));
                        esp_restart();
                    } else {
                        printf("Unknown: %s\n", cmd_buf);
                        printf("Commands: info, heap, wifi, stream, scan,\n"
                               "  red/green/blue/white/black, bright <0-100>, pattern [0-3],\n"
                               "  touch, touchdiag, sd, cam, audio, mic, imu, rtc, ntp, bat, reboot\n");
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
