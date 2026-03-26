/**
 * TinkerTab — WebSocket Touch Forwarder
 *
 * Forwards touch events to Dragon via WebSocket for remote control.
 */
#pragma once

#include "touch.h"
#include <stdbool.h>

typedef void (*touch_ws_disconnect_cb_t)(void);

/** Start the WebSocket touch forwarding task. */
void tab5_touch_ws_start(void);

/** Stop the WebSocket touch forwarding task. */
void tab5_touch_ws_stop(void);

/** Send a touch event to Dragon. Thread-safe. */
void tab5_touch_ws_send(const tab5_touch_point_t *points, uint8_t count);

/** Check if WebSocket is connected. */
bool tab5_touch_ws_connected(void);

/** Set disconnect callback for dragon_link. */
void tab5_touch_ws_set_disconnect_cb(touch_ws_disconnect_cb_t cb);
