if(NOT CONFIG_IDF_TARGET_ESP32)
    set(IMAGE_CODEC_SOURCES
        "src/image_to_jpeg.cpp"
        "src/jpeg_to_image.c"
    )
    set(IMAGE_CODEC_REQUIRES esp_jpeg esp_new_jpeg esp_image_effects esp_video)
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
