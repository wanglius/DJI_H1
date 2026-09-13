cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "../../components")
set(IDF_TARGET "esp32s3")
set(SDKCONFIG_DEFAULTS "../diagnostic_sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(DJI_H1_SINGLE_FRAME_DIAGNOSTIC)
