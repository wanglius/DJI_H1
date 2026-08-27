#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SC16_CHANNEL_A = 0,
    SC16_CHANNEL_B = 1
} sc16_channel_t;

typedef struct {
    spi_host_device_t spi_host;

    int pin_mosi;
    int pin_miso;
    int pin_sclk;
    int pin_cs;
    int pin_reset;

    uint32_t spi_clock_hz;
    uint32_t crystal_hz;
} sc16_config_t;


/**
 * Initialize SPI, reset SC16IS752, and verify basic communication.
 */
esp_err_t sc16_init(const sc16_config_t *config);


/**
 * Verify register access using the scratchpad register.
 */
esp_err_t sc16_test_channel(sc16_channel_t channel);


/**
 * Configure one SC16 UART.
 *
 * Initial implementation supports the proven configuration:
 * 115200 baud, 8N1, 1.8432 MHz crystal.
 */
esp_err_t sc16_uart_init(sc16_channel_t channel);



/**
 * Check whether RX data are available.
 */
bool sc16_rx_available(sc16_channel_t channel);


/**
 * Check whether transmitter is ready for another byte.
 */
bool sc16_tx_ready(sc16_channel_t channel);

/** Get the combined hardware RX FIFO overrun count. */
uint32_t sc16_get_rx_overrun_count(void);

/** Get the RX overrun count for one UART channel. */
uint32_t sc16_get_channel_rx_overrun_count(sc16_channel_t channel);

/** Reset both hardware RX FIFO overrun counters. */
void sc16_reset_rx_overrun_count(void);

/** Reset the RX overrun count for one UART channel. */
void sc16_reset_channel_rx_overrun_count(sc16_channel_t channel);

/**
 * Start one high-priority task that drains both hardware UART FIFOs into
 * independent ESP32 software stream buffers. Once active, sc16_rx_level()
 * and sc16_read_fifo() consume those software buffers.
 */
esp_err_t sc16_start_dual_rx_service(
    size_t buffer_size,
    UBaseType_t task_priority,
    BaseType_t core_id
);

/**
 * Stop the dual RX service and release its software stream buffers.
 *
 * The service exits cooperatively after its current FIFO-service cycle.
 */
esp_err_t sc16_stop_dual_rx_service(uint32_t timeout_ms);

/**
 * Get the cumulative number of bytes dropped because one software RX buffer
 * was full. Callers can snapshot this value to distinguish acquisition drops
 * from bytes deliberately discarded during shutdown.
 */
uint32_t sc16_get_software_rx_drop_count(sc16_channel_t channel);

/**
 * Write one byte.
 */
esp_err_t sc16_write_byte(
    sc16_channel_t channel,
    uint8_t data
);


/**
 * Read one byte.
 */
esp_err_t sc16_read_byte(
    sc16_channel_t channel,
    uint8_t *data
);


/**
 * Wait for transmitter ready.
 */
bool sc16_wait_tx_ready(
    sc16_channel_t channel,
    uint32_t timeout_ms
);


/**
 * Wait for receive data.
 */
bool sc16_wait_rx(
    sc16_channel_t channel,
    uint32_t timeout_ms
);


/**
 * Write a byte buffer.
 *
 * Returns number of bytes successfully transmitted.
 */
size_t sc16_write(
    sc16_channel_t channel,
    const uint8_t *data,
    size_t length,
    uint32_t timeout_ms
);


/**
 * Read available bytes.
 *
 * Returns number of bytes successfully received.
 */
size_t sc16_read(
    sc16_channel_t channel,
    uint8_t *buffer,
    size_t max_length
);

/**
 * Get the number of bytes currently stored
 * in the selected UART RX FIFO.
 */
esp_err_t sc16_rx_level(
    sc16_channel_t channel,
    uint8_t *level
);


/**
 * Read multiple bytes from the active RX source. Before the dual RX service
 * starts this is the hardware FIFO; afterwards it is the channel's software
 * stream buffer.
 *
 * Reads up to max_length bytes in one SPI transaction.
 * Direct hardware reads require max_length <= 64. Software-buffer reads may
 * be larger, but callers should still size the destination accordingly.
 *
 * Returns number of bytes read through bytes_read.
 */
esp_err_t sc16_read_fifo(
    sc16_channel_t channel,
    uint8_t *buffer,
    size_t max_length,
    size_t *bytes_read
);


/**
 * Read the SC16IS752 Line Status Register.
 */
esp_err_t sc16_get_lsr(
    sc16_channel_t channel,
    uint8_t *lsr
);

#ifdef __cplusplus
}
#endif
