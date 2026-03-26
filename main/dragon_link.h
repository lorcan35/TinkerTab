/**
 * TinkerTab — Dragon Link
 *
 * Manages the connection lifecycle between Tab5 and Dragon Q6A.
 * Handles discovery, handshake, MJPEG + touch WS start/stop, and reconnection.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    DRAGON_STATE_IDLE,          // WiFi not ready or link not started
    DRAGON_STATE_DISCOVERING,   // Polling GET /health
    DRAGON_STATE_HANDSHAKE,     // GET /api/handshake
    DRAGON_STATE_CONNECTED,     // Handshake OK, starting streams
    DRAGON_STATE_STREAMING,     // MJPEG + touch WS active
    DRAGON_STATE_RECONNECTING,  // Lost connection, backing off
    DRAGON_STATE_OFFLINE,       // Dragon unreachable after retries
} dragon_state_t;

/** Initialize Dragon link task. Call after WiFi is connected. */
esp_err_t tab5_dragon_link_init(void);

/** Get current connection state. */
dragon_state_t tab5_dragon_get_state(void);

/** Human-readable state string. */
const char *tab5_dragon_state_str(void);

/** Current MJPEG FPS (0 if not streaming). */
float tab5_dragon_get_fps(void);

/** Manually request disconnect. */
void tab5_dragon_request_stop(void);

/** Manually request reconnect after stop. */
void tab5_dragon_request_start(void);

/** Check if Dragon is actively streaming. */
bool tab5_dragon_is_streaming(void);
