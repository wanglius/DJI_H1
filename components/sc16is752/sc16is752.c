#include "sc16is752.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"


static const char *TAG = "SC16IS752";

static volatile uint32_t s_rx_overrun_count = 0;

static spi_device_handle_t s_spi = NULL;
static sc16_config_t s_config;


// ============================================================
// SC16IS752 registers
// ============================================================

#define REG_RHR_THR     0x00
#define REG_IER         0x01
#define REG_FCR_IIR     0x02
#define REG_LCR         0x03
#define REG_MCR         0x04
#define REG_LSR         0x05
#define REG_MSR         0x06
#define REG_SPR         0x07
#define REG_TXLVL       0x08
#define REG_RXLVL       0x09

#define REG_DLL         0x00
#define REG_DLH         0x01



// ============================================================
// LSR bits
// ============================================================

#define LSR_DR          (1 << 0)
#define LSR_OE          (1 << 1)   // RX overrun
#define LSR_PE          (1 << 2)
#define LSR_FE          (1 << 3)
#define LSR_BI          (1 << 4)
#define LSR_THRE        (1 << 5)
#define LSR_TEMT        (1 << 6)
#define LSR_FIFO_ERR    (1 << 7)


// ============================================================
// FCR bits
// ============================================================

#define FCR_FIFO_EN     (1 << 0)
#define FCR_RX_RESET    (1 << 1)
#define FCR_TX_RESET    (1 << 2)


// ============================================================
// LCR values
// ============================================================

#define LCR_8N1         0x03
#define LCR_DLAB        0x80


// ============================================================
// Register addressing
// ============================================================

static uint8_t sc16_reg_addr(
    uint8_t reg,
    sc16_channel_t channel,
    bool read)
{
    return (read ? 0x80 : 0x00)
         | ((reg & 0x0F) << 3)
         | (((uint8_t)channel & 0x03) << 1);
}


// ============================================================
// Low-level register write
// ============================================================

static esp_err_t sc16_write_reg(
    sc16_channel_t channel,
    uint8_t reg,
    uint8_t value)
{
    uint8_t tx[2];

    tx[0] = sc16_reg_addr(reg, channel, false);
    tx[1] = value;

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };

    return spi_device_transmit(s_spi, &t);
}


// ============================================================
// Low-level register read
// ============================================================

static esp_err_t sc16_read_reg(
    sc16_channel_t channel,
    uint8_t reg,
    uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[2] = {
        sc16_reg_addr(reg, channel, true),
        0x00
    };

    uint8_t rx[2] = {0};

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t ret = spi_device_transmit(s_spi, &t);

    if (ret == ESP_OK) {
        *value = rx[1];
    }

    return ret;
}


// ============================================================
// Hardware reset
// ============================================================

