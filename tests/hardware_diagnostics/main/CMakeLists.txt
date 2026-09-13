idf_component_register(
    SRCS "hardware_diagnostics_main.c"
         "../../sd_card_test.c"
         "../../coexistence_test.c"
         "../../../main/acquisition.c"
    INCLUDE_DIRS "." "../.." "../../../main"
    PRIV_REQUIRES board_support h1 measurement_recorder sc16is752 sd_card
                  esp_timer esp_system heap
)
