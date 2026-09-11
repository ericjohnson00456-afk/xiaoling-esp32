# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.1.0")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

message(STATUS "Using sdkconfig defaults for LSPlatform")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.lsplatform")
set(XIAOLING_VER "3.0.0")
set(PROJECT_VER ${XIAOLING_VER})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# esp_asrc 1.0.1 的内存分配日志仍用 %d 打印 uint32_t。
# 保留格式告警，但避免 ESP-IDF 5.4.4 的新工具链将供应商代码告警升级为构建错误。
idf_component_get_property(ESP_ASRC_LIB espressif__esp_asrc COMPONENT_LIB)
target_compile_options(${ESP_ASRC_LIB} PRIVATE -Wno-error=format)
