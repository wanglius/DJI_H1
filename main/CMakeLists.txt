idf_component_register(
    SRCS "main.c" "acquisition.c"
         "../tests/sd_card_test.c"
         "../tests/data_pipeline_test.c"
         "../tests/ab_protocol_test.c"
         "../tests/ab_link_test.c"
         "../tests/coexistence_test.c"
    INCLUDE_DIRS "." "../tests"
    REQUIRES sc16is752 h1 sd_card data_records drone_data ab_protocol
             esp_timer esp_system esp_driver_uart
)
