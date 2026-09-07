#pragma once

#include "driver/uart.h"

/* A-board UART wiring on the ESP32-S3 native UART0 pins. The application
 * console remains on USB Serial/JTAG, so UART0 is dedicated to this link.
 * Override these definitions at build time for another board revision. */
#ifndef DJI_AB_UART_TX_GPIO
#define DJI_AB_UART_TX_GPIO 43
#endif

#ifndef DJI_AB_UART_RX_GPIO
#define DJI_AB_UART_RX_GPIO 44
#endif

#define DJI_AB_UART_BAUD_RATE 115200

#ifndef DJI_AB_UART_PORT
#define DJI_AB_UART_PORT UART_NUM_0
#endif
