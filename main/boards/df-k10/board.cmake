if(XIAOLING_DF_K10_QEMU)
    if(NOT CONFIG_ETH_USE_OPENETH OR NOT CONFIG_IDF_TARGET_ESP32S3)
        message(FATAL_ERROR "DF-K10 QEMU requires ESP32-S3 and CONFIG_ETH_USE_OPENETH=y")
    endif()
    message(WARNING "DF-K10 QEMU TEST FIXTURE enabled: this image is not for customer hardware")
    idf_component_optional_requires(PRIVATE esp_eth)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/df-k10/df_k10_board.cc"
        PROPERTIES COMPILE_DEFINITIONS "XIAOLING_DF_K10_QEMU=1")
endif()
