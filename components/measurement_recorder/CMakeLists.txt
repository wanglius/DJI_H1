idf_component_register(
    SRCS "measurement_recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES data_records calculation sd_card clock_sync telemetry freertos
             esp_app_format esp_hw_support heap
)
