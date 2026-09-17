#pragma once

#include <stdbool.h>

#include "driver/spi_master.h"
#include "driver/uart.h"
#include "sdkconfig.h"

/* Single source of truth for the production ESP32-S3-WROOM-1U-N16R8 board.
 * Reusable peripheral components accept runtime configuration and must not
 * embed these assignments. Diagnostic firmware should include this profile
 * unless it intentionally targets different hardware. */

#define DJI_BOARD_EXPECTED_PSRAM_BYTES (8U * 1024U * 1024U)
#define DJI_BOARD_EXPECTED_FLASH_BYTES (16U * 1024U * 1024U)

/* Drone A-board link on the ESP32-S3 native UART0 pins. */
#define DJI_AB_UART_PORT UART_NUM_0
#ifndef DJI_AB_UART_TX_GPIO
#define DJI_AB_UART_TX_GPIO 43
#endif
#ifndef DJI_AB_UART_RX_GPIO
#define DJI_AB_UART_RX_GPIO 44
#endif
#define DJI_AB_UART_BAUD_RATE 115200

/* YY-M200 4G DTU link. The module must be persistently provisioned to match. */
#define DJI_DTU_UART_PORT UART_NUM_1
#ifndef DJI_DTU_UART_TX_GPIO
#define DJI_DTU_UART_TX_GPIO 17
#endif
#ifndef DJI_DTU_UART_RX_GPIO
#define DJI_DTU_UART_RX_GPIO 18
#endif
#define DJI_DTU_UART_BAUD_RATE 460800
#define DJI_DTU_FRAGMENT_GAP_MS 0
#define DJI_DTU_ACK_TIMEOUT_MS 3000
#define DJI_DTU_MAX_RETRIES 1
#define DJI_DTU_TELEMETRY_POOL_LENGTH 512
/* Preserve the normal 3 s + 6 s acknowledgement/retry window, then prefer
 * fresh flight data over retaining an obsolete cloud-delivery backlog. */
#define DJI_DTU_MAX_RESIDENCY_MS 10000
/* Combine two seconds of the 5 Hz A-board navigation stream into one compact
 * DGB1 telemetry message. GPS_TRACK.BIN remains individual DHR1 records. */
#define DJI_DTU_GPS_BATCH_MAX_RECORDS 10
#define DJI_DTU_GPS_BATCH_MAX_DELAY_MS 2000
/* Qualified 4 Hz latest-value telemetry cadence. The 250 ms selector changes
 * only MQTT admission; SD recording remains full-rate and independent. */
#define DJI_DTU_REFLECTANCE_INTERVAL_MS 250

#ifdef CONFIG_DJI_H1_TELEMETRY_TIMING_DIAGNOSTICS
#define DJI_DTU_TIMING_DIAGNOSTICS true
#else
#define DJI_DTU_TIMING_DIAGNOSTICS false
#endif

/* Removable mission-storage card on SPI2. */
#define DJI_SD_SPI_HOST SPI2_HOST
#define DJI_SD_PIN_CS 10
#define DJI_SD_PIN_MOSI 11
#define DJI_SD_PIN_SCLK 12
#define DJI_SD_PIN_MISO 13
#define DJI_SD_MAX_FREQUENCY_KHZ 10000
#define DJI_SD_MAX_TRANSFER_SIZE 4096
#define DJI_SD_MAX_OPEN_FILES 8
#define DJI_SD_MOUNT_POINT "/sdcard"

/* Dual H1 UART bridge on SPI3. */
#define DJI_SC16_SPI_HOST SPI3_HOST
#define DJI_SC16_PIN_MOSI 2
#define DJI_SC16_PIN_MISO 3
#define DJI_SC16_PIN_SCLK 5
#define DJI_SC16_PIN_CS 1
#define DJI_SC16_PIN_RESET 6
#define DJI_SC16_SPI_CLOCK_HZ 4000000
#define DJI_SC16_CRYSTAL_HZ 1843200
