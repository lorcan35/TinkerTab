/**
 * TinkerTab — Camera Driver (SC202CS / SC2356 2MP MIPI-CSI)
 *
 * Hardware:
 *   - Sensor: SmartSens SC202CS (SC2356) — 2MP, 1600x1200
 *     (M5Stack Tab5 docs say "SC2336" but the actual chip is SC2356/SC202CS)
 *   - Interface: MIPI-CSI 2 data lanes (hardware-fixed pins on ESP32-P4)
 *   - XCLK: 24MHz from fixed crystal oscillator (X2), NOT GPIO36
 *     (GPIO36 LEDC is kept for compatibility but the crystal provides the clock)
 *   - Control: SCCB (I2C-like) at address 0x36
 *   - Reset: IO expander PI4IOE5V6416 (0x43) pin P6 (active low)
 *   - PID: 0xEB52
 *
 * Uses ESP-IDF esp_driver_cam CSI controller API.
 *
 * Reference: espressif/esp-video-components — esp_cam_sensor/sensors/sc202cs/
 */

#include "camera.h"
#include "config.h"
#include "io_expander.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"

/* ESP-IDF camera controller (CSI) */
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"

static const char *TAG = "tab5_cam";

/* SC202CS (SC2356) SCCB (I2C) address — 0x36 on Tab5 (SID pin high) */
#define SC202CS_SCCB_ADDR       0x36

/* SC202CS chip ID registers (same register layout as SC2336 family) */
#define SC202CS_REG_CHIP_ID_H   0x3107
#define SC202CS_REG_CHIP_ID_L   0x3108
#define SC202CS_CHIP_ID         0xEB52

/* Camera XCLK */
#define CAM_XCLK_GPIO           36
#define CAM_XCLK_FREQ_HZ       24000000

/* MIPI-CSI config — SC202CS is a 1-lane sensor */
#define CAM_CSI_LANE_NUM        1
#define CAM_CSI_LANE_BITRATE    576  /* Mbps — from M5Stack esp_cam_sensor (576MHz for 720p RAW8) */

/* Frame buffer (allocated in PSRAM) */
#define CAM_FB_MAX_SIZE         (1920 * 1080 * 2)  /* Max: 1080p RGB565 */

/* State */
static bool s_initialized = false;
static esp_cam_ctlr_handle_t s_cam_ctrl = NULL;
static i2c_master_dev_handle_t s_sccb_dev = NULL;
static uint8_t *s_frame_buf = NULL;
static uint32_t s_frame_size = 0;
static uint16_t s_width = 1280;
static uint16_t s_height = 720;
static volatile bool s_capture_busy = false;  /* prevents overlapping captures */

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

static void cam_xclk_deinit(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

/* --- SCCB (I2C) for sensor register access --- */

static esp_err_t cam_sccb_init(i2c_master_bus_handle_t i2c_bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SC202CS_SCCB_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_sccb_dev);
}

static esp_err_t cam_sccb_write_reg(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_sccb_dev, buf, 3, 100);
}

static esp_err_t cam_sccb_read_reg(uint16_t reg, uint8_t *val)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(s_sccb_dev, reg_buf, 2, val, 1, 100),
        TAG, "SCCB read failed"
    );
    return ESP_OK;
}

static esp_err_t cam_check_sensor_id(void)
{
    uint8_t id_h = 0, id_l = 0;
    ESP_RETURN_ON_ERROR(cam_sccb_read_reg(SC202CS_REG_CHIP_ID_H, &id_h), TAG, "Read chip ID H failed");
    ESP_RETURN_ON_ERROR(cam_sccb_read_reg(SC202CS_REG_CHIP_ID_L, &id_l), TAG, "Read chip ID L failed");

    uint16_t chip_id = ((uint16_t)id_h << 8) | id_l;
    ESP_LOGI(TAG, "SC202CS chip ID: 0x%04X (expected 0x%04X)", chip_id, SC202CS_CHIP_ID);

    if (chip_id != SC202CS_CHIP_ID) {
        ESP_LOGW(TAG, "Unexpected chip ID — may be a different sensor revision");
    }
    return ESP_OK;
}

