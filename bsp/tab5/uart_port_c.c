/**
 * @file uart_port_c.c
 * @brief Implementation — see uart_port_c.h for API contract.
 */

#include "uart_port_c.h"

#include "bsp_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "uart_port_c";

#define PORT_C_UART_PORT (TAB5_PORT_C_UART_NUM)
/* TT #131 — bump from 256 to 4096.  At 1.5 Mbps, 256 bytes = 1.4 ms of
 * wire — too small to ride out the wakeword recv-loop holding the mutex. */
#define PORT_C_UART_RX_BUF_SZ (4096)
/* TT #131 — was 0 = synchronous-blocking mode for uart_write_bytes.
 * The pump was parked any time another caller held the recursive UART
 * mutex (e.g. voice_m5_llm_wakeword_run's recv loop), causing K144 to
 * see ~3 KB/s ingestion even at 1.5 Mbps.  8 KB ring = ~43 ms of wire,
 * which comfortably covers wakeword's per-iteration lock-hold. */
#define PORT_C_UART_TX_BUF_SZ (8192)

static bool s_initialized = false;
static uint32_t s_current_baud = TAB5_PORT_C_UART_BAUD;
static SemaphoreHandle_t s_uart_mutex = NULL; /* recursive — created in init */

esp_err_t tab5_port_c_uart_init(void) {
   if (s_initialized) {
      return ESP_OK;
   }

   /* Create the mutex once on first init.  Recursive so the same task can
    * re-enter (e.g. an audio_cb invoked from chain_run that itself wants
    * to call back into voice_m5_llm_*). */
   if (s_uart_mutex == NULL) {
      s_uart_mutex = xSemaphoreCreateRecursiveMutex();
      if (s_uart_mutex == NULL) {
         ESP_LOGE(TAG, "Port C UART mutex create failed");
         return ESP_ERR_NO_MEM;
      }
   }

   const uart_config_t cfg = {
       .baud_rate = TAB5_PORT_C_UART_BAUD,
       .data_bits = UART_DATA_8_BITS,
       .parity = UART_PARITY_DISABLE,
       .stop_bits = UART_STOP_BITS_1,
       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
       .source_clk = UART_SCLK_DEFAULT,
   };

   esp_err_t err =
       uart_driver_install(PORT_C_UART_PORT, PORT_C_UART_RX_BUF_SZ, PORT_C_UART_TX_BUF_SZ, 0, NULL, 0);
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

esp_err_t tab5_port_c_uart_reinit(void) {
   if (s_uart_mutex == NULL) return ESP_ERR_INVALID_STATE;
   /* Hold the mutex across delete+install so concurrent send/recv from
    * the ext_pcm pump or voice_m5_llm.c can't race the swap.  Recursive
    * so the watchdog (already holding the lock in its outer flow) is
    * safe. */
   if (xSemaphoreTakeRecursive(s_uart_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      ESP_LOGW(TAG, "reinit: mutex busy — skipping");
      return ESP_ERR_TIMEOUT;
   }

   uint32_t baud = s_current_baud;
   if (s_initialized) {
      uart_driver_delete(PORT_C_UART_PORT);
      s_initialized = false;
   }

   const uart_config_t cfg = {
       .baud_rate = baud,
       .data_bits = UART_DATA_8_BITS,
       .parity = UART_PARITY_DISABLE,
       .stop_bits = UART_STOP_BITS_1,
       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
       .source_clk = UART_SCLK_DEFAULT,
   };

   esp_err_t err = uart_driver_install(PORT_C_UART_PORT, PORT_C_UART_RX_BUF_SZ, PORT_C_UART_TX_BUF_SZ, 0, NULL, 0);
   if (err == ESP_OK) err = uart_param_config(PORT_C_UART_PORT, &cfg);
   if (err == ESP_OK) {
      err = uart_set_pin(PORT_C_UART_PORT, TAB5_PORT_C_UART_TX_GPIO, TAB5_PORT_C_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE);
   }

   if (err != ESP_OK) {
      ESP_LOGE(TAG, "reinit failed at install/config/pin: %s", esp_err_to_name(err));
      xSemaphoreGiveRecursive(s_uart_mutex);
      return err;
   }

   s_initialized = true;
   ESP_LOGW(TAG, "Port C UART reinstalled at %lu baud (driver-level wedge recovery)", (unsigned long)baud);
   xSemaphoreGiveRecursive(s_uart_mutex);
   return ESP_OK;
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

esp_err_t tab5_port_c_uart_set_baud(uint32_t baud) {
   if (!s_initialized) return ESP_ERR_INVALID_STATE;
   /* Drain TX before flipping so any in-flight bytes finish at the old
    * baud — uart_set_baudrate doesn't wait for the FIFO. */
   uart_wait_tx_done(PORT_C_UART_PORT, pdMS_TO_TICKS(50));
   esp_err_t err = uart_set_baudrate(PORT_C_UART_PORT, baud);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "uart_set_baudrate(%lu) failed: %s", (unsigned long)baud, esp_err_to_name(err));
      return err;
   }
   s_current_baud = baud;
   ESP_LOGI(TAG, "Port C UART baud → %lu", (unsigned long)baud);
   return ESP_OK;
}

uint32_t tab5_port_c_uart_get_baud(void) { return s_current_baud; }

esp_err_t tab5_port_c_lock(uint32_t timeout_ms) {
   if (s_uart_mutex == NULL) return ESP_ERR_INVALID_STATE;
   const TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
   return (xSemaphoreTakeRecursive(s_uart_mutex, ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void tab5_port_c_unlock(void) {
   if (s_uart_mutex == NULL) return;
   xSemaphoreGiveRecursive(s_uart_mutex);
}
