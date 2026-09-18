#pragma once

#include "observable_model.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <cJSON.h>

class LvglImage;

enum class QrCodeContentSource {
    kRoleConfig,
    kCustom,
};

struct QrCodeModelState {
    std::string role_config_url;
    std::shared_ptr<LvglImage> role_config_image;
    std::shared_ptr<LvglImage> custom_image;
    std::string custom_message;
    QrCodeContentSource content_source = QrCodeContentSource::kRoleConfig;
    bool loading = false;
    uint32_t generation = 0;
};

// 保存角色配置二维码缓存及当前二维码页内容。模型不触碰显示对象。
class QrCodeModel final : public ObservableModel<QrCodeModelState> {
public:
    static QrCodeModel& GetInstance();

    void HandleSettingMessage(const cJSON* data);
    void SetRoleConfigUrl(std::string url);
    void ClearRoleConfigUrl();
    void SelectRoleConfig();
    void SetCustomQrCode(std::unique_ptr<LvglImage> image, std::string message);

private:
    QrCodeModel() = default;

    static void FetchTask(void* arg);
    void CompleteFetch(const std::string& url, uint32_t generation,
                       std::unique_ptr<LvglImage> image);

    std::mutex cache_mutex_;
    uint32_t fetch_generation_ = 0;
};
