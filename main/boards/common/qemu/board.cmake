# All hardware board sources still compile and link. Only calls to the board
# factory are redirected in this explicitly requested simulation build.
if(NOT CONFIG_ETH_USE_OPENETH)
    message(FATAL_ERROR "XIAOLING_QEMU requires CONFIG_ETH_USE_OPENETH=y")
endif()
if(XIAOLING_DF_K10_QEMU)
    message(FATAL_ERROR "Use either XIAOLING_QEMU or the legacy DF-K10 fixture")
endif()
message(WARNING "QEMU profile test: hardware peripherals and network access are virtualized")
idf_component_optional_requires(PRIVATE esp_eth)
if(CONFIG_IDF_TARGET_ESP32C5 OR CONFIG_IDF_TARGET_ESP32C6)
    # IDF 5.4 predates these QEMU machines; supply their virtual EMAC mapping
    # without editing the installed SDK or changing any hardware build.
    idf_component_get_property(QEMU_ETH_LIB esp_eth COMPONENT_LIB)
    target_compile_options(${QEMU_ETH_LIB} PRIVATE
        "-include" "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/qemu/openeth_compat.h")
endif()
set(QEMU_BOARD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/qemu/board.cc")
target_sources(${COMPONENT_LIB} PRIVATE "${QEMU_BOARD_SOURCE}")
if(NOT XIAOLING_QEMU_ID)
    set(XIAOLING_QEMU_ID 1)
endif()
set_source_files_properties("${QEMU_BOARD_SOURCE}" PROPERTIES
    COMPILE_DEFINITIONS "XIAOLING_QEMU_ID=${XIAOLING_QEMU_ID}")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=_Z12create_boardv")
if(CONFIG_IDF_TARGET_ESP32P4)
    # There is no hosted Wi-Fi coprocessor in this fixture. Virtualize its MAC
    # query as well; network traffic still uses real lwIP/OpenCores sockets.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_get_mac")
    if(CONFIG_SPIRAM)
        target_sources(${COMPONENT_LIB} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/qemu/p4_psram.c")
        target_link_libraries(${COMPONENT_LIB} INTERFACE
            "-Wl,--wrap=esp_psram_impl_enable"
            "-Wl,--wrap=esp_psram_impl_get_physical_size"
            "-Wl,--wrap=esp_psram_impl_get_available_size")
    endif()
    # The P4 QEMU ROM's strtod implementation traps while cJSON parses
    # numeric fields in cloud messages. Keep this compatibility shim in the
    # explicitly test-only link path.
    target_sources(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/qemu/strtod_compat.c")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=strtod")
endif()
