# Component requirements are expanded before sdkconfig symbols are available.
# Query the build target so ESP32 does not acquire unsupported camera deps.
idf_build_get_property(IMAGE_CODEC_TARGET IDF_TARGET)
if(NOT IMAGE_CODEC_TARGET STREQUAL "esp32")
    set(IMAGE_CODEC_SOURCES
        "src/image_to_jpeg.cpp"
        "src/jpeg_to_image.c"
    )
    set(IMAGE_CODEC_REQUIRES esp_jpeg esp_new_jpeg esp_image_effects esp_video)
    if(IMAGE_CODEC_TARGET STREQUAL "esp32p4")
        list(APPEND IMAGE_CODEC_REQUIRES esp_driver_jpeg)
    endif()
else()
    # The legacy ESP32 target does not build the camera JPEG path.
    set(IMAGE_CODEC_SOURCES "")
    set(IMAGE_CODEC_REQUIRES "")
endif()

idf_component_register(
    SRCS ${IMAGE_CODEC_SOURCES}
    INCLUDE_DIRS "include"
    REQUIRES ${IMAGE_CODEC_REQUIRES}
)
