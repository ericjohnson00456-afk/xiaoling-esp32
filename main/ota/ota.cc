#include "ota.h"

#include "settings.h"
#include "system/system_info.h"
#include "ui/i18n/lang_config.h"
#include "xiaoling/conn/xiaoling_config.h"

#include <esp_app_desc.h>
#include <esp_log.h>

#define TAG "Ota"

Ota::Ota() {
    current_version_ = esp_app_get_description()->version;
    firmware_version_ = current_version_;
}

Ota::~Ota() = default;

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = std::string("https://") + XiaolingConfig::GetHost() + "/v1/ota/packages";
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (network == nullptr) {
        return nullptr;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return nullptr;
    }
    const auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");
    return http;
}
