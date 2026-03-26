/**
 * TinkerTab — WebSocket Touch Forwarder
 *
 * Forwards touch events to Dragon via WebSocket for remote control.
 */
#pragma once

#include "touch.h"

/** Start the WebSocket touch forwarding task. */
void tab5_touch_ws_start(void);

/** Send a touch event to Dragon. Thread-safe. */
void tab5_touch_ws_send(const tab5_touch_point_t *points, uint8_t count);

/** Check if WebSocket is connected. */
bool tab5_touch_ws_connected(void);
