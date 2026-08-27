#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "acquisition.h"
#include "sc16is752.h"

static const char *TAG = "DJI_H1_DUAL";
#define TEST_DURATION_MS 30000

void app_main(void)
{
    printf("\n============================================================\n");
    printf(" DJI_H1 - DUAL UART 30-SECOND DIAGNOSTIC STREAMING TEST\n");
    printf("============================================================\n");
    ESP_LOGI(TAG, "Free heap at boot: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    const sc16_config_t config = {
        .spi_host = SPI2_HOST,
        .pin_mosi = 2, .pin_miso = 3, .pin_sclk = 5,
        .pin_cs = 1, .pin_reset = 6,
        /* Two simultaneous 115200-baud UARTs share this SPI bus. */
        .spi_clock_hz = 4000000, .crystal_hz = 1843200,
    };
    ESP_ERROR_CHECK(sc16_init(&config));

    esp_err_t ret = acquisition_run_dual(TEST_DURATION_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Dual acquisition failed: %s", esp_err_to_name(ret));
        return;
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