static esp_err_t sc16_reset(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_config.pin_reset),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);

    if (ret != ESP_OK) {
        return ret;
    }

    gpio_set_level(s_config.pin_reset, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level(s_config.pin_reset, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Hardware reset complete");

    return ESP_OK;
}


// ============================================================
// Initialization
// ============================================================

esp_err_t sc16_init(const sc16_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;

    spi_bus_config_t buscfg = {
        .mosi_io_num = config->pin_mosi,
        .miso_io_num = config->pin_miso,
        .sclk_io_num = config->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    esp_err_t ret = spi_bus_initialize(
        config->spi_host,
        &buscfg,
        SPI_DMA_CH_AUTO
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "spi_bus_initialize failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = config->spi_clock_hz,
        .mode = 0,
        .spics_io_num = config->pin_cs,
        .queue_size = 1,
    };

    ret = spi_bus_add_device(
        config->spi_host,
        &devcfg,
        &s_spi
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "spi_bus_add_device failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG,
             "SPI initialized: %lu Hz",
             (unsigned long)config->spi_clock_hz);

    ret = sc16_reset();

    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}


// ============================================================
// Scratchpad register test
// ============================================================

esp_err_t sc16_test_channel(sc16_channel_t channel)
{
    uint8_t value;

    ESP_LOGI(TAG,
             "Testing UART-%c register access",
             channel == SC16_CHANNEL_A ? 'A' : 'B');

    esp_err_t ret =
        sc16_write_reg(channel, REG_SPR, 0x55);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = sc16_read_reg(channel, REG_SPR, &value);

    if (ret != ESP_OK) {
        return ret;
    }

    if (value != 0x55) {
        ESP_LOGE(TAG,
                 "SPR mismatch: expected 0x55, got 0x%02X",
                 value);
        return ESP_FAIL;
    }

    ret = sc16_write_reg(channel, REG_SPR, 0xAA);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = sc16_read_reg(channel, REG_SPR, &value);

    if (ret != ESP_OK) {
        return ret;
    }

    if (value != 0xAA) {
        ESP_LOGE(TAG,
                 "SPR mismatch: expected 0xAA, got 0x%02X",
                 value);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "UART-%c register test passed",
             channel == SC16_CHANNEL_A ? 'A' : 'B');

    return ESP_OK;
}


// ============================================================
// UART configuration
// ============================================================

esp_err_t sc16_uart_init(sc16_channel_t channel)
{
    /*
     * Proven hardware configuration:
     *
     * SC16 clock = 1.8432 MHz
     * divisor    = 1
     *
     * 1.8432 MHz / (16 * 1) = 115200 baud
     *
     * UART format = 8N1
     */

    ESP_LOGI(TAG,
             "Configuring UART-%c: 115200 8N1",
             channel == SC16_CHANNEL_A ? 'A' : 'B');

    esp_err_t ret;

    ret = sc16_write_reg(
        channel,
        REG_LCR,
        LCR_DLAB
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ret = sc16_write_reg(
        channel,
        REG_DLL,
        0x01
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ret = sc16_write_reg(
        channel,
        REG_DLH,
        0x00
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ret = sc16_write_reg(
        channel,
        REG_LCR,
        LCR_8N1
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ret = sc16_write_reg(
        channel,
        REG_FCR_IIR,
        FCR_FIFO_EN |
        FCR_RX_RESET |
        FCR_TX_RESET
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ret = sc16_write_reg(
        channel,
        REG_MCR,
        0x00
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "UART-%c configured",
             channel == SC16_CHANNEL_A ? 'A' : 'B');

    return ESP_OK;
}


// ============================================================
// Status
// ============================================================

bool sc16_rx_available(sc16_channel_t channel)
{
    uint8_t lsr;

    if (sc16_read_reg(
            channel,
            REG_LSR,
            &lsr) != ESP_OK) {
        return false;
    }

    if (lsr & LSR_OE) {
        s_rx_overrun_count++;
    }

    return (lsr & LSR_DR) != 0;
}


bool sc16_tx_ready(sc16_channel_t channel)
{
    uint8_t lsr;

    if (sc16_read_reg(
            channel,
            REG_LSR,
            &lsr) != ESP_OK) {
        return false;
    }

    return (lsr & LSR_THRE) != 0;
}


// ============================================================
// One-byte TX / RX
// ============================================================

esp_err_t sc16_write_byte(
    sc16_channel_t channel,
    uint8_t data)
{
    return sc16_write_reg(
        channel,
        REG_RHR_THR,
        data
    );
}




esp_err_t sc16_read_byte(
    sc16_channel_t channel,
    uint8_t *data)
{
    return sc16_read_reg(
        channel,
        REG_RHR_THR,
        data
    );
}


// ============================================================
// Wait functions
// ============================================================

bool sc16_wait_tx_ready(
    sc16_channel_t channel,
    uint32_t timeout_ms)
{
    while (timeout_ms > 0) {

        if (sc16_tx_ready(channel)) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
        timeout_ms--;
    }

    return false;
}


bool sc16_wait_rx(
    sc16_channel_t channel,
    uint32_t timeout_ms)
{
    while (timeout_ms > 0) {

        if (sc16_rx_available(channel)) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
        timeout_ms--;
    }

    return false;
}


// ============================================================
// Buffer TX
// ============================================================

size_t sc16_write(
    sc16_channel_t channel,
    const uint8_t *data,
    size_t length,
    uint32_t timeout_ms)
{
    if (data == NULL) {
        return 0;
    }

    size_t written = 0;

    while (written < length) {

        if (!sc16_wait_tx_ready(
                channel,
                timeout_ms)) {
            break;
        }

        if (sc16_write_byte(
                channel,
                data[written]) != ESP_OK) {
            break;
        }

        written++;
    }

    return written;
}


// ============================================================
// Buffer RX
// ============================================================

size_t sc16_read(
    sc16_channel_t channel,
    uint8_t *buffer,
    size_t max_length)
{
    if (buffer == NULL) {
        return 0;
    }

    size_t count = 0;

    while (count < max_length &&
           sc16_rx_available(channel)) {

        if (sc16_read_byte(
                channel,
                &buffer[count]) != ESP_OK) {
            break;
        }

        count++;
    }

    return count;
}


/*
 *pull the fifo all at once
 */
esp_err_t sc16_rx_level(
    sc16_channel_t channel,
    uint8_t *level)
{
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return sc16_read_reg(
        channel,
        REG_RXLVL,
        level
    );
}


/*
 *check the fifo overrun count
 */
uint32_t sc16_get_rx_overrun_count(void)
{
    return s_rx_overrun_count;
}

/**
 * Reset the RX overrun counter.
 */
void sc16_reset_rx_overrun_count(void)
{
    s_rx_overrun_count = 0;
}

/*
 * very fast FIFO burst read
 */
esp_err_t sc16_read_fifo(
    sc16_channel_t channel,
    uint8_t *buffer,
    size_t max_length,
    size_t *bytes_read)
{
    if (buffer == NULL ||
        bytes_read == NULL) {

        return ESP_ERR_INVALID_ARG;
    }

    *bytes_read = 0;


    if (max_length == 0) {
        return ESP_OK;
    }


    /*
     * SC16IS752 RX FIFO depth = 64 bytes.
     */
    if (max_length > 64) {
        max_length = 64;
    }


    /*
     * SPI transaction:
     *
     * byte 0:
     *     register address = RHR read
     *
     * following bytes:
     *     dummy TX bytes while receiving FIFO content
     *
     * Maximum:
     *
     * 1 command byte + 64 FIFO bytes
     */
    uint8_t tx[65] = {0};
    uint8_t rx[65] = {0};


    tx[0] =
        sc16_reg_addr(
            REG_RHR_THR,
            channel,
            true
        );


    spi_transaction_t t = {
        .length =
            (max_length + 1) * 8,

        .tx_buffer = tx,
        .rx_buffer = rx,
    };


    esp_err_t ret =
        spi_device_transmit(
            s_spi,
            &t
        );


    if (ret != ESP_OK) {
        return ret;
    }


    /*
     * rx[0] corresponds to the command/address phase.
     * FIFO data begin at rx[1].
     */
    memcpy(
        buffer,
        &rx[1],
        max_length
    );


    *bytes_read =
        max_length;


    return ESP_OK;
}