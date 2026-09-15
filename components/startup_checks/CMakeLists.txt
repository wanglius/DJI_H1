set(startup_check_sources "startup_checks.c")

# Keep qualification-only test bodies out of normal production images.
if(CONFIG_DJI_H1_BOOT_SELF_TESTS)
    list(APPEND startup_check_sources
        "ab_protocol_test.c"
        "data_pipeline_test.c"
        "telemetry_transport_test.c"
    )
endif()

idf_component_register(
    SRCS ${startup_check_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES ab_protocol calculation clock_sync data_records drone_data
             gps_batch telemetry_transport
)
