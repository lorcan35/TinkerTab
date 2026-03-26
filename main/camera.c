/**
 * TinkerTab — Camera Driver (SC2336 2MP MIPI-CSI)
 *
 * Hardware:
 *   - Sensor: SmartSens SC2336 (2MP, 1920x1080)
 *   - Interface: MIPI-CSI 2 data lanes (hardware-fixed pins on ESP32-P4)
 *   - XCLK: GPIO 36 via LEDC @ 24MHz
 *   - Control: SCCB (I2C-like) at address 0x30
 *   - Reset: IO expander PI4IOE5V6416 (0x43) pin P6 (active low)
 *   - PID: 0xCB3A
 *
 * Uses ESP-IDF esp_driver_cam CSI controller API.
 *
 * Reference: M5Tab5-UserDemo/platforms/tab5/components/esp_cam_sensor/sensors/sc2336/
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

/* SC2336 SCCB (I2C) address */
#define SC2336_SCCB_ADDR        0x30

/* SC2336 chip ID registers */
#define SC2336_REG_CHIP_ID_H    0x3107
#define SC2336_REG_CHIP_ID_L    0x3108
#define SC2336_CHIP_ID          0xCB3A

/* Camera XCLK */
#define CAM_XCLK_GPIO           36
#define CAM_XCLK_FREQ_HZ       24000000

/* MIPI-CSI config */
#define CAM_CSI_LANE_NUM        2
#define CAM_CSI_LANE_BITRATE    400  /* Mbps per lane */

/* Frame buffer (allocated in PSRAM) */
#define CAM_FB_MAX_SIZE         (1920 * 1080 * 2)  /* Max: 1080p RGB565 */

/* State */
static bool s_initialized = false;
static esp_cam_ctlr_handle_t s_cam_ctrl = NULL;
static i2c_master_dev_handle_t s_sccb_dev = NULL;
static uint8_t *s_frame_buf = NULL;
static uint32_t s_frame_size = 0;
static uint16_t s_width = 640;
static uint16_t s_height = 480;

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
        .device_address = SC2336_SCCB_ADDR,
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
    ESP_RETURN_ON_ERROR(cam_sccb_read_reg(SC2336_REG_CHIP_ID_H, &id_h), TAG, "Read chip ID H failed");
    ESP_RETURN_ON_ERROR(cam_sccb_read_reg(SC2336_REG_CHIP_ID_L, &id_l), TAG, "Read chip ID L failed");

    uint16_t chip_id = ((uint16_t)id_h << 8) | id_l;
    ESP_LOGI(TAG, "SC2336 chip ID: 0x%04X (expected 0x%04X)", chip_id, SC2336_CHIP_ID);

    if (chip_id != SC2336_CHIP_ID) {
        ESP_LOGW(TAG, "Unexpected chip ID — may be a different sensor revision");
    }
    return ESP_OK;
}

/* --- SC2336 sensor init (basic VGA mode) --- */

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
    /* Software reset */
    ESP_RETURN_ON_ERROR(cam_sccb_write_reg(0x0103, 0x01), TAG, "Soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    /*
     * SC2336 register init for 640x480 @ 30fps, MIPI 2-lane RAW10
     *
     * These are typical values — the exact sequence should be validated
     * against the SC2336 datasheet or factory firmware register dump.
     *
     * TODO: Replace with verified register table from sc2336_settings.h
     */

    /* PLL / clock configuration */
    cam_sccb_write_reg(0x0100, 0x00);  /* Stream off during config */

    /* Basic analog settings */
    cam_sccb_write_reg(0x36e9, 0x80);  /* Bypass PLL */
    cam_sccb_write_reg(0x37f9, 0x80);

    /* MIPI output config */
    cam_sccb_write_reg(0x3018, 0x32);  /* 2-lane MIPI */
    cam_sccb_write_reg(0x3019, 0x0c);

    /* Window size — 640x480 */
    cam_sccb_write_reg(0x3208, 0x02);  /* H size high = 640 */
    cam_sccb_write_reg(0x3209, 0x80);
    cam_sccb_write_reg(0x320a, 0x01);  /* V size high = 480 */
    cam_sccb_write_reg(0x320b, 0xe0);

    /* Undo PLL bypass */
    cam_sccb_write_reg(0x36e9, 0x24);
    cam_sccb_write_reg(0x37f9, 0x24);

    /* Stream on */
    cam_sccb_write_reg(0x0100, 0x01);

    ESP_LOGI(TAG, "SC2336 initialized (640x480 basic mode)");
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
        .queue_items = 1,
        .h_res = s_width,
        .v_res = s_height,
        .data_lane_num = CAM_CSI_LANE_NUM,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .lane_bit_rate_mbps = CAM_CSI_LANE_BITRATE,
    };

    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_cfg, &s_cam_ctrl), TAG, "CSI controller init failed");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_trans_finished = cam_csi_frame_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_cam_ctrl, &cbs, NULL), TAG, "Register CBS failed");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(s_cam_ctrl), TAG, "CSI enable failed");

    ESP_LOGI(TAG, "CSI controller initialized (%dx%d, %d lanes @ %d Mbps)",
             s_width, s_height, CAM_CSI_LANE_NUM, CAM_CSI_LANE_BITRATE);
    return ESP_OK;
}

