idf_component_register(
    SRCS "gps_batch.c"
    INCLUDE_DIRS "include"
    REQUIRES data_records telemetry_transport
)
