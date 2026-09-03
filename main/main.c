#include <stdio.h>
#include "esp_err.h"
#include "data_pipeline_test.h"
#include "ab_protocol_test.h"
#include "ab_link.h"
#include "mission_control.h"
#include "drone_data.h"

void app_main(void)
{
    printf("\nDJI_H1 - A-BOARD CONTROLLED DUAL ACQUISITION\n");
    ESP_ERROR_CHECK(data_pipeline_self_test());
    ESP_ERROR_CHECK(ab_protocol_self_test());
    ESP_ERROR_CHECK(drone_data_clear()); /* Do not publish self-test GPS as real. */
    ESP_ERROR_CHECK(mission_control_init());
    ESP_ERROR_CHECK(ab_link_start());
    /* Persistent services own the app: no boot acquisition or scratch writes. */
}
