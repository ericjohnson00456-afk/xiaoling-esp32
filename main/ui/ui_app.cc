#include "ui_app.h"

#include "home_presenter.h"
#include "display.h"
#include "ui_manager.h"
#include "ui_page_ids.h"
#include "wifi_model.h"

#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
#include "assets.h"
#include "emoji_model.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <memory>
#endif

namespace UiApp {

bool ReloadEmojiResources() {
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    auto* display = UiManager::GetInstance().display();
    if (display == nullptr) {
        return false;
    }
    DisplayLockGuard lock(display);
    auto& themes = LvglThemeManager::GetInstance();
    auto* dark = themes.GetTheme("dark");
    if (dark == nullptr || dark->emoji_model() == nullptr) {
        return false;
    }
    return dark->emoji_model()->LoadFromAssets(Assets::GetInstance());
#else
    return true;
#endif
}

bool Initialize() {
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    // Home 页面创建前先加载表情动画定义，避免页面打开后再解析 JSON。
    auto emoji_model = std::make_shared<EmojiModel>();
    if (emoji_model->LoadFromAssets(Assets::GetInstance())) {
        auto& themes = LvglThemeManager::GetInstance();
        if (auto* light = themes.GetTheme("light")) {
            light->set_emoji_model(emoji_model);
        }
        if (auto* dark = themes.GetTheme("dark")) {
            dark->set_emoji_model(emoji_model);
        }
    } else {
        ESP_LOGW("UiApp", "表情动画资源加载失败，Home 页面将使用无表情回退");
    }
#endif

    auto& ui = UiManager::GetInstance();
    if (ui.RegisterScreen(&HomePresenter::Screen()) != 0 ||
        ui.SetDefaultScreen(UI_PAGE_HOME) != 0) {
        return false;
    }
    WifiModel::GetInstance().Refresh();
    return true;
}

}  // UiApp 命名空间
