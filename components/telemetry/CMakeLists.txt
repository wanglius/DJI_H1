idf_component_register(
    SRCS "telemetry.c" "dtu_boot_check.c"
    INCLUDE_DIRS "include"
    REQUIRES data_records gps_batch telemetry_transport freertos esp_driver_uart
             esp_timer heap boot_health
)

# The provisioning JSON remains the source of truth. Never embed MQAUTH/password.
# Explicit override supports other deployments without editing source files.
set(DJI_DTU_BOOT_PROFILE "" CACHE FILEPATH "YY-M200 provisioning JSON for read-only boot checks")
if(NOT DJI_DTU_BOOT_PROFILE)
    set(DJI_DTU_BOOT_PROFILE "${CMAKE_CURRENT_LIST_DIR}/../../tests/dtu_uart_bridge/dtu_mqtt_config.local.json")
    if(NOT EXISTS "${DJI_DTU_BOOT_PROFILE}")
        set(DJI_DTU_BOOT_PROFILE "${CMAKE_CURRENT_LIST_DIR}/../../tests/dtu_uart_bridge/dtu_mqtt_config.example.json")
        message(WARNING "DTU boot check uses example profile; provide local provisioning JSON before deployment")
    endif()
endif()
set(profile_generator "${CMAKE_CURRENT_LIST_DIR}/../../tools/generate_dtu_boot_profile.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${DJI_DTU_BOOT_PROFILE}" "${profile_generator}"
    "${CMAKE_CURRENT_LIST_DIR}/../../tests/dtu_uart_bridge/configure_dtu_mqtt.py")
execute_process(COMMAND "${PYTHON}" "${profile_generator}" "${DJI_DTU_BOOT_PROFILE}"
    "${CMAKE_CURRENT_BINARY_DIR}/dtu_boot_profile.h" RESULT_VARIABLE profile_result)
if(NOT profile_result EQUAL 0)
    message(FATAL_ERROR "Cannot generate non-secret DTU boot profile")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
