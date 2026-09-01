#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

#include "data_pipeline_test.h"
#include "ab_protocol_test.h"
#include "ab_link_test.h"
#include "drone_data.h"
#include "coexistence_test.h"

static const char *TAG = "DJI_H1_COEXIST";

#define TEST_DURATION_MS 30000

void app_main(void)
{
    printf("\n============================================================\n");
    printf(" DJI_H1 - DUAL SPECTROMETER + SD COEXISTENCE TEST\n");
    printf("============================================================\n");

    ESP_ERROR_CHECK(data_pipeline_self_test());
    ESP_ERROR_CHECK(ab_protocol_self_test());
    ESP_ERROR_CHECK(ab_link_test_start());
    ESP_ERROR_CHECK(drone_data_init_fake());
    gps_record_t gps;
    ESP_ERROR_CHECK(drone_data_get_latest(&gps));
    ESP_LOGI(TAG, "Fake GPS: lat=%.7f lon=%.7f alt=%.3fm utc=%lu.%03u",
             gps.data.latitude_e7 / 10000000.0,
             gps.data.longitude_e7 / 10000000.0,
             gps.data.altitude_relative_mm / 1000.0,
             (unsigned long)gps.data.utc_seconds,
             (unsigned)gps.data.utc_milliseconds);

    esp_err_t ret = coexistence_test_run(TEST_DURATION_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Coexistence test failed: %s",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPECTROMETER + SD COEXISTENCE TEST PASSED");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
