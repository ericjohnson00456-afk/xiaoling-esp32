# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.1.0")

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE XIAOLING_COMMIT_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT XIAOLING_COMMIT_ID)
    set(XIAOLING_COMMIT_ID "unknown")
endif()
add_compile_definitions(XIAOLING_COMMIT_ID=\"${XIAOLING_COMMIT_ID}\")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

message(STATUS "Using sdkconfig defaults for LSPlatform")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.lsplatform")

# Some ESP32-S3 boards use Quad PSRAM while the target-wide defaults use
# Octal PSRAM. Load board-specific defaults before project() so a clean
# configuration selects the hardware settings for the requested board.
if(BOARD_TYPE)
    set(BOARD_SDKCONFIG_DEFAULTS "main/boards/${BOARD_TYPE}/sdkconfig.defaults")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${BOARD_SDKCONFIG_DEFAULTS}")
        list(APPEND SDKCONFIG_DEFAULTS "${BOARD_SDKCONFIG_DEFAULTS}")
        message(STATUS "Using board sdkconfig defaults: ${BOARD_SDKCONFIG_DEFAULTS}")
    endif()
endif()

set(XIAOLING_VER "3.0.0")
set(PROJECT_VER ${XIAOLING_VER})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# esp-ml307 3.3.2 performs blocking TLS/TCP writes and retries WANT_WRITE
# forever. Replace only its ESP transport implementations from the project so
# realtime AEC WebSocket writes have a deadline, while keeping the registry
# component itself immutable and upgradeable.
idf_component_get_property(ESP_ML307_LIB 78__esp-ml307 COMPONENT_LIB)
get_target_property(ESP_ML307_SOURCES ${ESP_ML307_LIB} SOURCES)
list(FILTER ESP_ML307_SOURCES EXCLUDE REGEX "/src/esp/esp_(ssl|tcp)\\.cc$")
list(APPEND ESP_ML307_SOURCES
     "${CMAKE_CURRENT_LIST_DIR}/main/transport_patch/esp_ssl.cc"
     "${CMAKE_CURRENT_LIST_DIR}/main/transport_patch/esp_tcp.cc")
set_property(TARGET ${ESP_ML307_LIB} PROPERTY SOURCES "${ESP_ML307_SOURCES}")

# esp_asrc 1.0.1 的内存分配日志仍用 %d 打印 uint32_t。
# 保留格式告警，但避免 ESP-IDF 5.4.4 的新工具链将供应商代码告警升级为构建错误。
idf_component_get_property(ESP_ASRC_LIB espressif__esp_asrc COMPONENT_LIB)
target_compile_options(${ESP_ASRC_LIB} PRIVATE -Wno-error=format)
