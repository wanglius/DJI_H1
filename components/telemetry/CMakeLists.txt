idf_component_register(
    SRCS "telemetry.c"
    INCLUDE_DIRS "include"
    REQUIRES data_records gps_batch telemetry_transport freertos esp_driver_uart
             esp_timer heap
)
