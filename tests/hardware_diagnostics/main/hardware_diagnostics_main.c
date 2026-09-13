#include "coexistence_test.h"
#include "esp_err.h"
#include "sd_card_test.h"
#include "sdkconfig.h"

void app_main(void)
{
#if CONFIG_DJI_H1_DIAGNOSTIC_COEXISTENCE
    ESP_ERROR_CHECK(coexistence_test_run(CONFIG_DJI_H1_DIAGNOSTIC_DURATION_MS));
#else
    ESP_ERROR_CHECK(sd_card_connection_test());
#endif
}
