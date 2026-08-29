#include "coexistence_test.h"

#include <stdbool.h>
#include <string.h>

#include "acquisition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sc16is752.h"
#include "sd_card.h"

static const char *TAG = "COEXIST_TEST";

#define SD_TEST_BLOCK_SIZE 512
#define SD_TEST_WRITE_PERIOD_MS 100
#define SD_TEST_FLUSH_BLOCKS 10

typedef struct {
    sd_card_file_t *file;
    SemaphoreHandle_t done;
    bool run;
    esp_err_t result;
    uint32_t blocks_written;
    uint64_t bytes_written;
} sd_writer_context_t;

static void sd_writer_task(void *arg)
{
    sd_writer_context_t *ctx = arg;
    uint8_t block[SD_TEST_BLOCK_SIZE];

    while (__atomic_load_n(&ctx->run, __ATOMIC_ACQUIRE)) {
        memset(block, 0xA5, sizeof(block));
        uint32_t block_number = ctx->blocks_written;
        int64_t timestamp_us = esp_timer_get_time();
        memcpy(block, &block_number, sizeof(block_number));
        memcpy(block + sizeof(block_number), &timestamp_us, sizeof(timestamp_us));

        size_t written = 0;
        ctx->result = sd_card_file_write(
            ctx->file, block, sizeof(block), &written);
        if (ctx->result != ESP_OK || written != sizeof(block)) {
            ESP_LOGE(TAG, "SD background write failed at block %lu",
                     (unsigned long)ctx->blocks_written);
            if (ctx->result == ESP_OK) ctx->result = ESP_FAIL;
            break;
        }
        ctx->blocks_written++;
        ctx->bytes_written += written;

        if ((ctx->blocks_written % SD_TEST_FLUSH_BLOCKS) == 0) {
            ctx->result = sd_card_file_flush(ctx->file);
            if (ctx->result != ESP_OK) {
                ESP_LOGE(TAG, "SD periodic flush failed");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SD_TEST_WRITE_PERIOD_MS));
    }

    if (ctx->result == ESP_OK) {
        ctx->result = sd_card_file_flush(ctx->file);
    }
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

esp_err_t coexistence_test_run(uint32_t duration_ms)
{
    esp_err_t result = ESP_OK;
    bool mounted = false;
    bool writer_started = false;
    sd_writer_context_t writer = {0};

    const sd_card_config_t sd_config =
        SD_CARD_LILYGO_T8_S3_DEFAULT_CONFIG();
    ESP_LOGI(TAG, "SD card: SPI2 CS=%d MOSI=%d SCLK=%d MISO=%d",
             sd_config.pin_cs, sd_config.pin_mosi,
             sd_config.pin_sclk, sd_config.pin_miso);
    result = sd_card_mount(&sd_config);
    if (result != ESP_OK) return result;
    mounted = true;
    result = sd_card_print_info(stdout);
    if (result != ESP_OK) goto cleanup;

    const sc16_config_t sc16_config = {
        .spi_host = SPI3_HOST,
        .pin_mosi = 2,
        .pin_miso = 3,
        .pin_sclk = 5,
        .pin_cs = 1,
        .pin_reset = 6,
        .spi_clock_hz = 4000000,
        .crystal_hz = 1843200,
    };
    ESP_LOGI(TAG, "SC16IS752: SPI3 CS=%d MOSI=%d SCLK=%d MISO=%d",
             sc16_config.pin_cs, sc16_config.pin_mosi,
             sc16_config.pin_sclk, sc16_config.pin_miso);
    result = sc16_init(&sc16_config);
    if (result != ESP_OK) goto cleanup;

    /*
     * Flashing resets the ESP32 and SC16IS752 while the H1 spectrometers may
     * remain powered. Leave both UARTs idle before sending the first command
     * to isolate reset-domain settling from protocol recovery behavior.
     */
    ESP_LOGI(TAG, "Waiting 500 ms for UART/H1 settling after SC16 reset");
    vTaskDelay(pdMS_TO_TICKS(500));

    writer.result = ESP_OK;
    writer.done = xSemaphoreCreateBinary();
    if (writer.done == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    result = sd_card_file_open("COEXIST.BIN", "wb", &writer.file);
    if (result != ESP_OK) goto cleanup;

    __atomic_store_n(&writer.run, true, __ATOMIC_RELEASE);
    if (xTaskCreatePinnedToCore(sd_writer_task, "sd_coexist", 4096,
                                &writer, 3, NULL, 0) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    writer_started = true;

    ESP_LOGI(TAG, "Starting simultaneous acquisition and SD writes for %.1fs",
             duration_ms / 1000.0);
    result = acquisition_run_dual(duration_ms);

cleanup:
    if (writer_started) {
        __atomic_store_n(&writer.run, false, __ATOMIC_RELEASE);
        if (xSemaphoreTake(writer.done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "SD writer did not stop; resources retained");
            return ESP_ERR_TIMEOUT;
        }
        writer_started = false;
        if (writer.result != ESP_OK && result == ESP_OK) {
            result = writer.result;
        }
    }
    if (writer.file != NULL) {
        esp_err_t close_result = sd_card_file_close(writer.file);
        writer.file = NULL;
        if (close_result != ESP_OK && result == ESP_OK) result = close_result;
    }
    if (writer.done != NULL) {
        vSemaphoreDelete(writer.done);
        writer.done = NULL;
    }
    if (mounted) {
        esp_err_t unmount_result = sd_card_unmount();
        if (unmount_result != ESP_OK && result == ESP_OK) {
            result = unmount_result;
        }
    }

    ESP_LOGI(TAG, "SD coexistence totals: blocks=%lu bytes=%llu result=%s",
             (unsigned long)writer.blocks_written,
             (unsigned long long)writer.bytes_written,
             esp_err_to_name(result));
    return result;
}
