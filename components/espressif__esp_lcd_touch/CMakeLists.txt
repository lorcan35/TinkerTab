if(IDF_TARGET STREQUAL "linux")
    idf_component_register()
    return()
endif()

idf_component_register(SRCS "esp_lcd_touch.c" INCLUDE_DIRS "include" REQUIRES "driver" "esp_lcd")
