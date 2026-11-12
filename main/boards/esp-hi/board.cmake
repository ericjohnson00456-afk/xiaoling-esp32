# The board now uses the normal LCD/ROMFS UI, including activation codes.
# Keep the obsolete AAF implementation out of the production source list.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/boards/esp-hi/emoji_display.cc"
    PROPERTIES HEADER_FILE_ONLY TRUE)
if(CONFIG_ESPTOOLPY_FLASHSIZE_4MB)
    set(ESP_HI_EMOJI_DIR "${CMAKE_CURRENT_BINARY_DIR}/esp_hi_emoji")
    file(GLOB_RECURSE ESP_HI_EMOJI_SOURCES
        "${PROJECT_DIR}/main/resource/assets/emoji/*")
    add_custom_command(OUTPUT "${ESP_HI_EMOJI_DIR}/emoji.json"
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/prepare_emoji.py"
                --source "${PROJECT_DIR}/main/resource/assets/emoji"
                --output "${ESP_HI_EMOJI_DIR}" --step 3
        DEPENDS ${ESP_HI_EMOJI_SOURCES} "${CMAKE_CURRENT_LIST_DIR}/prepare_emoji.py"
        VERBATIM)
    set(XIAOLING_RESOURCE_EMOJI_DIR "${ESP_HI_EMOJI_DIR}")
    set(XIAOLING_RESOURCE_EMOJI_DEPENDS "${ESP_HI_EMOJI_DIR}/emoji.json")
endif()
