set(UI_SRCS
    "ui_theme.c"
    "ui_splash.c" "ui_home.c" "ui_settings.c"
    "ui_camera.c" "ui_files.c" "ui_audio.c" "ui_keyboard.c" "ui_voice.c" "ui_wifi.c"
    "ui_video_pane.c"
    "ui_chat.c" "ui_notes.c" "ui_feedback.c" "ui_agents.c" "ui_memory.c" "ui_focus.c" "ui_sessions.c"
    "ui_mode_sheet.c"
    "ui_nav_sheet.c"
    "ui_onboarding.c"
    "chat_msg_store.c"
    "chat_header.c"
    "chat_input_bar.c"
    "chat_suggestions.c"
    "chat_msg_view.c"
    "chat_session_drawer.c"
    "md_strip.c"
    "fonts/fraunces_italic_32.c"
    "fonts/fraunces_italic_22.c"
    "fonts/jetbrains_mono_medium_14.c"
)

if(IDF_TARGET STREQUAL "linux")
    idf_component_register(
        SRCS "main.c" ${UI_SRCS}
        INCLUDE_DIRS "."
        REQUIRES tab5
            nvs_flash esp_system esp_timer log freertos heap
            json esp_event esp_netif lvgl
            hal_linux
    )
else()
    idf_component_register(
        SRCS "main.c"
             "sdcard.c" "wifi.c"
             "voice.c" "voice_codec.c" "voice_video.c" "mode_manager.c"
             "debug_server.c" "debug_obs.c" "pool_probe.c" "heap_watchdog.c"
             "service_registry.c" "service_storage.c" "service_display.c"
             "service_audio.c" "service_network.c" "service_dragon.c"
             "ui_core.c" ${UI_SRCS}
             "settings.c" "ota.c" "media_cache.c"
             "widget_store.c"
             "spring_anim.c"
             "task_worker.c"
             "tool_log.c"
             "m5_stackflow.c"
        EMBED_TXTFILES "debug_ui.html"
        INCLUDE_DIRS "."
        REQUIRES tab5
                 esp_lcd esp_psram driver nvs_flash esp_system heap esp_mm esp_timer
                 json esp_driver_jpeg fatfs sdmmc esp_driver_i2s esp_driver_cam lvgl
                 esp_wifi esp_netif esp_event esp_http_client esp_http_server tcp_transport
                 espcoredump app_update esp_https_ota
                 esp_driver_isp esp_video esp_cam_sensor
                 espressif__esp_audio_codec
    )
endif()
