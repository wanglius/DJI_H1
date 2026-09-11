#pragma once

#include "driver/uart.h"

/* DTU wiring and serial settings. Override these definitions from the build
 * system or edit this single header when the board routing changes. */
#ifndef DJI_DTU_UART_PORT
#define DJI_DTU_UART_PORT UART_NUM_1
#endif

#ifndef DJI_DTU_UART_TX_GPIO
#define DJI_DTU_UART_TX_GPIO 17
#endif

#ifndef DJI_DTU_UART_RX_GPIO
#define DJI_DTU_UART_RX_GPIO 18
#endif

#ifndef DJI_DTU_UART_BAUD_RATE
#define DJI_DTU_UART_BAUD_RATE 460800
#endif

#ifndef DJI_DTU_UART_RX_BUFFER_SIZE
#define DJI_DTU_UART_RX_BUFFER_SIZE 4096
#endif

#ifndef DJI_DTU_UART_TX_BUFFER_SIZE
#define DJI_DTU_UART_TX_BUFFER_SIZE 4096
#endif

#ifndef DJI_DTU_USB_RX_BUFFER_SIZE
#define DJI_DTU_USB_RX_BUFFER_SIZE 4096
#endif

#ifndef DJI_DTU_USB_TX_BUFFER_SIZE
#define DJI_DTU_USB_TX_BUFFER_SIZE 4096
#endif
