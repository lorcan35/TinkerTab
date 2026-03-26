/**
 * TinkerClaw Tab5 — IO Expander driver (PI4IOE5V6416)
 *
 * Ported from M5Stack Tab5 BSP. Controls LCD/touch reset, WiFi power, etc.
 */

#include "io_expander.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tab5_ioexp";

// PI4IO registers
#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR     0x03
#define PI4IO_REG_OUT_SET    0x05
#define PI4IO_REG_OUT_H_IM   0x07
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN    0x0B
#define PI4IO_REG_PULL_SEL   0x0D
#define PI4IO_REG_IN_STA     0x0F
#define PI4IO_REG_INT_MASK   0x11

#define I2C_TIMEOUT_MS 50

#define setbit(x, y) ((x) |= (1 << (y)))
#define clrbit(x, y) ((x) &= ~(1 << (y)))

static i2c_master_dev_handle_t s_pi4ioe1 = NULL;
static i2c_master_dev_handle_t s_pi4ioe2 = NULL;

static void pi4io_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_master_transmit(dev, buf, 2, I2C_TIMEOUT_MS);
}

static uint8_t pi4io_read(i2c_master_dev_handle_t dev, uint8_t reg)
{
    uint8_t buf[1] = {reg};
    uint8_t val[1] = {0};
    i2c_master_transmit_receive(dev, buf, 1, val, 1, I2C_TIMEOUT_MS);
    return val[0];
}

static void pi4io_set_bit(i2c_master_dev_handle_t dev, uint8_t bit, bool set)
{
    uint8_t val = pi4io_read(dev, PI4IO_REG_OUT_SET);
    if (set) {
        setbit(val, bit);
    } else {
        clrbit(val, bit);
    }
    pi4io_write(dev, PI4IO_REG_OUT_SET, val);
}

esp_err_t tab5_io_expander_init(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "Initializing IO expanders");

    // PI4IOE1 at 0x43
    i2c_device_config_t cfg1 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TAB5_PI4IOE1_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg1, &s_pi4ioe1));

    pi4io_write(s_pi4ioe1, PI4IO_REG_CHIP_RESET, 0xFF);
    pi4io_read(s_pi4ioe1, PI4IO_REG_CHIP_RESET);  // read to complete reset
    pi4io_write(s_pi4ioe1, PI4IO_REG_IO_DIR,    0b01111111);  // P7=input, rest output
    pi4io_write(s_pi4ioe1, PI4IO_REG_OUT_H_IM,  0b00000000);
    pi4io_write(s_pi4ioe1, PI4IO_REG_PULL_SEL,  0b01111111);  // pull up
    pi4io_write(s_pi4ioe1, PI4IO_REG_PULL_EN,   0b01111111);
    // P1=SPK_EN, P2=EXT5V_EN, P4=LCD_RST, P5=TP_RST, P6=CAM_RST high
    pi4io_write(s_pi4ioe1, PI4IO_REG_OUT_SET,   0b01110110);

    ESP_LOGI(TAG, "PI4IOE1 (0x43) initialized");

    // PI4IOE2 at 0x44
    i2c_device_config_t cfg2 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TAB5_PI4IOE2_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg2, &s_pi4ioe2));

    pi4io_write(s_pi4ioe2, PI4IO_REG_CHIP_RESET, 0xFF);
    pi4io_read(s_pi4ioe2, PI4IO_REG_CHIP_RESET);
    pi4io_write(s_pi4ioe2, PI4IO_REG_IO_DIR,    0b10111001);
    pi4io_write(s_pi4ioe2, PI4IO_REG_OUT_H_IM,  0b00000110);
    pi4io_write(s_pi4ioe2, PI4IO_REG_PULL_SEL,  0b10111001);
    pi4io_write(s_pi4ioe2, PI4IO_REG_PULL_EN,   0b11111001);
    pi4io_write(s_pi4ioe2, PI4IO_REG_IN_DEF_STA, 0b01000000);
    pi4io_write(s_pi4ioe2, PI4IO_REG_INT_MASK,  0b10111111);
    // P0=WLAN_PWR_EN, P3=USB5V_EN high
    pi4io_write(s_pi4ioe2, PI4IO_REG_OUT_SET,   0b00001001);

    ESP_LOGI(TAG, "PI4IOE2 (0x44) initialized");
    return ESP_OK;
}

void tab5_set_wifi_power(bool en)
{
    if (!s_pi4ioe2) return;
    pi4io_set_bit(s_pi4ioe2, 0, en);
    ESP_LOGI(TAG, "WiFi power: %s", en ? "ON" : "OFF");
}

void tab5_set_lcd_reset(bool active)
{
    if (!s_pi4ioe1) return;
    // P4 = LCD_RST, active low
    pi4io_set_bit(s_pi4ioe1, 4, !active);
}

void tab5_set_touch_reset(bool active)
{
    if (!s_pi4ioe1) return;
    // P5 = TP_RST, active low
    pi4io_set_bit(s_pi4ioe1, 5, !active);
}

void tab5_reset_display_and_touch(void)
{
    ESP_LOGI(TAG, "Resetting LCD and touch");

    // Read-modify-write both reset bits simultaneously (matching BSP behavior)
    if (!s_pi4ioe1) return;
    uint8_t val = pi4io_read(s_pi4ioe1, PI4IO_REG_OUT_SET);

    // Assert reset: clear P4 (LCD_RST) and P5 (TP_RST)
    val &= ~((1 << 4) | (1 << 5));
    pi4io_write(s_pi4ioe1, PI4IO_REG_OUT_SET, val);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Release reset: set P4 and P5 back high
    val |= ((1 << 4) | (1 << 5));
    pi4io_write(s_pi4ioe1, PI4IO_REG_OUT_SET, val);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "LCD and touch reset complete");
}
