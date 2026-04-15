/**
 * TinkerTab — Camera Driver (SC2336 2MP MIPI-CSI)
 *
 * SC2336 sensor connected via MIPI-CSI (2 data lanes).
 * XCLK provided by LEDC on GPIO 36 @ 24MHz.
 * Camera reset via IO expander PI4IOE5V6416 (0x43) pin P6.
 * SCCB (I2C) address: 0x30.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/** Camera frame format */
typedef enum {
    TAB5_CAM_FMT_JPEG,
    TAB5_CAM_FMT_RGB565,
    TAB5_CAM_FMT_YUV422,
} tab5_cam_format_t;

/** Camera resolution */
typedef enum {
    TAB5_CAM_RES_QVGA,    // 320x240
    TAB5_CAM_RES_VGA,     // 640x480
    TAB5_CAM_RES_HD,      // 1280x720
    TAB5_CAM_RES_FULL,    // 1920x1080 (2MP)
} tab5_cam_resolution_t;

/** Captured frame */
typedef struct {
    uint8_t *data;         // Frame data (caller must not free — managed internally)
    uint32_t size;         // Frame size in bytes
    uint16_t width;
    uint16_t height;
    tab5_cam_format_t format;
} tab5_cam_frame_t;

/**
 * Initialize camera (SC2336 + MIPI-CSI).
 * Requires IO expander to be initialized first (for camera reset).
 * Starts XCLK on GPIO 36 @ 24MHz.
 */
esp_err_t tab5_camera_init(void);

/** Deinitialize camera and free resources. */
esp_err_t tab5_camera_deinit(void);

/** Check if camera is initialized. */
bool tab5_camera_initialized(void);

/** Set resolution. Must be called before capture (or re-init). */
esp_err_t tab5_camera_set_resolution(tab5_cam_resolution_t res);

/** Capture a single frame. Frame data valid until next capture or deinit. */
esp_err_t tab5_camera_capture(tab5_cam_frame_t *frame);

/** Save captured frame to file (JPEG). Requires SD card mounted. */
esp_err_t tab5_camera_save_jpeg(const tab5_cam_frame_t *frame, const char *path);

/** Get camera sensor info string. */
const char* tab5_camera_info(void);
