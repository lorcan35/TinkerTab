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

#ifdef __cplusplus
}
#endif
