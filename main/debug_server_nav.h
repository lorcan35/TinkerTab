/*
 * debug_server_nav.h — screen navigation debug HTTP family.
 *
 * Wave 23b follow-up (#332): fourteenth per-family extract.  Owns
 * /navigate (LVGL-thread screen swap) + /screen (current screen +
 * overlay state) + the s_nav_target string + tab5_debug_set_nav_target
 * public setter (called from ui_chrome / ui_chat / ui_camera / etc.
 * to keep the harness's last-screen view in sync with the actually-
 * loaded screen on swipe-back / back-button paths).
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers /navigate + /screen against the live HTTPD server.
 * Called from debug_server.c init after the server is started. */
void debug_server_nav_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
