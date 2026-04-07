/**
 * TinkerTab — Camera Driver (SC202CS 2MP MIPI-CSI)
 *
 * Uses esp_video V4L2 stack (same as M5Stack Tab5 demo).
 * Pipeline: SC202CS → MIPI CSI (1-lane) → ISP (RAW→RGB) → /dev/video0
 *
 * The esp_cam_sensor component handles sensor detection, register config,
 * and MIPI PHY setup automatically. We just use V4L2 ioctls.
 */

#include "camera.h"
#include "config.h"
#include "io_expander.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"

/* esp_video V4L2 framework */
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

static const char *TAG = "tab5_cam";

/* V4L2 device path */
#define CAM_DEV_PATH        "/dev/video0"
#define CAM_BUFFER_COUNT    2

/* Camera XCLK */
#define CAM_XCLK_GPIO      36
#define CAM_XCLK_FREQ_HZ   24000000

/* State */
static bool     s_initialized = false;
static int      s_video_fd = -1;
static void    *s_frame_bufs[CAM_BUFFER_COUNT] = {NULL};
static uint32_t s_frame_sizes[CAM_BUFFER_COUNT] = {0};
static uint16_t s_width = 0;
static uint16_t s_height = 0;
static volatile bool s_capture_busy = false;

/* --- XCLK (24MHz via LEDC on GPIO 36) --- */

static esp_err_t cam_xclk_init(void)
{
    ledc_timer_config_t timer_conf = {
        .duty_resolution = LEDC_TIMER_1_BIT,
        .freq_hz = CAM_XCLK_FREQ_HZ,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_conf), TAG, "LEDC timer config failed");

    ledc_channel_config_t ch_conf = {
        .gpio_num = CAM_XCLK_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_conf), TAG, "LEDC channel config failed");

    ESP_LOGI(TAG, "XCLK started on GPIO %d @ %d MHz", CAM_XCLK_GPIO, CAM_XCLK_FREQ_HZ / 1000000);
    return ESP_OK;
}

/* --- Public API --- */

extern i2c_master_bus_handle_t tab5_get_i2c_bus(void);

esp_err_t tab5_camera_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing camera (esp_video V4L2 stack)...");

    /* Start XCLK for sensor */
    ESP_RETURN_ON_ERROR(cam_xclk_init(), TAG, "XCLK init failed");

    /* Init esp_video framework — auto-detects SC202CS sensor */
    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = tab5_get_i2c_bus(),
            .freq = 400000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    esp_video_init_config_t vid_cfg = {
        .csi = &csi_cfg,
        .dvp = NULL,
        .jpeg = NULL,
    };

    esp_err_t err = esp_video_init(&vid_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "esp_video initialized — sensor auto-detected");

    /* Open V4L2 device */
    s_video_fd = open(CAM_DEV_PATH, O_RDWR);
    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s", CAM_DEV_PATH);
        return ESP_FAIL;
    }

    /* Query capabilities */
    struct v4l2_capability cap;
    if (ioctl(s_video_fd, VIDIOC_QUERYCAP, &cap) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed");
        close(s_video_fd);
        s_video_fd = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "V4L2 device: %s, driver: %s", cap.card, cap.driver);

    /* Get current format */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_video_fd, VIDIOC_G_FMT, &fmt);
    ESP_LOGI(TAG, "Default format: %dx%d, pixfmt=0x%x",
             fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);

    /* Set RGB565 output format */
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(s_video_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGW(TAG, "VIDIOC_S_FMT RGB565 failed, trying default format");
    }

    /* Re-read actual format */
    ioctl(s_video_fd, VIDIOC_G_FMT, &fmt);
    s_width = fmt.fmt.pix.width;
    s_height = fmt.fmt.pix.height;
    ESP_LOGI(TAG, "Active format: %dx%d, pixfmt=0x%x",
             s_width, s_height, fmt.fmt.pix.pixelformat);

    /* Request MMAP buffers */
    struct v4l2_requestbuffers req = {0};
    req.count = CAM_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        close(s_video_fd);
        s_video_fd = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Allocated %d video buffers", req.count);

    /* MMAP + queue each buffer */
    for (int i = 0; i < (int)req.count && i < CAM_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(s_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed", i);
            continue;
        }

        s_frame_bufs[i] = mmap(NULL, buf.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                s_video_fd, buf.m.offset);
        if (s_frame_bufs[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%d] failed", i);
            s_frame_bufs[i] = NULL;
            continue;
        }
        s_frame_sizes[i] = buf.length;

        /* Queue buffer for capture */
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QBUF[%d] failed", i);
        }

        ESP_LOGI(TAG, "Buffer[%d]: %p, %lu bytes", i, s_frame_bufs[i], (unsigned long)buf.length);
    }

    /* Start streaming */
    int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &stream_type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        close(s_video_fd);
        s_video_fd = -1;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Camera ready! %dx%d @ V4L2 (MMAP double-buffer)", s_width, s_height);
    return ESP_OK;
}

