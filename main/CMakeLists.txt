idf_component_register(
    SRCS "main.c" "acquisition.c" "mission_control.c" "ab_link.c"
    INCLUDE_DIRS "."
    REQUIRES board_support sc16is752 h1 sd_card data_records drone_data ab_protocol clock_sync
             calculation measurement_recorder
             startup_checks telemetry telemetry_transport
             esp_timer esp_system esp_driver_uart esp_psram heap spi_flash
)

# Deployment credentials are never part of the source tree/index. Reconfigure
# automatically when the JSON changes; the resulting binary contains secrets.
set(M100M_CONFIG "${PROJECT_DIR}/config/m100m.local.json")
if(NOT EXISTS "${M100M_CONFIG}")
    message(FATAL_ERROR "Copy config/m100m.example.json to m100m.local.json and provision broker/topics")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${M100M_CONFIG}"
    "${PROJECT_DIR}/tools/generate_m100m_config.py")
execute_process(COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/generate_m100m_config.py"
    "${M100M_CONFIG}" "${CMAKE_CURRENT_BINARY_DIR}/m100m_config.h"
    RESULT_VARIABLE config_result)
if(NOT config_result EQUAL 0)
    message(FATAL_ERROR "Invalid M100M deployment configuration")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
