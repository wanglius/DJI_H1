#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int uart_port_t;
typedef struct { uart_port_t uart_port; int tx_gpio, rx_gpio; uint32_t baud_rate; } telemetry_config_t;
int uart_write_bytes(uart_port_t port, const void *data, size_t length);
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks);
int uart_read_bytes(uart_port_t port, void *data, uint32_t length, TickType_t ticks);
esp_err_t uart_flush_input(uart_port_t port);
