idf_component_register(
    SRCS "main.c" "acquisition.c"
         "../tests/sd_card_test.c"
         "../tests/data_pipeline_test.c"
         "../tests/coexistence_test.c"
    INCLUDE_DIRS "." "../tests"
    REQUIRES sc16is752 h1 sd_card data_records drone_data esp_timer esp_system
)
