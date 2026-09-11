#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dtu_bridge_config.h"

#define BRIDGE_CHUNK_SIZE 1024
#define BRIDGE_TASK_STACK_SIZE 4096
#define BRIDGE_TASK_PRIORITY 10

static const char *const TAG = "DTU_BRIDGE";

static void write_usb_all(const uint8_t *data, size_t length)
{
    while (length > 0) {
        int written = usb_serial_jtag_write_bytes(
            data, length, pdMS_TO_TICKS(100));
        if (written <= 0) {
            /* The PC may close the COM port. Drop the remainder rather than
             * blocking DTU reception indefinitely. */
            return;
        }
        data += written;
        length -= (size_t)written;
    }
}

static void write_uart_all(const uint8_t *data, size_t length)
{
    while (length > 0) {
        int written = uart_write_bytes(DJI_DTU_UART_PORT, data, length);
        if (written <= 0) {
            return;
        }
        data += written;
        length -= (size_t)written;
    }
}

static void bridge_task(void *context)
{
    (void)context;
    uint8_t pc_to_dtu[BRIDGE_CHUNK_SIZE];
    uint8_t dtu_to_pc[BRIDGE_CHUNK_SIZE];

    char banner[160];
    int banner_length = snprintf(
        banner, sizeof(banner),
        "\r\n[DJI_H1 DTU bridge: USB <-> UART%u, GPIO%u TX, GPIO%u RX, "
        "%u 8N1]\r\n",
        (unsigned)DJI_DTU_UART_PORT,
        (unsigned)DJI_DTU_UART_TX_GPIO,
        (unsigned)DJI_DTU_UART_RX_GPIO,
        (unsigned)DJI_DTU_UART_BAUD_RATE);
    if (banner_length > 0 && (size_t)banner_length < sizeof(banner)) {
        write_usb_all((const uint8_t *)banner, (size_t)banner_length);
    }

    for (;;) {
        int pc_length = usb_serial_jtag_read_bytes(
            pc_to_dtu, sizeof(pc_to_dtu), pdMS_TO_TICKS(2));
        if (pc_length > 0) {
            write_uart_all(pc_to_dtu, (size_t)pc_length);
        }

        int dtu_length = uart_read_bytes(
            DJI_DTU_UART_PORT, dtu_to_pc, sizeof(dtu_to_pc),
            pdMS_TO_TICKS(2));
        if (dtu_length > 0) {
            write_usb_all(dtu_to_pc, (size_t)dtu_length);
        }
    }
}

void app_main(void)
{
    const uart_config_t uart_config = {
        .baud_rate = DJI_DTU_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = DJI_DTU_USB_TX_BUFFER_SIZE,
        .rx_buffer_size = DJI_DTU_USB_RX_BUFFER_SIZE,
    };

    ESP_LOGI(TAG, "PC USB Serial/JTAG <-> DTU UART bridge");
    ESP_LOGI(TAG, "DTU UART%u TX=GPIO%u RX=GPIO%u %u 8N1",
             (unsigned)DJI_DTU_UART_PORT,
             (unsigned)DJI_DTU_UART_TX_GPIO,
             (unsigned)DJI_DTU_UART_RX_GPIO,
             (unsigned)DJI_DTU_UART_BAUD_RATE);

    ESP_ERROR_CHECK(uart_driver_install(
        DJI_DTU_UART_PORT, DJI_DTU_UART_RX_BUFFER_SIZE,
        DJI_DTU_UART_TX_BUFFER_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DJI_DTU_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(
        DJI_DTU_UART_PORT, DJI_DTU_UART_TX_GPIO, DJI_DTU_UART_RX_GPIO,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));

    /* From this point onward the PC stream is raw DTU traffic. Application
     * logging would corrupt binary payloads received by the PC. */
    esp_log_level_set("*", ESP_LOG_NONE);

    BaseType_t created = xTaskCreate(
        bridge_task, "dtu_uart_bridge", BRIDGE_TASK_STACK_SIZE,
        NULL, BRIDGE_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