/* --- SC202CS sensor init (basic VGA mode) --- */

/**
 * Minimal SC2336 register init for 640x480 output.
 *
 * NOTE: For full resolution and all modes, the complete register table
 * from the M5Tab5-UserDemo sc2336_settings.h should be used.
 * This basic init gets the sensor outputting frames for testing.
 *
 * TODO: Import full sc2336_settings.h register tables for HD/Full modes.
 */
static esp_err_t cam_sensor_init_vga(void)
{
    /*
     * SC202CS full register init: 1280x720 @ 30fps, MIPI 1-lane, RAW8.
     * Register table from M5Stack esp_cam_sensor component (verified working).
     * 129 registers — proper PLL, analog, timing, MIPI, window config.
     */
    ESP_LOGI(TAG, "Initializing SC202CS (1280x720 RAW8 1-lane, 129 registers)...");

    /* Software reset */
    cam_sccb_write_reg(0x0103, 0x01);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Stream off during config */
    cam_sccb_write_reg(0x0100, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* PLL config */
    cam_sccb_write_reg(0x36e9, 0x80);
    cam_sccb_write_reg(0x36ea, 0x06);
    cam_sccb_write_reg(0x36eb, 0x0a);
    cam_sccb_write_reg(0x36ec, 0x01);
    cam_sccb_write_reg(0x36ed, 0x18);
    cam_sccb_write_reg(0x36e9, 0x24);

    /* System + MIPI config */
    cam_sccb_write_reg(0x301f, 0x18);  /* 1-lane MIPI */
    cam_sccb_write_reg(0x3031, 0x08);  /* RAW8 output */
    cam_sccb_write_reg(0x3037, 0x00);

    /* Window: 1280x720 */
    cam_sccb_write_reg(0x3200, 0x00); cam_sccb_write_reg(0x3201, 0xa0);
    cam_sccb_write_reg(0x3202, 0x00); cam_sccb_write_reg(0x3203, 0xf0);
    cam_sccb_write_reg(0x3204, 0x05); cam_sccb_write_reg(0x3205, 0xa7);
    cam_sccb_write_reg(0x3206, 0x03); cam_sccb_write_reg(0x3207, 0xc7);
    cam_sccb_write_reg(0x3208, 0x05); cam_sccb_write_reg(0x3209, 0x00);  /* H=1280 */
    cam_sccb_write_reg(0x320a, 0x02); cam_sccb_write_reg(0x320b, 0xd0);  /* V=720  */
    cam_sccb_write_reg(0x3210, 0x00); cam_sccb_write_reg(0x3211, 0x04);
    cam_sccb_write_reg(0x3212, 0x00); cam_sccb_write_reg(0x3213, 0x04);

    /* Analog + timing (full Espressif-verified table) */
    cam_sccb_write_reg(0x3301, 0xff); cam_sccb_write_reg(0x3304, 0x68);
    cam_sccb_write_reg(0x3306, 0x40); cam_sccb_write_reg(0x3308, 0x08);
    cam_sccb_write_reg(0x3309, 0xa8); cam_sccb_write_reg(0x330b, 0xd0);
    cam_sccb_write_reg(0x330c, 0x18); cam_sccb_write_reg(0x330d, 0xff);
    cam_sccb_write_reg(0x330e, 0x20); cam_sccb_write_reg(0x331e, 0x59);
    cam_sccb_write_reg(0x331f, 0x99); cam_sccb_write_reg(0x3333, 0x10);
    cam_sccb_write_reg(0x335e, 0x06); cam_sccb_write_reg(0x335f, 0x08);
    cam_sccb_write_reg(0x3364, 0x1f); cam_sccb_write_reg(0x337c, 0x02);
    cam_sccb_write_reg(0x337d, 0x0a); cam_sccb_write_reg(0x338f, 0xa0);
    cam_sccb_write_reg(0x3390, 0x01); cam_sccb_write_reg(0x3391, 0x03);
    cam_sccb_write_reg(0x3392, 0x1f); cam_sccb_write_reg(0x3393, 0xff);
    cam_sccb_write_reg(0x3394, 0xff); cam_sccb_write_reg(0x3395, 0xff);
    cam_sccb_write_reg(0x33a2, 0x04); cam_sccb_write_reg(0x33ad, 0x0c);
    cam_sccb_write_reg(0x33b1, 0x20); cam_sccb_write_reg(0x33b3, 0x38);
    cam_sccb_write_reg(0x33f9, 0x40); cam_sccb_write_reg(0x33fb, 0x48);
    cam_sccb_write_reg(0x33fc, 0x0f); cam_sccb_write_reg(0x33fd, 0x1f);
    cam_sccb_write_reg(0x349f, 0x03); cam_sccb_write_reg(0x34a6, 0x03);
    cam_sccb_write_reg(0x34a7, 0x1f); cam_sccb_write_reg(0x34a8, 0x38);
    cam_sccb_write_reg(0x34a9, 0x30); cam_sccb_write_reg(0x34ab, 0xd0);
    cam_sccb_write_reg(0x34ad, 0xd8); cam_sccb_write_reg(0x34f8, 0x1f);
    cam_sccb_write_reg(0x34f9, 0x20);

    /* AFE + driver */
    cam_sccb_write_reg(0x3630, 0xa0); cam_sccb_write_reg(0x3631, 0x92);
    cam_sccb_write_reg(0x3632, 0x64); cam_sccb_write_reg(0x3633, 0x43);
    cam_sccb_write_reg(0x3637, 0x49); cam_sccb_write_reg(0x363a, 0x85);
    cam_sccb_write_reg(0x363c, 0x0f); cam_sccb_write_reg(0x3650, 0x31);
    cam_sccb_write_reg(0x3670, 0x0d); cam_sccb_write_reg(0x3674, 0xc0);
    cam_sccb_write_reg(0x3675, 0xa0); cam_sccb_write_reg(0x3676, 0xa0);
    cam_sccb_write_reg(0x3677, 0x92); cam_sccb_write_reg(0x3678, 0x96);
    cam_sccb_write_reg(0x3679, 0x9a); cam_sccb_write_reg(0x367c, 0x03);
    cam_sccb_write_reg(0x367d, 0x0f); cam_sccb_write_reg(0x367e, 0x01);
    cam_sccb_write_reg(0x367f, 0x0f); cam_sccb_write_reg(0x3698, 0x83);
    cam_sccb_write_reg(0x3699, 0x86); cam_sccb_write_reg(0x369a, 0x8c);
    cam_sccb_write_reg(0x369b, 0x94); cam_sccb_write_reg(0x36a2, 0x01);
    cam_sccb_write_reg(0x36a3, 0x03); cam_sccb_write_reg(0x36a4, 0x07);
    cam_sccb_write_reg(0x36ae, 0x0f); cam_sccb_write_reg(0x36af, 0x1f);
    cam_sccb_write_reg(0x36bd, 0x22); cam_sccb_write_reg(0x36be, 0x22);
    cam_sccb_write_reg(0x36bf, 0x22); cam_sccb_write_reg(0x36d0, 0x01);
    cam_sccb_write_reg(0x370f, 0x02); cam_sccb_write_reg(0x3721, 0x6c);
    cam_sccb_write_reg(0x3722, 0x8d); cam_sccb_write_reg(0x3725, 0xc5);
    cam_sccb_write_reg(0x3727, 0x14); cam_sccb_write_reg(0x3728, 0x04);
    cam_sccb_write_reg(0x37b7, 0x04); cam_sccb_write_reg(0x37b8, 0x04);
    cam_sccb_write_reg(0x37b9, 0x06); cam_sccb_write_reg(0x37bd, 0x07);
    cam_sccb_write_reg(0x37be, 0x0f);

    /* AEC/AGC */
    cam_sccb_write_reg(0x3901, 0x02); cam_sccb_write_reg(0x3903, 0x40);
    cam_sccb_write_reg(0x3905, 0x8d); cam_sccb_write_reg(0x3907, 0x00);
    cam_sccb_write_reg(0x3908, 0x41); cam_sccb_write_reg(0x391f, 0x41);
    cam_sccb_write_reg(0x3933, 0x80); cam_sccb_write_reg(0x3934, 0x02);
    cam_sccb_write_reg(0x3937, 0x6f); cam_sccb_write_reg(0x393a, 0x01);
    cam_sccb_write_reg(0x393d, 0x01); cam_sccb_write_reg(0x393e, 0xc0);
    cam_sccb_write_reg(0x39dd, 0x41);

    /* Exposure defaults */
    cam_sccb_write_reg(0x3e00, 0x00); cam_sccb_write_reg(0x3e01, 0x4d);
    cam_sccb_write_reg(0x3e02, 0xc0); cam_sccb_write_reg(0x3e09, 0x00);
    cam_sccb_write_reg(0x4509, 0x28); cam_sccb_write_reg(0x450d, 0x61);

    /* Stream on */
    cam_sccb_write_reg(0x0100, 0x01);
    vTaskDelay(pdMS_TO_TICKS(50));  /* allow first frame to stabilize */

    ESP_LOGI(TAG, "SC202CS initialized (1280x720 RAW8, 1-lane MIPI, 129 registers)");
    return ESP_OK;
}

/* --- CSI Controller --- */

static bool cam_csi_frame_done_cb(esp_cam_ctlr_handle_t handle,
                                   esp_cam_ctlr_trans_t *trans,
                                   void *user_data)
{
    s_frame_size = trans->received_size;
    return false;  /* No high-priority task woken */
}

static esp_err_t cam_csi_init(void)
{
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .byte_swap_en = false,
        .queue_items = 2,  /* double-buffer: prevents "queue full" when preview timer overlaps */
        .h_res = s_width,
        .v_res = s_height,
        .data_lane_num = CAM_CSI_LANE_NUM,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,  /* SC202CS RAW8 mode (0x3031=0x08) */
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .lane_bit_rate_mbps = CAM_CSI_LANE_BITRATE,
    };

    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_cfg, &s_cam_ctrl), TAG, "CSI controller init failed");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_trans_finished = cam_csi_frame_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_cam_ctrl, &cbs, NULL), TAG, "Register CBS failed");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(s_cam_ctrl), TAG, "CSI enable failed");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(s_cam_ctrl), TAG, "CSI start failed");

    ESP_LOGI(TAG, "CSI controller initialized + started (%dx%d, %d lanes @ %d Mbps)",
             s_width, s_height, CAM_CSI_LANE_NUM, CAM_CSI_LANE_BITRATE);
    return ESP_OK;
}

