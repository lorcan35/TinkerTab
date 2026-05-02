/*
 * debug_server_codec.h — codec stack debug endpoint family.
 *
 * Wave 23b follow-up (#332): third per-family extract from
 * debug_server.c, after K144 (#336) and OTA (#337).
 *
 * Endpoints:
 *   POST /codec/opus_test  — synthetic OPUS encoder smoke test
 *
 * The opus_test endpoint was added in TT #264 (Wave 19) to bisect the
 * SILK NSQ stack-overflow crash; the bisect found 24 KB as the
 * watermark, which led to the MIC_TASK_STACK_SIZE bump in voice.c.
 * The endpoint stays as a permanent diagnostic — useful for any
 * future codec-stack health check or stack-budget validation.
 */

#pragma once

#include "esp_http_server.h"

/* Register the codec endpoint family on `server`.  Called from
 * tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_codec_register(httpd_handle_t server);
