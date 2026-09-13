#pragma once

#include "driver/uart.h"

/* Fixed memory population of the production module. Boot validation catches
 * a wrong module variant or unusable external memory before mission-ready. */
#define DJI_BOARD_EXPECTED_PSRAM_BYTES (8U * 1024U * 1024U)
#define DJI_BOARD_EXPECTED_FLASH_BYTES (16U * 1024U * 1024U)

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

/* 4G DTU UART. MQTT endpoint/topic/QoS and DTU-side packetization are
 * provisioned separately and retained by the DTU itself. */
#ifndef DJI_DTU_UART_TX_GPIO
#define DJI_DTU_UART_TX_GPIO 17
#endif

#ifndef DJI_DTU_UART_RX_GPIO
#define DJI_DTU_UART_RX_GPIO 18
#endif

#ifndef DJI_DTU_UART_PORT
#define DJI_DTU_UART_PORT UART_NUM_1
#endif

#define DJI_DTU_UART_BAUD_RATE 460800
#define DJI_DTU_FRAGMENT_GAP_MS 6
#define DJI_DTU_GPS_MIN_INTERVAL_MS 1000
#define DJI_DTU_ACK_TIMEOUT_MS 3000
#define DJI_DTU_MAX_RETRIES 1
#define DJI_DTU_TELEMETRY_POOL_LENGTH 512
#define DJI_DTU_TIMING_DIAGNOSTICS true
