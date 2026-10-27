#pragma once
#include <cstdint>
using uart_port_t = int;
enum uart_parity_t { UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD };
constexpr int ESP_OK = 0, UART_STOP_BITS_1 = 1, UART_STOP_BITS_2 = 2;
inline int uart_set_baudrate(uart_port_t, uint32_t) { return ESP_OK; }
inline int uart_set_parity(uart_port_t, uart_parity_t) { return ESP_OK; }
inline int uart_set_stop_bits(uart_port_t, int) { return ESP_OK; }
inline int uart_wait_tx_done(uart_port_t, int) { return ESP_OK; }
