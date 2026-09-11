#include <stdio.h>
#include "esp_err.h"
#include "esp_mac.h"
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

void app_main(void)
{
    printf("\nDJI_H1 - A-BOARD CONTROLLED DUAL ACQUISITION\n");
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
