set(UI_SRCS
    "ui_theme.c"
    "ui_splash.c" "ui_home.c" "ui_settings.c"
    "ui_camera.c" "ui_files.c" "ui_audio.c" "ui_keyboard.c" "ui_voice.c" "ui_wifi.c"
    "ui_video_pane.c"
    "ui_chat.c" "ui_notes.c" "ui_feedback.c" "ui_agents.c" "ui_memory.c" "ui_focus.c" "ui_sessions.c" "ui_skills.c"
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
             "voice.c" "voice_ws_proto.c" "voice_modes.c" "voice_solo.c" "voice_codec.c" "voice_video.c" "voice_widget_ws.c" "voice_billing.c" "mode_manager.c"
             "debug_server.c" "debug_server_m5.c" "debug_server_ota.c"
             "debug_server_codec.c" "debug_server_voice.c" "debug_server_mode.c"
             "debug_server_settings.c" "debug_server_wifi.c" "debug_server_dictation.c"
             "debug_server_call.c" "debug_server_camera.c" "debug_server_obs.c"
             "debug_server_input.c" "debug_server_nav.c" "debug_server_admin.c"
             "debug_server_chat.c" "debug_server_inject.c" "debug_server_metrics.c"
             "debug_server_solo.c"
             "openrouter_sse.c"
             "debug_obs.c" "pool_probe.c" "heap_watchdog.c"
             "service_registry.c" "service_storage.c" "service_display.c"
             "service_audio.c" "service_network.c" "service_dragon.c"
             "ui_core.c" ${UI_SRCS}
             "settings.c" "ota.c" "media_cache.c"
             "widget_store.c"
             "widget_mode_dot.c"
             "widget_icons.c"
             "ui_chrome.c"
             "spring_anim.c"
             "task_worker.c"
             "tool_log.c"
             "m5_stackflow.c"
             "voice_m5_llm.c"
             "voice_onboard.c"
        EMBED_TXTFILES "debug_ui.html"
        INCLUDE_DIRS "."
        REQUIRES tab5
                 esp_lcd esp_psram driver nvs_flash esp_system heap esp_mm esp_timer
                 json esp_driver_jpeg fatfs sdmmc esp_driver_i2s esp_driver_cam lvgl
                 esp_wifi esp_netif esp_event esp_http_client esp_http_server tcp_transport
                 espcoredump app_update esp_https_ota
                 esp_driver_isp esp_video esp_cam_sensor mbedtls
                 espressif__esp_audio_codec
    )
endif()