esp_err_t tab5_camera_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_video_fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        if (s_frame_bufs[i] && s_frame_bufs[i] != MAP_FAILED) {
            munmap(s_frame_bufs[i], s_frame_sizes[i]);
            s_frame_bufs[i] = NULL;
        }
    }

    if (s_video_fd >= 0) {
        close(s_video_fd);
        s_video_fd = -1;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Camera deinitialized");
    return ESP_OK;
}

bool tab5_camera_initialized(void)
{
    return s_initialized;
}

esp_err_t tab5_camera_set_resolution(tab5_cam_resolution_t res)
{
    /* Resolution is set by the sensor driver automatically.
     * To change: deinit, modify format, reinit. For now, use default. */
    ESP_LOGW(TAG, "Resolution change not implemented (using sensor default %dx%d)", s_width, s_height);
    return ESP_OK;
}

esp_err_t tab5_camera_capture(tab5_cam_frame_t *frame)
{
    if (!s_initialized || s_video_fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_capture_busy) {
        return ESP_ERR_NOT_FINISHED;
    }
    s_capture_busy = true;

    /* Dequeue a filled buffer from the driver */
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0) {
        s_capture_busy = false;
        ESP_LOGW(TAG, "VIDIOC_DQBUF failed");
        return ESP_FAIL;
    }

    /* Fill frame struct */
    frame->data = (uint8_t *)s_frame_bufs[buf.index];
    frame->size = buf.bytesused;
    frame->width = s_width;
    frame->height = s_height;
    frame->format = TAB5_CAM_FMT_RGB565;

    /* Return buffer to driver for next frame */
    ioctl(s_video_fd, VIDIOC_QBUF, &buf);

    s_capture_busy = false;
    return ESP_OK;
}

esp_err_t tab5_camera_save_jpeg(const tab5_cam_frame_t *frame, const char *path)
{
    if (!frame || !frame->data || !path) return ESP_ERR_INVALID_ARG;

    /* For RGB565 frames, save as raw BMP (JPEG encoding needs esp_jpeg) */
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }

    /* Simple BMP header for RGB565 */
    uint32_t data_size = frame->width * frame->height * 2;
    uint32_t file_size = 14 + 40 + 12 + data_size;  /* BMP header + DIB header + masks + data */
    uint8_t bmp_header[66] = {0};

    /* BMP file header */
    bmp_header[0] = 'B'; bmp_header[1] = 'M';
    memcpy(&bmp_header[2], &file_size, 4);
    uint32_t offset = 66;
    memcpy(&bmp_header[10], &offset, 4);

    /* DIB header (BITMAPINFOHEADER) */
    uint32_t dib_size = 40;
    memcpy(&bmp_header[14], &dib_size, 4);
    int32_t w = frame->width, h = -(int32_t)frame->height;  /* negative = top-down */
    memcpy(&bmp_header[18], &w, 4);
    memcpy(&bmp_header[22], &h, 4);
    uint16_t planes = 1; memcpy(&bmp_header[26], &planes, 2);
    uint16_t bpp = 16; memcpy(&bmp_header[28], &bpp, 2);
    uint32_t compression = 3; /* BI_BITFIELDS */ memcpy(&bmp_header[30], &compression, 4);
    memcpy(&bmp_header[34], &data_size, 4);

    /* RGB565 bitmasks */
    uint32_t r_mask = 0xF800, g_mask = 0x07E0, b_mask = 0x001F;
    memcpy(&bmp_header[54], &r_mask, 4);
    memcpy(&bmp_header[58], &g_mask, 4);
    memcpy(&bmp_header[62], &b_mask, 4);

    fwrite(bmp_header, 1, 66, f);
    fwrite(frame->data, 1, data_size, f);
    fclose(f);

    ESP_LOGI(TAG, "Saved %dx%d BMP to %s (%lu bytes)",
             frame->width, frame->height, path, (unsigned long)file_size);
    return ESP_OK;
}

const char *tab5_camera_info(void)
{
    static char info[128];
    if (s_initialized) {
        snprintf(info, sizeof(info), "SC202CS %dx%d V4L2, XCLK=24MHz",
                 s_width, s_height);
    } else {
        snprintf(info, sizeof(info), "Camera not initialized");
    }
    return info;
}
