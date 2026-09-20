#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class AssetStore;

// Firmware-owned localized audio stored in the respak ROMFS package.
// Activation assets use the reserved 300-series IDs so they never collide
// with OTA-managed tone.bin IDs.
class LocalizedAudio final {
public:
    static constexpr uint16_t kActivationGuideId = 300;
    static constexpr uint16_t kActivationDigitFirstId = 301;
    static constexpr uint16_t kActivationDigitLastId = 310;

    static LocalizedAudio& GetInstance();

    bool Initialize(AssetStore& assets);
    bool GetData(const std::string& name, const uint8_t** data, size_t* size) const;
    bool GetActivationData(uint16_t id, const uint8_t** data, size_t* size) const;

private:
    LocalizedAudio() = default;
    LocalizedAudio(const LocalizedAudio&) = delete;
    LocalizedAudio& operator=(const LocalizedAudio&) = delete;

    bool GetPathData(const std::string& path, const uint8_t** data, size_t* size) const;

    AssetStore* assets_ = nullptr;
};
