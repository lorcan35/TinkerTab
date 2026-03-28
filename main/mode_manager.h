/**
 * TinkerTab Mode FSM — Resource Manager
 *
 * The device is in ONE mode at a time. Switching modes stops/starts
 * services to prevent DMA RAM exhaustion from concurrent SDIO usage.
 *
 * Mode switches are mutex-protected and idempotent.
 */
#pragma once

#include "esp_err.h"

typedef enum {
    MODE_IDLE,       // WiFi only, no streaming, no voice
    MODE_STREAMING,  // WiFi + MJPEG + touch WS (default when Dragon connected)
    MODE_VOICE,      // WiFi + voice WebSocket + mic + speaker (MJPEG paused)
    MODE_BROWSING,   // Same as STREAMING for now
} tab5_mode_t;

/**
 * Initialize mode manager. Call once at boot.
 */
void tab5_mode_init(void);

/**
 * Get current mode.
 */
tab5_mode_t tab5_mode_get(void);

/**
 * Get current mode as string (for logging/debug).
 */
const char *tab5_mode_str(void);

/**
 * Switch to a new mode. Stops services from the old mode, waits for
 * DMA to settle, then starts services for the new mode.
 *
 * Thread-safe (mutex-protected). Idempotent (switching to current mode is a no-op).
 * Returns ESP_OK on success, ESP_ERR_TIMEOUT if mutex couldn't be acquired.
 */
esp_err_t tab5_mode_switch(tab5_mode_t new_mode);
