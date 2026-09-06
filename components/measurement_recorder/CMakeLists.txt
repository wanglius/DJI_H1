idf_component_register(
    SRCS "measurement_recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES data_records calculation sd_card clock_sync freertos
)
