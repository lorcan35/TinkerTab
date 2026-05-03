/*
 * debug_server_camera.h — display + camera capture debug HTTP family.
 *
 * Wave 23b follow-up (#332): eleventh per-family extract.  Owns
 * /screenshot, /screenshot.jpg and /camera plus the shared hardware
 * JPEG encoder they both use.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Eagerly create the hardware JPEG encoder once at server startup.
 * Lazy per-request init races when multiple sockets arrive before
 * the encoder is populated — both tasks try to init, the second one
 * crashes in jpeg_release_codec_handle(NULL).  Called from
 * tab5_debug_server_start in debug_server.c. */
esp_err_t debug_server_camera_init_jpeg(void);

/* Register /screenshot, /screenshot.jpg, /camera against the live
 * HTTPD server.  Called from debug_server.c init after the server
 * is started. */
void debug_server_camera_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
