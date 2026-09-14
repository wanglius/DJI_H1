idf_component_register(
    SRCS "dtu_telemetry_stress_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board_support telemetry data_records esp_timer esp_system
                  esp_psram heap
)

if(DEFINED DTU_STRESS_MISSION_ID)
    target_compile_definitions(
        ${COMPONENT_LIB} PRIVATE
        DTU_STRESS_MISSION_ID=${DTU_STRESS_MISSION_ID}
    )
endif()

if(DEFINED DTU_STRESS_FIRE_AND_FORGET)
    target_compile_definitions(
        ${COMPONENT_LIB} PRIVATE
        DTU_STRESS_FIRE_AND_FORGET=${DTU_STRESS_FIRE_AND_FORGET}
    )
endif()
