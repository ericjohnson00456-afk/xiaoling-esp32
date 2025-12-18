# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.5")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

if(NOT "$ENV{BUILD_XIAOZHI}" STREQUAL "1")
    message(STATUS "Using sdkconfig defaults for LSPlatform")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.lsplatform")
    set(XIAOLING_VER "1.2.5")
    set(PROJECT_VER ${XIAOLING_VER})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

