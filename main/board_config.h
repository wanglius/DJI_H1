#pragma once

#include "driver/uart.h"

/* A-board UART wiring. Override at build time or edit only these definitions. */
#ifndef DJI_AB_UART_TX_GPIO
#define DJI_AB_UART_TX_GPIO 17
#endif

#ifndef DJI_AB_UART_RX_GPIO
#define DJI_AB_UART_RX_GPIO 18
#endif

#define DJI_AB_UART_BAUD_RATE 115200

#ifndef DJI_AB_UART_PORT
#define DJI_AB_UART_PORT UART_NUM_1
#endif
