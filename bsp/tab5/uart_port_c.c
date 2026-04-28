/**
 * @file uart_port_c.c
 * @brief Implementation — see uart_port_c.h for API contract.
 */

#include "uart_port_c.h"

#include "bsp_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uart_port_c";

#define PORT_C_UART_PORT (TAB5_PORT_C_UART_NUM)
#define PORT_C_UART_RX_BUF_SZ (256)

static bool s_initialized = false;

esp_err_t tab5_port_c_uart_init(void) {
   if (s_initialized) {
      return ESP_OK;
   }

   const uart_config_t cfg = {
       .baud_rate = TAB5_PORT_C_UART_BAUD,
       .data_bits = UART_DATA_8_BITS,
       .parity = UART_PARITY_DISABLE,
       .stop_bits = UART_STOP_BITS_1,
       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
       .source_clk = UART_SCLK_DEFAULT,
   };

   esp_err_t err = uart_driver_install(PORT_C_UART_PORT, PORT_C_UART_RX_BUF_SZ, 0, 0, NULL, 0);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
      return err;
   }

   err = uart_param_config(PORT_C_UART_PORT, &cfg);
   if (err != ESP_OK) {
      uart_driver_delete(PORT_C_UART_PORT);
      return err;
   }

   err = uart_set_pin(PORT_C_UART_PORT, TAB5_PORT_C_UART_TX_GPIO, TAB5_PORT_C_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                      UART_PIN_NO_CHANGE);
   if (err != ESP_OK) {
      uart_driver_delete(PORT_C_UART_PORT);
      return err;
   }

   s_initialized = true;
   ESP_LOGI(TAG, "Port C UART ready (TX=%d, RX=%d, %d 8N1)", TAB5_PORT_C_UART_TX_GPIO, TAB5_PORT_C_UART_RX_GPIO,
            TAB5_PORT_C_UART_BAUD);
   return ESP_OK;
}

void tab5_port_c_uart_deinit(void) {
   if (!s_initialized) return;
   uart_driver_delete(PORT_C_UART_PORT);
   s_initialized = false;
}

bool tab5_port_c_uart_is_initialized(void) { return s_initialized; }

int tab5_port_c_send(const void *buf, size_t len) {
   if (!s_initialized || buf == NULL) return -1;
   return uart_write_bytes(PORT_C_UART_PORT, buf, len);
}

int tab5_port_c_recv(void *buf, size_t len, uint32_t timeout_ms) {
   if (!s_initialized || buf == NULL) return -1;
   int n = uart_read_bytes(PORT_C_UART_PORT, buf, len, pdMS_TO_TICKS(timeout_ms));
   return (n < 0) ? 0 : n;
}

void tab5_port_c_flush(void) {
   if (!s_initialized) return;
   uart_flush_input(PORT_C_UART_PORT);
}
