# Optional matrix build settings; loaded after project() creates IDF targets.
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE "-Wl,--strip-debug")
# Concurrent matrix profiles otherwise each start a full-size Ninja pool.
# Keep their aggregate CPU demand bounded without changing compiler flags.
set(XIAOLING_MATRIX_COMPILE_JOBS 2 CACHE STRING "Compile workers per matrix profile")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS
    "xiaoling_matrix_compile=${XIAOLING_MATRIX_COMPILE_JOBS}"
    "xiaoling_matrix_link=1")
idf_build_get_property(MATRIX_COMPONENTS BUILD_COMPONENTS)
foreach(MATRIX_COMPONENT IN LISTS MATRIX_COMPONENTS)
    idf_component_get_property(MATRIX_COMPONENT_LIB ${MATRIX_COMPONENT} COMPONENT_LIB)
    if(TARGET ${MATRIX_COMPONENT_LIB})
        get_target_property(MATRIX_COMPONENT_TYPE ${MATRIX_COMPONENT_LIB} TYPE)
        if(NOT MATRIX_COMPONENT_TYPE STREQUAL "INTERFACE_LIBRARY")
            set_target_properties(${MATRIX_COMPONENT_LIB} PROPERTIES
                JOB_POOL_COMPILE xiaoling_matrix_compile JOB_POOL_LINK xiaoling_matrix_link)
        endif()
    endif()
endforeach()
set_target_properties(${CMAKE_PROJECT_NAME}.elf PROPERTIES
    JOB_POOL_COMPILE xiaoling_matrix_compile JOB_POOL_LINK xiaoling_matrix_link)
