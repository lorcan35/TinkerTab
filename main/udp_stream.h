/**
 * TinkerTab -- UDP JPEG Stream Receiver
 *
 * Low-latency video receiver for Dragon display streaming over UDP.
 * Replaces HTTP MJPEG for ~10-20ms latency instead of 100-250ms.
 *
 * Protocol:
 *   Chunked mode: [frame_num:4B BE][chunk_idx:2B BE][total_chunks:2B BE][jpeg_data...]
 *   Simple mode:  [frame_num:4B BE][jpeg_data...]  (entire JPEG in one datagram)
 */
#pragma once

#include <stdbool.h>

typedef void (*udp_stream_disconnect_cb_t)(void);

/** Start the UDP JPEG stream receiver on port 5000. */
void udp_stream_start(void);

/** Stop the UDP stream receiver. */
void udp_stream_stop(void);

/** Check if the UDP stream receiver is actively running. */
bool udp_stream_is_active(void);

/** Get current decode FPS (0 if not streaming). */
float udp_stream_get_fps(void);

/** Set callback for stream timeout / disconnect. */
void udp_stream_set_disconnect_cb(udp_stream_disconnect_cb_t cb);