/* --- Public API --- */

esp_err_t tab5_camera_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing camera (SC202CS MIPI-CSI)...");

    /* Allocate frame buffer in PSRAM */
    s_frame_buf = heap_caps_calloc(1, CAM_FB_MAX_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer (%d bytes in PSRAM)", CAM_FB_MAX_SIZE);
        return ESP_ERR_NO_MEM;
    }

    /* Power-on sequence matching BSP: XCLK → reset → SCCB
     * The Tab5 has a fixed 24MHz crystal (X2) for camera MCLK, but we also
     * drive GPIO36 via LEDC for compatibility. */

    /* 1. Start XCLK (24MHz on GPIO 36) — must be before reset release */
    ESP_RETURN_ON_ERROR(cam_xclk_init(), TAG, "XCLK init failed");
    vTaskDelay(pdMS_TO_TICKS(5));

    /* 2. Camera reset via IO expander (P6 on 0x43, active low) */
    tab5_set_camera_reset(true);   /* Assert reset (pin LOW) */
    vTaskDelay(pdMS_TO_TICKS(10));
    tab5_set_camera_reset(false);  /* Release reset (pin HIGH) */
    vTaskDelay(pdMS_TO_TICKS(20)); /* Sensor needs time to boot after reset */

    /* 3. Initialize SCCB (I2C) at 0x36 (SC202CS address) */
    extern i2c_master_bus_handle_t tab5_get_i2c_bus(void);
    i2c_master_bus_handle_t i2c_bus = tab5_get_i2c_bus();
    if (!i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not available");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(cam_sccb_init(i2c_bus), TAG, "SCCB init failed");

    /* Check sensor ID */
    ESP_RETURN_ON_ERROR(cam_check_sensor_id(), TAG, "Sensor ID check failed");

    /* Initialize sensor registers (VGA mode for now) */
    ESP_RETURN_ON_ERROR(cam_sensor_init_vga(), TAG, "Sensor init failed");

    /* Initialize CSI controller */
    ESP_RETURN_ON_ERROR(cam_csi_init(), TAG, "CSI init failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Camera initialized! SC202CS @ %dx%d", s_width, s_height);
    return ESP_OK;
}

