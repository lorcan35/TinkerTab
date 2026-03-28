/**
 * TinkerTab v1.0.0 — Full Hardware + LVGL Native UI
 *
 * 1. Boot on ESP32-P4 — init all hardware (I2C, IO, display, touch, SD, camera, audio, IMU, RTC, battery)
 * 2. Show boot splash screen via LVGL
 * 3. Connect to WiFi via ESP32-C6 co-processor (ESP-Hosted SDIO)
 * 4. Transition to home screen (native LVGL launcher)
 * 5. If Dragon detected: switch to MJPEG streaming mode
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

#include "config.h"
#include "io_expander.h"
#include "display.h"
#include "touch.h"
#include "wifi.h"
#include "dragon_link.h"
#include "mjpeg_stream.h"
#include "touch_ws.h"
#include "sdcard.h"
#include "camera.h"
#include "audio.h"
#include "imu.h"
#include "rtc.h"
#include "battery.h"
#include "bluetooth.h"
#include "ui_core.h"
#include "ui_splash.h"
#include "ui_home.h"
#include "ui_keyboard.h"
#include "ui_voice.h"
#include "settings.h"
#include "debug_server.h"
#include "voice.h"
#include "mode_manager.h"
#include "service_registry.h"

static const char *TAG = "tab5";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
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

// Touch polling handled by LVGL indev driver (no WebSocket forwarding without WiFi)

// Deferred overlay init — runs on LVGL timer task to avoid main stack overflow
static void deferred_overlay_init_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    ESP_LOGI(TAG, "Creating overlays (deferred)...");
    ui_keyboard_init(NULL);
    ui_voice_init();
    ESP_LOGI(TAG, "Keyboard + Voice UI overlays initialized");
}

// Voice E2E test task — uses mode manager for clean MJPEG↔voice switching
static void voice_test_task(void *arg)
{
    printf("[voice_test] Mode: %s, heap: %lu\n",
           tab5_mode_str(), (unsigned long)esp_get_free_heap_size());

    // Switch to VOICE mode — stops MJPEG, starts voice connect
    printf("[voice_test] Switching to VOICE mode...\n");
    esp_err_t mr = tab5_mode_switch(MODE_VOICE);
    if (mr != ESP_OK) {
        printf("[voice_test] Mode switch failed: %s\n", esp_err_to_name(mr));
        vTaskSuspend(NULL);
        return;
    }
    printf("[voice_test] Now in %s mode, heap: %lu\n",
           tab5_mode_str(), (unsigned long)esp_get_free_heap_size());

    // voice_connect_async was called by mode_switch — wait for connection
    for (int w = 0; w < 20; w++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        voice_state_t vs = voice_get_state();
        if (vs == VOICE_STATE_LISTENING || vs == VOICE_STATE_READY) {
            printf("[voice_test] Voice connected! state=%d\n", vs);
            break;
        }
        if (vs == VOICE_STATE_IDLE && w > 4) {
            printf("[voice_test] Voice failed to connect\n");
            goto done;
        }
    }

    // Record for 3 seconds
    voice_state_t vs = voice_get_state();
    if (vs == VOICE_STATE_READY) {
        voice_start_listening();
    }
    if (voice_get_state() == VOICE_STATE_LISTENING) {
        printf("[voice_test] Recording 3s...\n");
        vTaskDelay(pdMS_TO_TICKS(3000));
        voice_stop_listening();
        printf("[voice_test] Stopped, waiting for Dragon response...\n");

        // Wait for processing + playback to complete
        for (int w = 0; w < 60; w++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            vs = voice_get_state();
            if (vs == VOICE_STATE_READY || vs == VOICE_STATE_IDLE) {
                printf("[voice_test] Done! Transcript: %s\n",
                       voice_get_last_transcript());
                break;
            }
            if (w % 4 == 0) printf("[voice_test] waiting... state=%d\n", vs);
        }
    }

done:
    // Switch back to STREAMING — stops voice, resumes MJPEG
    printf("[voice_test] Switching back to STREAMING...\n");
    tab5_mode_switch(MODE_STREAMING);
    printf("[voice_test] Complete. Mode: %s\n", tab5_mode_str());
    vTaskSuspend(NULL);  /* P4 TLSP workaround (#20) */
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

    // Log human-readable reset reason for crash diagnostics
    static const char *reset_reason_str[] = {
        [ESP_RST_UNKNOWN]   = "UNKNOWN",
        [ESP_RST_POWERON]   = "POWERON",
        [ESP_RST_EXT]       = "EXTERNAL",
        [ESP_RST_SW]        = "SOFTWARE",
        [ESP_RST_PANIC]     = "PANIC (crash!)",
        [ESP_RST_INT_WDT]   = "INTERRUPT_WDT (crash!)",
        [ESP_RST_TASK_WDT]  = "TASK_WDT (crash!)",
        [ESP_RST_WDT]       = "OTHER_WDT (crash!)",
        [ESP_RST_DEEPSLEEP] = "DEEPSLEEP",
        [ESP_RST_BROWNOUT]  = "BROWNOUT",
        [ESP_RST_SDIO]      = "SDIO",
    };
    esp_reset_reason_t rst = esp_reset_reason();
    const char *rst_str = (rst < sizeof(reset_reason_str)/sizeof(reset_reason_str[0]) && reset_reason_str[rst])
                          ? reset_reason_str[rst] : "UNKNOWN";
    ESP_LOGW(TAG, "Reset reason: %s (%d)", rst_str, (int)rst);
    if (rst == ESP_RST_PANIC || rst == ESP_RST_INT_WDT || rst == ESP_RST_TASK_WDT || rst == ESP_RST_WDT) {
        ESP_LOGE(TAG, "*** PREVIOUS BOOT CRASHED! Check core dump with: espcoredump.py info_corefile ***");
    }

    ESP_LOGI(TAG, "Free heap: %lu, min heap: %lu, PSRAM free: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Set timezone to UAE (Gulf Standard Time, UTC+4)
    setenv("TZ", "GST-4", 1);
    tzset();

    // Register all services (no init yet — just populate the registry)
    tab5_services_register_all();

    // --- Layer 0: Platform init (I2C bus, IO expanders) ---
    // Services need the I2C bus to exist before their init() runs.

    // Initialize I2C bus
    esp_err_t ret;
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

            // WiFi power ON — C6 co-processor needs power before SDIO init
            tab5_set_wifi_power(true);
        }
    }

    // --- Layer 1: Init all services (allocate, configure hardware) ---
    tab5_services_init_all();

    // Show blue screen while booting (LVGL init deferred until after WiFi)
    tab5_display_fill_color(0x001F);

    // Peripheral drivers not yet wrapped as services (camera, IMU, RTC, battery)
    if (s_i2c_bus) {
        ESP_LOGI(TAG, "Initializing camera (SC2336)...");
        ret = tab5_camera_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Camera init failed: %s", esp_err_to_name(ret));
        } else {
            s_cam_ok = true;
        }

        ESP_LOGI(TAG, "Initializing IMU (BMI270)...");
        ret = tab5_imu_init(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "IMU init failed: %s", esp_err_to_name(ret));
        } else {
            s_imu_ok = true;
        }

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

    // --- Layer 2: Start core services ---
    tab5_services_start(SERVICE_STORAGE);
    tab5_services_start(SERVICE_DISPLAY);
    tab5_services_start(SERVICE_AUDIO);
    tab5_services_start(SERVICE_NETWORK);

    // Derive convenience flags from service state
    static bool s_wifi_ok = false;
    s_wifi_ok = tab5_services_is_running(SERVICE_NETWORK);
    s_touch_ok = (tab5_services_get_state(SERVICE_DISPLAY) != SERVICE_STATE_ERROR);
    s_sd_ok = (tab5_services_get_state(SERVICE_STORAGE) != SERVICE_STATE_ERROR);
    s_audio_ok = (tab5_services_get_state(SERVICE_AUDIO) != SERVICE_STATE_ERROR);
    s_mic_ok = s_audio_ok; /* mic init is part of audio service */

    // --- Layer 3: Start Dragon (needs WiFi) ---
    if (s_wifi_ok) {
        tab5_services_start(SERVICE_DRAGON);
    }

    // Initialize LVGL UI layer (deferred until after WiFi to avoid PSRAM contention)
    ESP_LOGI(TAG, "Initializing LVGL UI...");
    ret = tab5_ui_init(tab5_display_get_panel());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL UI init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LVGL initialized — showing Glyph splash");

        // Show Glyph OS splash screen
        tab5_ui_lock();
        ui_splash_create();
        ui_splash_set_status("Hardware OK");
        ui_splash_set_progress(30);
        tab5_ui_unlock();
        vTaskDelay(pdMS_TO_TICKS(400));

        tab5_ui_lock();
        ui_splash_set_status(s_wifi_ok ? "WiFi connected" : "WiFi offline");
        ui_splash_set_progress(60);
        tab5_ui_unlock();
        vTaskDelay(pdMS_TO_TICKS(400));

        tab5_ui_lock();
        ui_splash_set_status("Dragon link started");
        ui_splash_set_progress(85);
        tab5_ui_unlock();
        vTaskDelay(pdMS_TO_TICKS(400));

        tab5_ui_lock();
        ui_splash_set_status("Glyph OS ready");
        ui_splash_set_progress(100);
        tab5_ui_unlock();
        vTaskDelay(pdMS_TO_TICKS(600));

        // Transition to Glyph home screen
        tab5_ui_lock();
        ui_splash_destroy();
        ui_home_create();
        tab5_ui_unlock();
        ESP_LOGI(TAG, "Glyph home screen loaded");

        // Defer overlay creation — runs on the LVGL timer task stack
        // (main task stack is too small for all the LVGL object creation)
        tab5_ui_lock();
        lv_timer_create(deferred_overlay_init_cb, 100, NULL);
        tab5_ui_unlock();
        ESP_LOGI(TAG, "Overlay init deferred to LVGL timer");
    }

    // Start debug HTTP server (needs WiFi + display)
    if (s_wifi_ok) {
        ret = tab5_debug_server_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Debug server init failed: %s", esp_err_to_name(ret));
        }
    }

    // Print service status table
    tab5_services_print_status();

    ESP_LOGI(TAG, "TinkerTab v1.0.0 running — WiFi=%s Touch=%s SD=%s Cam=%s Audio=%s Mic=%s IMU=%s RTC=%s Bat=%s",
             s_wifi_ok ? "Y" : "N", s_touch_ok ? "Y" : "N",
             s_sd_ok ? "Y" : "N", s_cam_ok ? "Y" : "N",
             s_audio_ok ? "Y" : "N", s_mic_ok ? "Y" : "N",
             s_imu_ok ? "Y" : "N", s_rtc_ok ? "Y" : "N", s_bat_ok ? "Y" : "N");
    printf("\nTinkerTab ready. Commands: info, heap, wifi, dragon, stream, scan,\n"
           "  red/green/blue/white/black, bright <0-100>, pattern [0-3],\n"
           "  touch, touchdiag, sd, cam, audio, mic, imu, rtc, bat, services, reboot\n\n");

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
                        printf("WiFi: %s\n", s_wifi_ok ? "connected" : "not connected");
                        printf("Touch: %s\n", s_touch_ok ? "active" : "inactive");
                        printf("SD: %s\n", s_sd_ok ? "mounted" : "not mounted");
                        printf("Camera: %s\n", s_cam_ok ? "ready" : "not init");
                        printf("Audio: %s\n", s_audio_ok ? "ready" : "not init");
                        printf("Mic: %s\n", s_mic_ok ? "ready" : "not init");
                        printf("IMU: %s\n", s_imu_ok ? "ready" : "not init");
                        printf("RTC: %s\n", s_rtc_ok ? "ready" : "not init");
                        printf("Battery: %s\n", s_bat_ok ? "ready" : "not init");
                        printf("BLE: stub (waiting for ESP-Hosted support)\n");
                        printf("Mode: %s\n", tab5_mode_str());
                    } else if (strcmp(cmd_buf, "mode") == 0) {
                        printf("Mode: %s\n", tab5_mode_str());
                    } else if (strcmp(cmd_buf, "heap") == 0) {
                        printf("Heap: %lu / PSRAM: %lu\n",
                               (unsigned long)esp_get_free_heap_size(),
                               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                    } else if (strcmp(cmd_buf, "wifi") == 0) {
                        {
                            char ssid_tmp[33];
                            tab5_settings_get_wifi_ssid(ssid_tmp, sizeof(ssid_tmp));
                            printf("WiFi: %s (SSID: %s)\n", s_wifi_ok ? "connected" : "not connected", ssid_tmp);
                        }
                    } else if (strcmp(cmd_buf, "dragon") == 0) {
                        {
                            char dhost[64];
                            tab5_settings_get_dragon_host(dhost, sizeof(dhost));
                            printf("Dragon: %s (target: %s:%d)\n",
                                   tab5_dragon_state_str(), dhost, tab5_settings_get_dragon_port());
                        }
                        if (tab5_dragon_is_streaming()) {
                            printf("  MJPEG: %.1f FPS\n", tab5_dragon_get_fps());
                            printf("  Touch WS: %s\n", tab5_touch_ws_connected() ? "connected" : "disconnected");
                        }
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
                        if (s_rtc_ok) {  // NTP needs WiFi — disabled for now
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
                    } else if (strcmp(cmd_buf, "tasks") == 0) {
                        ESP_LOGI(TAG, "Free heap: %lu, min: %lu",
                                 (unsigned long)esp_get_free_heap_size(),
                                 (unsigned long)esp_get_minimum_free_heap_size());
                        ESP_LOGI(TAG, "PSRAM free: %lu",
                                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                        ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());
                        printf("Task count: %lu\n", (unsigned long)uxTaskGetNumberOfTasks());
                    } else if (strcmp(cmd_buf, "services") == 0) {
                        tab5_services_print_status();
                    } else if (strcmp(cmd_buf, "voice") == 0) {
                        if (!s_mic_ok || !s_audio_ok) {
                            printf("Voice: audio/mic not initialized\n");
                        } else {
                            printf("Spawning voice test task...\n");
                            xTaskCreatePinnedToCore(voice_test_task, "voice_test",
                                                    16384, NULL, 5, NULL, 1);
                        }
                    } else if (strcmp(cmd_buf, "reboot") == 0) {
                        printf("Rebooting...\n");
                        vTaskDelay(pdMS_TO_TICKS(100));
                        esp_restart();
                    } else {
                        printf("Unknown: %s\n", cmd_buf);
                        printf("Commands: info, heap, wifi, stream, scan,\n"
                               "  red/green/blue/white/black, bright <0-100>, pattern [0-3],\n"
                               "  touch, touchdiag, sd, cam, audio, mic, voice, imu, rtc, ntp, bat, reboot\n");
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
