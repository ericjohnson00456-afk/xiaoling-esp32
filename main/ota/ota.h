#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_err.h>
#include "board.h"

class Ota {
public:
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
    esp_err_t Activate();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    const ResourceInfo& GetWakeWordResource() const { return wake_word_resource_; }
    const ResourceInfo& GetToneResource() const { return tone_resource_; }
    const ResourceInfo& GetEmojiResource() const { return emoji_resource_; }
    std::string GetCheckVersionUrl();
#ifdef CONFIG_LSPLATFORM
    bool HasACtivationQRCode() { return has_activation_qrcode_; }
    const std::string& GetActivationQRCode() const {return activation_qrcode_; }
#endif // CONFIG_LSPLATFORM

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    ResourceInfo wake_word_resource_;
    ResourceInfo tone_resource_;
    ResourceInfo emoji_resource_;
    int activation_timeout_ms_ = 30000;
#ifdef CONFIG_LSPLATFORM
    bool has_activation_qrcode_ = false;
    std::string activation_qrcode_;
#endif // CONFIG_LSPLATFORM

    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