/* --- Public API --- */

esp_err_t tab5_camera_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing camera (SC2336 MIPI-CSI)...");

    /* Allocate frame buffer in PSRAM */
    s_frame_buf = heap_caps_calloc(1, CAM_FB_MAX_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer (%d bytes in PSRAM)", CAM_FB_MAX_SIZE);
        return ESP_ERR_NO_MEM;
    }

    /* Power-on sequence matching BSP: XCLK → reset → SCCB
     * SC2336 needs clock running before it responds on I2C. */

    /* 1. Start XCLK (24MHz on GPIO 36) — must be before reset release */
    ESP_RETURN_ON_ERROR(cam_xclk_init(), TAG, "XCLK init failed");
    vTaskDelay(pdMS_TO_TICKS(5));

    /* 2. Camera reset via IO expander (P6 on 0x43, active low) */
    tab5_set_camera_reset(true);   /* Assert reset (pin LOW) */
    vTaskDelay(pdMS_TO_TICKS(10));
    tab5_set_camera_reset(false);  /* Release reset (pin HIGH) */
    vTaskDelay(pdMS_TO_TICKS(20)); /* SC2336 needs time after reset with XCLK running */

    /* 3. Initialize SCCB (I2C) — uses system I2C bus */
    extern i2c_master_bus_handle_t tab5_get_i2c_bus(void);
    i2c_master_bus_handle_t i2c_bus = tab5_get_i2c_bus();
    if (!i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not available");
        return ESP_ERR_INVALID_STATE;
    }

    /* Try primary address 0x30, fall back to 0x36 (SID pin variant) */
    esp_err_t sccb_ret = cam_sccb_init(i2c_bus);
    if (sccb_ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB init failed at 0x%02X", SC2336_SCCB_ADDR);
        return sccb_ret;
    }

    /* Probe: try reading chip ID at primary address */
    esp_err_t probe_ret = cam_check_sensor_id();
    if (probe_ret != ESP_OK) {
        ESP_LOGW(TAG, "SC2336 not found at 0x%02X, trying 0x36...", SC2336_SCCB_ADDR);
        /* Remove old device and try alternate address */
        i2c_master_bus_rm_device(s_sccb_dev);
        s_sccb_dev = NULL;
        i2c_device_config_t alt_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x36,
            .scl_speed_hz = 100000,
        };
        sccb_ret = i2c_master_bus_add_device(i2c_bus, &alt_cfg, &s_sccb_dev);
        if (sccb_ret != ESP_OK) {
            ESP_LOGE(TAG, "SCCB init at 0x36 also failed");
            return sccb_ret;
        }
        probe_ret = cam_check_sensor_id();
        if (probe_ret != ESP_OK) {
            ESP_LOGE(TAG, "SC2336 not found at 0x36 either — camera not responding");
            return probe_ret;
        }
        ESP_LOGI(TAG, "SC2336 found at alternate address 0x36");
    }

    /* Initialize sensor registers (VGA mode for now) */
    ESP_RETURN_ON_ERROR(cam_sensor_init_vga(), TAG, "Sensor init failed");

    /* Initialize CSI controller */
    ESP_RETURN_ON_ERROR(cam_csi_init(), TAG, "CSI init failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Camera initialized! SC2336 @ %dx%d", s_width, s_height);
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

    esp_cam_ctlr_trans_t trans = {
        .buffer = s_frame_buf,
        .buflen = CAM_FB_MAX_SIZE,
    };

    s_frame_size = 0;
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_receive(s_cam_ctrl, &trans, 2000), TAG, "Capture failed");

    /* Wait for frame done callback */
    /* TODO: Use semaphore/event instead of polling */
    int retries = 100;
    while (s_frame_size == 0 && retries-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_frame_size == 0) {
        ESP_LOGE(TAG, "Capture timeout — no frame received");
        return ESP_ERR_TIMEOUT;
    }

    frame->data = s_frame_buf;
    frame->size = s_frame_size;
    frame->width = s_width;
    frame->height = s_height;
    frame->format = TAB5_CAM_FMT_RGB565;

    ESP_LOGI(TAG, "Captured frame: %dx%d, %lu bytes", s_width, s_height, (unsigned long)s_frame_size);
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
             "SC2336 %dx%d MIPI-CSI %d-lane, XCLK=%dMHz, SCCB=0x%02X",
             s_width, s_height, CAM_CSI_LANE_NUM,
             CAM_XCLK_FREQ_HZ / 1000000, SC2336_SCCB_ADDR);
    return info;
}
