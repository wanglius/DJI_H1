#include "startup_checks.h"

#include "sdkconfig.h"

#if CONFIG_DJI_H1_BOOT_SELF_TESTS
#include "ab_protocol_test.h"
#include "calculation.h"
#include "clock_sync.h"
#include "data_pipeline_test.h"
#include "drone_data.h"
#include "esp_check.h"
#include "esp_log.h"
#include "telemetry_transport_test.h"

static const char *const TAG = "STARTUP_CHECKS";
#endif

esp_err_t startup_checks_run(void)
{
#if CONFIG_DJI_H1_BOOT_SELF_TESTS
    ESP_RETURN_ON_ERROR(data_pipeline_self_test(), TAG,
                        "Data-pipeline qualification check failed");
    ESP_RETURN_ON_ERROR(telemetry_transport_self_test(), TAG,
                        "Telemetry-transport qualification check failed");
    ESP_RETURN_ON_ERROR(ab_protocol_self_test(), TAG,
                        "A-B protocol qualification check failed");
    ESP_RETURN_ON_ERROR(clock_sync_self_test(), TAG,
                        "Clock-sync qualification check failed");
    ESP_RETURN_ON_ERROR(calculation_self_test(), TAG,
                        "Calculation qualification check failed");

    /* The pipeline test installs a synthetic GPS sample. It must never become
     * visible to the production recorder or telemetry services. */
    ESP_RETURN_ON_ERROR(drone_data_clear(), TAG,
                        "Could not clear qualification GPS state");
    ESP_LOGI(TAG, "All boot qualification checks passed");
#endif
    return ESP_OK;
}
