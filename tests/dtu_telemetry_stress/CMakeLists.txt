cmake_minimum_required(VERSION 3.16)

# Reuse the production serializer, fragmenter, retention pool, UART owner, and
# ACK handling while keeping this diagnostic app independent from main/.
set(EXTRA_COMPONENT_DIRS "../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(DJI_H1_DTU_TELEMETRY_STRESS)
