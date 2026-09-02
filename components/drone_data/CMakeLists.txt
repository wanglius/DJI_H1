idf_component_register(
    SRCS "drone_data.c"
    INCLUDE_DIRS "include"
    REQUIRES data_records ab_protocol freertos
)
