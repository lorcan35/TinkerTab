idf_component_register(
    SRCS "main.c" "display.c" "io_expander.c" "esp_lcd_st7123.c" "touch.c"
         "sdcard.c" "camera.c" "wifi.c"
         "audio.c" "mic.c" "imu.c" "rtc.c" "battery.c" "bluetooth.c"
         "dragon_link.c" "mjpeg_stream.c" "touch_ws.c" "voice.c"
         "ui_core.c" "ui_splash.c" "ui_home.c" "ui_settings.c"
         "ui_camera.c" "ui_files.c" "ui_audio.c" "ui_keyboard.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_psram driver nvs_flash esp_system heap esp_mm esp_timer
             json esp_driver_jpeg fatfs sdmmc esp_driver_i2s esp_driver_cam lvgl
             esp_wifi esp_netif esp_event esp_http_client tcp_transport
)
