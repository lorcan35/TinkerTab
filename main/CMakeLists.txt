idf_component_register(
    SRCS "main.c" "display.c" "io_expander.c" "esp_lcd_st7123.c" "touch.c"
         "wifi.c" "mjpeg_stream.c" "touch_ws.c" "sdcard.c" "camera.c"
         "audio.c" "mic.c" "imu.c" "rtc.c" "battery.c" "bluetooth.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_psram driver nvs_flash esp_system heap esp_mm esp_timer
             esp_wifi esp_netif esp_event esp_http_client tcp_transport json
             esp_driver_jpeg fatfs sdmmc esp_driver_i2s
)
