# Tab5 rotates its camera frames with the ESP32-P4 PPA accelerator.
if(CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE)
    idf_component_optional_requires(PRIVATE esp_driver_ppa)
endif()
