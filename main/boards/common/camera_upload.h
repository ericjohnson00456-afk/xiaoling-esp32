#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Upload a complete JPEG without copying the full multipart body into RAM.
std::string UploadCameraJpeg(const std::string& url, const std::string& token,
                             const std::string& question,
                             const uint8_t* jpeg, size_t size);
