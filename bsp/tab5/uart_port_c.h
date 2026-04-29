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

/**
 * @brief Acquire exclusive access to Port C UART.
 *
 * The K144 module + StackFlow protocol uses a single UART link shared by
 * every voice_m5_llm_* caller.  Concurrent send/recv from multiple
 * FreeRTOS tasks corrupts the response buffer
 * (see docs/AUDIT-k144-chain-2026-04-29.md item #1).  Every public entry
 * point in voice_m5_llm.c wraps its send/recv pair with this lock.
 * Long-lived consumers (chain drain loop) take/release per outer-loop
 * iteration so a stop-flag flip can land between frames.
 *
 * Recursive — same task may take the lock multiple times safely.
 * Lock is auto-created in tab5_port_c_uart_init().
 *
 * @param timeout_ms  Wait budget.  Use UINT32_MAX for portMAX_DELAY.
 * @return ESP_OK on acquisition;
 *         ESP_ERR_TIMEOUT on contention;
 *         ESP_ERR_INVALID_STATE if UART not yet initialised.
 */
esp_err_t tab5_port_c_lock(uint32_t timeout_ms);

/**
 * @brief Release the Port C UART lock.  Must be paired with a prior
 *        successful tab5_port_c_lock().  Safe to call on a recursively-
 *        held lock; releases one level.
 */
void tab5_port_c_unlock(void);

#ifdef __cplusplus
}
#endif
