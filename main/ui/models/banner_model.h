#pragma once

#include "observable_model.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>

/*
 * Banner 数据模型：订阅 lschat 的设置推送（action=result, sub=setting），
 * 从中解析 data.theme.frontend.banner = { interval_ms, resources:[{text}, ...] }。
 *
 * 只做协议解析与数据持有，不认识 UI；解析成功后通过回调发布，
 * 由 application（组合根）把回调接到 UiManager。
 */
struct BannerModelState {
    std::vector<std::string> texts;
    uint32_t interval_ms = 0;
};

class BannerModel : public ObservableModel<BannerModelState> {
public:
    /* 提供当前唤醒词名称，用于替换横幅文本中的 #唤醒词# 占位符；返回空串则不替换 */
    using WakeWordProvider = std::function<std::string()>;

    static BannerModel& GetInstance() {
        static BannerModel instance;
        return instance;
    }

    void SetWakeWordProvider(WakeWordProvider provider) {
        std::lock_guard<std::mutex> lock(provider_mutex_);
        wake_word_provider_ = std::move(provider);
    }

    /* 处理一条 result/setting 推送的 data 节点。非 banner 内容、缺字段时静默忽略。
       可在任意任务上下文调用（只做解析与回调，不触碰 LVGL）。 */
    void HandleSettingMessage(const cJSON* data);

private:
    BannerModel() = default;
    ~BannerModel() = default;
    BannerModel(const BannerModel&) = delete;
    BannerModel& operator=(const BannerModel&) = delete;

    std::string SubstituteWakeWord(const std::string& text) const;

    mutable std::mutex provider_mutex_;
    WakeWordProvider wake_word_provider_;
};
