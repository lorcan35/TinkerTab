#pragma once

#include <stdbool.h>

typedef void (*mjpeg_disconnect_cb_t)(void);

void tab5_mjpeg_start(void);
void tab5_mjpeg_stop(void);
bool tab5_mjpeg_is_running(void);
float tab5_mjpeg_get_fps(void);
void tab5_mjpeg_set_disconnect_cb(mjpeg_disconnect_cb_t cb);
