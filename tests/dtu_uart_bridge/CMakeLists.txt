cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "../../components/board_support")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Keep this diagnostic firmware independent from the production application.
idf_build_set_property(MINIMAL_BUILD ON)
project(DJI_H1_DTU_UART_BRIDGE)
