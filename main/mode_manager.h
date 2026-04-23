/**
 * TinkerTab Mode FSM — voice-pipeline coordinator
 *
 * Post-#154 this is a thin wrapper: the CDP browser-streaming states
 * (STREAMING/BROWSING) are gone, leaving only IDLE ↔ VOICE. The FSM is
 * kept because ui_voice / ui_notes / service_dragon all call through
 * tab5_mode_switch() from multiple tasks, and the mutex gives us a
 * single-flight guarantee around voice_connect_async().
 */
#pragma once

#include "esp_err.h"

typedef enum {
    MODE_IDLE,       // WiFi only, voice WS disconnected
    MODE_VOICE,      // Voice WS connected, mic/speaker owned by voice pipeline
} tab5_mode_t;

/** Initialize mode-manager mutex. Call once at boot before any switch. */
void tab5_mode_init(void);

/** Current mode (IDLE / VOICE). Safe from any task. */
tab5_mode_t tab5_mode_get(void);

/** Human-readable mode string for logs + debug REPL. */
const char *tab5_mode_str(void);

/** Transition mode — single-flight via mutex; blocks up to 5s on contention. */
esp_err_t tab5_mode_switch(tab5_mode_t new_mode);
