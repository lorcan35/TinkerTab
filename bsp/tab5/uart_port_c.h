/**
 * @file uart_port_c.h
 * @brief Tab5 Port C UART driver — exposed at the side 4-pin header AND
 *        M5-Bus rear pins 15/16 (same wires).  Used to talk to stacked
 *        add-ons such as the M5 K144 LLM Module Kit.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tab5_port_c_uart_init(void);
void tab5_port_c_uart_deinit(void);
bool tab5_port_c_uart_is_initialized(void);
int tab5_port_c_send(const void *buf, size_t len);
int tab5_port_c_recv(void *buf, size_t len, uint32_t timeout_ms);
void tab5_port_c_flush(void);

/**
 * @brief Change the active UART baud rate (Phase 6a — adaptive baud).
 *
 * Wraps `uart_set_baudrate` so callers don't need to include `driver/uart.h`.
 * Caller is responsible for the protocol-level negotiation with the peer
 * (e.g. sys.uartsetup on the K144 side) — this only flips Tab5's UART.
 *
 * @param baud  New baud rate.  ESP32-P4 supports up to 5 Mbps on
 *              UART_NUM_1; signal integrity over the M5-Bus + Mate FPC
 *              chain caps the practical ceiling around 1 Mbps.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if not initialised;
 *         propagated from `uart_set_baudrate` otherwise.
 */
esp_err_t tab5_port_c_uart_set_baud(uint32_t baud);

/** Current Tab5-side UART baud (last successful set, or init default). */
uint32_t tab5_port_c_uart_get_baud(void);

#ifdef __cplusplus
}
#endif
