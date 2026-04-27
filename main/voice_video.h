/*
 * voice_video.h — live JPEG streaming Tab5 → Dragon (#266 / Phase 3A
 * of two-way video).
 *
 * Captures camera frames via the BSP V4L2 stack, applies the user's
 * NVS cam_rot pref (matches the viewfinder), JPEG-encodes via the
 * ESP32-P4 hardware engine, and ships each frame over the existing
 * voice WS as a binary frame prefixed with a 4-byte type tag.
 *
 * Wire format (one binary WS frame per video frame):
 *   bytes 0..3  : magic tag "VID0" (0x56 0x49 0x44 0x30)
 *   bytes 4..7  : payload length (uint32_t big-endian, sanity check)
 *   bytes 8..   : JPEG bytes
 *
 * Audio frames don't carry the magic tag — Dragon's binary handler
 * checks for "VID0" and routes accordingly so existing PCM uplink is
 * unchanged.
 *
 * Lifecycle:
 *   voice_video_init() once at boot (idempotent).
 *   voice_video_start_streaming(fps) starts a worker task that pumps
 *     frames at ~fps Hz (capped at 10 to keep WS bandwidth sane).
 *   voice_video_stop_streaming() stops the worker.  Idempotent.
 *
 * Threading: the streaming task runs on Core 1, owns the JPEG encoder
 * engine (one per stream), and synchronises camera capture with
 * tab5_camera_capture's internal busy flag.  Send goes through the
 * existing voice_ws_send_binary().
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define VOICE_VIDEO_MAGIC      0x56494430u   /* "VID0" big-endian */
#define VOICE_VIDEO_MAX_FPS    10
#define VOICE_VIDEO_DEFAULT_FPS 5

/* Idempotent.  Must be called once before start_streaming.  Allocates
 * the JPEG encoder engine + mutex. */
esp_err_t voice_video_init(void);

/* Start the worker task at the requested fps (clamped to [1..MAX]).
 * Returns ESP_OK on success, ESP_ERR_INVALID_STATE if already
 * streaming, ESP_ERR_NO_MEM on alloc failure. */
esp_err_t voice_video_start_streaming(int fps);

/* Stop the worker task.  Blocks until the next frame finishes (max
 * ~200 ms typical).  Idempotent. */
esp_err_t voice_video_stop_streaming(void);

/* True when the worker task is alive. */
bool voice_video_is_streaming(void);

/* Stats for /info / debug. */
typedef struct {
    bool     active;
    int      fps;
    uint32_t frames_sent;
    uint32_t frames_dropped;
    uint32_t bytes_sent;
    uint32_t last_jpeg_bytes;
    /* #268 Phase 3B downlink stats */
    uint32_t frames_recv;
    uint32_t frames_recv_dropped;
    uint32_t bytes_recv;
    uint32_t last_recv_jpeg_bytes;
} voice_video_stats_t;

void voice_video_get_stats(voice_video_stats_t *out);

/* #268 Phase 3B: feed one inbound video frame received from Dragon.
 * `wire_bytes` MUST start with the "VID0" magic (caller should peek
 * before invoking).  Copies the JPEG payload into an internal slot
 * and asks the renderer to draw it on the LVGL thread.  Returns ESP_OK
 * on a frame the renderer accepted, or ESP_ERR_NOT_FOUND when no
 * video pane is open. */
esp_err_t voice_video_on_downlink_frame(const uint8_t *wire_bytes, size_t len);

/* #268: peek helper — returns true if `data` starts with the "VID0"
 * magic.  Used by voice.c::handle_binary_message before deciding to
 * route to the audio decode path or here. */
bool voice_video_peek_downlink_magic(const void *data, size_t len);
