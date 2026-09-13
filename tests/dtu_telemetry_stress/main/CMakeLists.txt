idf_component_register(
    SRCS "dtu_telemetry_stress_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES telemetry data_records esp_timer esp_system esp_psram heap
)
