if(NOT IDF_TARGET STREQUAL "linux")
    idf_component_register(
        SRCS "display.c" "esp_lcd_st7123.c" "touch.c"
             "audio.c" "mic.c" "camera.c"
             "io_expander.c" "battery.c" "imu.c" "tab5_rtc.c" "bluetooth.c"
        INCLUDE_DIRS "."
        REQUIRES esp_lcd esp_psram driver esp_driver_i2c esp_driver_i2s
                 esp_driver_jpeg esp_driver_cam esp_driver_isp
                 esp_video esp_cam_sensor esp_sccb_intf
                 esp_driver_ledc esp_driver_gpio heap esp_mm
                 esp_system esp_timer log freertos
                 espressif__esp_codec_dev
                 espressif__esp_lcd_touch espressif__esp_lcd_touch_st7123
    )
else()
    # Linux simulator: no hardware drivers, just expose headers
    idf_component_register(
        INCLUDE_DIRS "."
    )
endif()
