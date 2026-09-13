#pragma once

#include "dji_h1_board.h"

/* Diagnostic-only buffer sizes. Physical UART routing and baud rate come from
 * the shared production-board profile. */

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