esp_err_t tab5_camera_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    if (s_cam_ctrl) {
        esp_cam_ctlr_disable(s_cam_ctrl);
        esp_cam_ctlr_del(s_cam_ctrl);
        s_cam_ctrl = NULL;
    }

    cam_xclk_deinit();

    if (s_frame_buf) {
        free(s_frame_buf);
        s_frame_buf = NULL;
    }

    /* Assert camera reset to save power */
    tab5_set_camera_reset(false);

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
    switch (res) {
        case TAB5_CAM_RES_QVGA:  s_width = 320;  s_height = 240;  break;
        case TAB5_CAM_RES_VGA:   s_width = 640;  s_height = 480;  break;
        case TAB5_CAM_RES_HD:    s_width = 1280; s_height = 720;  break;
        case TAB5_CAM_RES_FULL:  s_width = 1920; s_height = 1080; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Resolution set to %dx%d (will apply on next init)", s_width, s_height);
    /* TODO: Re-init sensor and CSI with new resolution */
    return ESP_OK;
}

esp_err_t tab5_camera_capture(tab5_cam_frame_t *frame)
{
    if (!s_initialized || !s_cam_ctrl || !s_frame_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Prevent overlapping captures — preview timer can fire while
     * previous frame is still being received via CSI DMA */
    if (s_capture_busy) {
        return ESP_ERR_NOT_FINISHED;
    }
    s_capture_busy = true;

    esp_cam_ctlr_trans_t trans = {
        .buffer = s_frame_buf,
        .buflen = CAM_FB_MAX_SIZE,
    };

    s_frame_size = 0;
    esp_err_t err = esp_cam_ctlr_receive(s_cam_ctrl, &trans, 2000);
    if (err != ESP_OK) {
        s_capture_busy = false;
        ESP_LOGW(TAG, "CSI receive failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait for frame done callback */
    int retries = 200;  /* up to 2s */
    while (s_frame_size == 0 && retries-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_capture_busy = false;

    if (s_frame_size == 0) {
        ESP_LOGE(TAG, "Capture timeout — no frame received");
        return ESP_ERR_TIMEOUT;
    }

    frame->data = s_frame_buf;
    frame->size = s_frame_size;
    frame->width = s_width;
    frame->height = s_height;
    frame->format = TAB5_CAM_FMT_RGB565;

    return ESP_OK;
}

esp_err_t tab5_camera_save_jpeg(const tab5_cam_frame_t *frame, const char *path)
{
    if (!frame || !frame->data || !path) return ESP_ERR_INVALID_ARG;

    /*
     * TODO: Convert RGB565 to JPEG using ESP32-P4 HW JPEG encoder.
     * For now, save raw frame data.
     * The HW JPEG encoder is available via esp_driver_jpeg component.
     */

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }

    size_t written = fwrite(frame->data, 1, frame->size, f);
    fclose(f);

    if (written != frame->size) {
        ESP_LOGE(TAG, "Write incomplete: %d / %lu bytes", (int)written, (unsigned long)frame->size);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Frame saved to %s (%lu bytes)", path, (unsigned long)frame->size);
    return ESP_OK;
}

const char* tab5_camera_info(void)
{
    static char info[128];
    snprintf(info, sizeof(info),
             "SC202CS %dx%d MIPI-CSI %d-lane, XCLK=%dMHz, SCCB=0x%02X",
             s_width, s_height, CAM_CSI_LANE_NUM,
             CAM_XCLK_FREQ_HZ / 1000000, SC202CS_SCCB_ADDR);
    return info;
}
