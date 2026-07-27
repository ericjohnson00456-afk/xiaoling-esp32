# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "2.1.1")

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
set(XIAOLING_EXTRA_SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS}")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.lsplatform;main/boards/common/sdkconfig.defaults")

# Some ESP32-S3 boards use Quad PSRAM while the target-wide defaults use
# Octal PSRAM. Load board-specific defaults before project() so a clean
# configuration selects the hardware settings for the requested board.
include("${CMAKE_CURRENT_LIST_DIR}/main/boards/common/board_config.cmake")
if(BOARD_DIR)
    xiaoling_resolve_board_dir("${BOARD_DIR}" BOARD_DIR)
elseif(BOARD_TYPE)
    xiaoling_resolve_board_dir("${BOARD_TYPE}" BOARD_DIR)
endif()
if(BOARD_DIR)
    set(BOARD_SDKCONFIG_DEFAULTS "main/boards/${BOARD_DIR}/sdkconfig.defaults")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${BOARD_SDKCONFIG_DEFAULTS}")
        list(APPEND SDKCONFIG_DEFAULTS "${BOARD_SDKCONFIG_DEFAULTS}")
        message(STATUS "Using board sdkconfig defaults: ${BOARD_SDKCONFIG_DEFAULTS}")
    endif()
endif()
list(APPEND SDKCONFIG_DEFAULTS ${XIAOLING_EXTRA_SDKCONFIG_DEFAULTS})
list(REMOVE_DUPLICATES SDKCONFIG_DEFAULTS)

set(XIAOLING_VER ${PROJECT_VER})
set(XIAOLING_PROJECT_NAME "xiaoling_v${XIAOLING_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${XIAOLING_PROJECT_NAME})

# cpp_bus_driver 1.1.0 still declares only the legacy driver umbrella.
# Add the IDF 6 split dependencies without editing managed component sources.
if(IDF_TARGET STREQUAL "esp32p4")
    idf_component_get_property(XIAOLING_CPP_BUS_LIB llgok__cpp_bus_driver COMPONENT_LIB)
    target_link_libraries(${XIAOLING_CPP_BUS_LIB} PUBLIC
        idf::esp_driver_gpio idf::esp_driver_i2c idf::esp_driver_spi
        idf::esp_driver_uart idf::esp_driver_i2s idf::esp_driver_ledc
        idf::esp_driver_sdmmc idf::sdmmc)
    target_compile_options(${XIAOLING_CPP_BUS_LIB} PUBLIC
        "-include${CMAKE_CURRENT_LIST_DIR}/main/boards/lilygo/t-display-p4/idf6_compat.h")
endif()

if(XIAOLING_MATRIX_STRIP_DEBUG)
    include("${CMAKE_CURRENT_LIST_DIR}/main/boards/common/qemu/matrix.cmake")
endif()

# 普通 idf.py build 同时生成供量产工具使用的全量烧录包，以及后台 OTA
# 上传所需的版本/MD5 信息。merge-bin 会读取 flash_args，因此包含 bootloader、
# 分区表、应用和所有已配置了镜像的资源分区。
set(XIAOLING_OTA_INFO
    "${CMAKE_BINARY_DIR}/xiaoling_v${XIAOLING_VER}_ota_info.txt")
add_custom_target(xiaoling_ota_info
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/scripts/generate_ota_info.py"
            --version "${XIAOLING_VER}"
            --app "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin"
            --output "${XIAOLING_OTA_INFO}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/scripts/generate_ota_info.py"
    BYPRODUCTS "${XIAOLING_OTA_INFO}"
    COMMENT "Generating ${XIAOLING_OTA_INFO}"
    VERBATIM)
add_dependencies(xiaoling_ota_info gen_project_binary)

if(TARGET generated_split_resources)
    add_dependencies(merge-bin generated_split_resources)
endif()
if(TARGET generated_default_assets)
    add_dependencies(merge-bin generated_default_assets)
endif()

# 保留 ESP-IDF 原始产物名供 flash/merge-bin 等内置目标使用，同时额外生成带
# 版型前缀的发布文件，并生成完整 zip 和仅含应用、OTA 信息、合并固件的
# _test.zip。BOARD_NAME 在发布
# 构建中代表具体版型；普通 idf.py build 未传入时，脚本会从编译命令中读取。
add_custom_target(xiaoling_release_artifacts ALL
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/scripts/package_release_artifacts.py"
            --build-dir "${CMAKE_BINARY_DIR}"
            --project-name "${PROJECT_NAME}"
            --board "${BOARD_NAME}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/scripts/package_release_artifacts.py"
    COMMENT "Packaging board-specific Xiaoling release artifacts"
    VERBATIM)
add_dependencies(xiaoling_release_artifacts merge-bin xiaoling_ota_info)

# esp_asrc 1.0.1 的内存分配日志仍用 %d 打印 uint32_t。
# 保留格式告警，但避免 ESP-IDF 5.4.4 的新工具链将供应商代码告警升级为构建错误。
idf_component_get_property(ESP_ASRC_LIB espressif__esp_asrc COMPONENT_LIB)
target_compile_options(${ESP_ASRC_LIB} PRIVATE -Wno-error=format)
