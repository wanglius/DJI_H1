idf_component_register(
    SRCS "main.c" "acquisition.c" "mission_control.c" "ab_link.c"
    INCLUDE_DIRS "."
    REQUIRES board_support sc16is752 h1 sd_card data_records drone_data ab_protocol clock_sync
             calculation measurement_recorder
             startup_checks telemetry telemetry_transport boot_health
             esp_timer esp_system esp_driver_uart esp_psram heap spi_flash
)
