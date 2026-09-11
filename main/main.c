#include <stdio.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "data_pipeline_test.h"
#include "telemetry_transport_test.h"
#include "ab_protocol_test.h"
#include "ab_link.h"
#include "mission_control.h"
#include "drone_data.h"
#include "clock_sync.h"
#include "board_config.h"
#include "telemetry.h"
#include "telemetry_transport.h"

static const char *TAG = "DJI_H1";

static esp_err_t verify_board_psram(void)
{
#if CONFIG_SPIRAM
    if (!esp_psram_is_initialized()) return ESP_ERR_NOT_FOUND;

    size_t detected = esp_psram_get_size();
    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG,
             "PSRAM initialized: detected=%u bytes (%u MiB), "
             "heap_total=%u bytes, heap_free=%u bytes",
             (unsigned)detected, (unsigned)(detected / (1024U * 1024U)),
             (unsigned)heap_total, (unsigned)heap_free);
    if (detected != DJI_BOARD_EXPECTED_PSRAM_BYTES) {
        ESP_LOGE(TAG, "Expected %u PSRAM bytes for N16R8, detected %u",
                 (unsigned)DJI_BOARD_EXPECTED_PSRAM_BYTES,
                 (unsigned)detected);
        return ESP_ERR_INVALID_SIZE;
    }
    if (heap_total == 0 || heap_free == 0) return ESP_ERR_NO_MEM;
    return ESP_OK;
#else
    ESP_LOGE(TAG, "Production N16R8 build has PSRAM disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void app_main(void)
{
    printf("\nDJI_H1 - A-BOARD CONTROLLED DUAL ACQUISITION\n");
    ESP_ERROR_CHECK(verify_board_psram());
    ESP_ERROR_CHECK(data_pipeline_self_test());
    ESP_ERROR_CHECK(telemetry_transport_self_test());
    ESP_ERROR_CHECK(ab_protocol_self_test());
    ESP_ERROR_CHECK(clock_sync_self_test());
    ESP_ERROR_CHECK(drone_data_clear()); /* Do not publish self-test GPS as real. */
    ESP_ERROR_CHECK(clock_sync_init());
    uint8_t factory_mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(factory_mac));
    uint64_t source_id = 0;
    for (size_t i = 0; i < sizeof(factory_mac); i++) {
        source_id = (source_id << 8) | factory_mac[i];
    }
    const telemetry_config_t telemetry = {
        .uart_port = DJI_DTU_UART_PORT,
        .tx_gpio = DJI_DTU_UART_TX_GPIO,
        .rx_gpio = DJI_DTU_UART_RX_GPIO,
        .baud_rate = DJI_DTU_UART_BAUD_RATE,
        .fragment_gap_ms = DJI_DTU_FRAGMENT_GAP_MS,
        .gps_min_interval_ms = DJI_DTU_GPS_MIN_INTERVAL_MS,
        .source_id = source_id,
        .ack_timeout_ms = DJI_DTU_ACK_TIMEOUT_MS,
        .max_retries = DJI_DTU_MAX_RETRIES,
    };
    ESP_ERROR_CHECK(telemetry_start(&telemetry));
    ESP_ERROR_CHECK(mission_control_init());
    ESP_ERROR_CHECK(ab_link_start());
    /* Persistent services own the app: no boot acquisition or scratch writes. */
}
