#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <esp_err.h>
#include "board.h"

class Ota {
public:
    enum class Resource { Tone, Emoji, WakeWord };

    struct ResourceInfo {
        bool available = false;
        size_t size = 0;
        std::array<uint8_t, 16> md5{};
        std::string url;
        std::string wake_word;
    };

    Ota();
    ~Ota();

    esp_err_t CheckVersion();
    // Fetches the independently managed wake-word, tone and emoji packages.
    esp_err_t CheckResources();
    bool VerifyResource(Resource resource, size_t expected_size,
                        const std::array<uint8_t, 16>& expected_md5);
    bool UpgradeResource(Resource resource, const std::string& url, size_t expected_size,
                         const std::array<uint8_t, 16>& expected_md5,
                         std::function<void(int progress, size_t speed)> callback);
    bool HasNewVersion() { return has_new_version_; }
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    size_t GetFirmwareSize() const { return firmware_size_; }
    const std::string& GetFirmwareDescription() const { return firmware_description_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const ResourceInfo& GetWakeWordResource() const { return wake_word_resource_; }
    const ResourceInfo& GetToneResource() const { return tone_resource_; }
    const ResourceInfo& GetEmojiResource() const { return emoji_resource_; }

private:
    bool has_new_version_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    size_t firmware_size_ = 0;
    std::string firmware_description_;
    ResourceInfo wake_word_resource_;
    ResourceInfo tone_resource_;
    ResourceInfo emoji_resource_;

    std::string GetCheckVersionUrl();
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
