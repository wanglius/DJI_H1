#include "sd_card_test.h"

#include <string.h>

#include "esp_log.h"
#include "sd_card.h"

static const char *TAG = "SD_CARD_TEST";

esp_err_t sd_card_connection_test(void)
{
    static const char expected[] =
        "DJI_H1 LilyGO T8-S3 TF connection test: PASS\n";
    char received[sizeof(expected)] = {0};
    sd_card_file_t *file = NULL;
    size_t transferred = 0;
    esp_err_t result;

    const sd_card_config_t config =
        SD_CARD_LILYGO_T8_S3_DEFAULT_CONFIG();
    ESP_LOGI(TAG, "TF wiring: CS=%d MOSI=%d SCLK=%d MISO=%d",
             config.pin_cs, config.pin_mosi,
             config.pin_sclk, config.pin_miso);

    result = sd_card_mount(&config);
    if (result != ESP_OK) return result;
    result = sd_card_print_info(stdout);
    if (result != ESP_OK) goto cleanup;

    /* FatFs long-file-name support is disabled, so use an 8.3 name. */
    result = sd_card_file_open("H1TEST.TXT", "wb", &file);
    if (result != ESP_OK) goto cleanup;
    result = sd_card_file_write(
        file, expected, sizeof(expected) - 1, &transferred);
    esp_err_t close_result = sd_card_file_close(file);
    file = NULL;
    if (result != ESP_OK || close_result != ESP_OK) {
        if (result == ESP_OK) result = close_result;
        goto cleanup;
    }
    if (transferred != sizeof(expected) - 1) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Wrote %u bytes to H1TEST.TXT", (unsigned)transferred);

    result = sd_card_file_open("H1TEST.TXT", "rb", &file);
    if (result != ESP_OK) goto cleanup;
    result = sd_card_file_read(
        file, received, sizeof(received) - 1, &transferred);
    close_result = sd_card_file_close(file);
    file = NULL;
    if (result != ESP_OK || close_result != ESP_OK) {
        if (result == ESP_OK) result = close_result;
        goto cleanup;
    }
    if (transferred != sizeof(expected) - 1 ||
        memcmp(received, expected, sizeof(expected) - 1) != 0) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ESP_LOGI(TAG, "TF CARD COMPONENT TEST PASSED");

cleanup:
    if (file != NULL) {
        esp_err_t close_result = sd_card_file_close(file);
        if (result == ESP_OK) result = close_result;
    }
    esp_err_t unmount_result = sd_card_unmount();
    if (result == ESP_OK) result = unmount_result;
    return result;
}
