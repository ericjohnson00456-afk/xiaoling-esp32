# Resource packaging policy is derived from the selected hardware configuration.
# Board files may provide exceptional content (for example ESP-HI's reduced
# emoji set), but they do not choose the memory budget or the default font.
function(xiaoling_resource_options output_dir args_var depends_var)
    set(resource_args)
    set(resource_depends)

    if(XIAOLING_RESOURCE_EMOJI_DIR)
        list(APPEND resource_args --emoji-dir "${XIAOLING_RESOURCE_EMOJI_DIR}")
        list(APPEND resource_depends ${XIAOLING_RESOURCE_EMOJI_DEPENDS})
    endif()

    # The split respak font is selected from the effective hardware budget:
    # without PSRAM, leave more internal RAM available; with PSRAM but only
    # 4/8 MB Flash, use the smaller common font; larger configurations keep
    # the normal resource set. These are sdkconfig facts, so the policy also
    # applies to new boards without a board.cmake file.
    set(resource_font)
    set(resource_budget "default")
    set(resource_flash "${CONFIG_ESPTOOLPY_FLASHSIZE}")
    if(NOT resource_flash)
        set(resource_flash "unknown")
    endif()
    set(resource_psram "none")
    if(CONFIG_SPIRAM_MODE_OCT)
        set(resource_psram "octal")
    elseif(CONFIG_SPIRAM_MODE_HEX)
        set(resource_psram "hex")
    elseif(CONFIG_SPIRAM_MODE_QUAD)
        set(resource_psram "quad")
    elseif(CONFIG_SPIRAM)
        set(resource_psram "enabled")
    endif()
    if(NOT CONFIG_SPIRAM)
        set(resource_font "font_puhui_14_1")
        set(resource_budget "no-psram")
    elseif(CONFIG_ESPTOOLPY_FLASHSIZE_4MB OR CONFIG_ESPTOOLPY_FLASHSIZE_8MB)
        set(resource_font "font_puhui_20_4")
        set(resource_budget "psram-4/8mb-flash")
    endif()

    if(resource_font)
        set(font_source "${XIAOZHI_FONTS_PATH}/src/${resource_font}.c")
        # The runtime font role uses this canonical resource name regardless
        # of which source variant was selected by the budget policy.
        set(font_file "${output_dir}/font_puhui_common_20_4.bin")
        add_custom_command(OUTPUT "${font_file}"
            COMMAND "${PYTHON}" "${PROJECT_DIR}/scripts/convert_lvgl_c_font.py"
                    "${font_source}" "${font_file}"
            DEPENDS "${font_source}" "${PROJECT_DIR}/scripts/convert_lvgl_c_font.py"
            VERBATIM)
        list(APPEND resource_args --font-file "${font_file}")
        list(APPEND resource_depends "${font_file}")
    endif()

    if(XIAOLING_RESOURCE_EMOJI_DIR)
        message(STATUS "Resource budget: ${resource_budget} (flash=${resource_flash}, psram=${resource_psram}); emoji source: ${XIAOLING_RESOURCE_EMOJI_DIR}")
    else()
        message(STATUS "Resource budget: ${resource_budget} (flash=${resource_flash}, psram=${resource_psram})")
    endif()
    set(${args_var} "${resource_args}" PARENT_SCOPE)
    set(${depends_var} "${resource_depends}" PARENT_SCOPE)
endfunction()
