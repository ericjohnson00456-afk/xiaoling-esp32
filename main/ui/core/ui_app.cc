#include "ui_app.h"

#include "ui/screens/home/home_presenter.h"
#include "board.h"
#include "display/display.h"
#include "ui/core/ui_manager.h"
#include "ui/core/ui_page_ids.h"
#include "ui/screens/qrcode/qrcode_presenter.h"
#include "events/ui_event_handler.h"

#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
#include "resource/assets.h"
#include "emoji_repository.h"
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
    if (dark == nullptr || dark->emoji_repository() == nullptr) {
        return false;
    }
    return dark->emoji_repository()->LoadFromAssets(Assets::GetInstance().EmojiStore());
#else
    return true;
#endif
}

bool Initialize() {
    auto& ui = UiManager::GetInstance();
    if (!ui.initialized()) {
        auto& board = Board::GetInstance();
        auto* display = board.GetDisplay();
        if (display == nullptr) {
            ESP_LOGE("UiApp", "Board did not provide a display");
            return false;
        }
        ui.Initialize(display);
    }

    if (UiEventHandler::Initialize() != ESP_OK) {
        return false;
    }

#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    // Home 页面创建前先加载表情动画定义，避免页面打开后再解析 JSON。
    auto emoji_repository = std::make_shared<EmojiRepository>();
    if (emoji_repository->LoadFromAssets(Assets::GetInstance().EmojiStore())) {
        auto& themes = LvglThemeManager::GetInstance();
        if (auto* light = themes.GetTheme("light")) {
            light->set_emoji_repository(emoji_repository);
        }
        if (auto* dark = themes.GetTheme("dark")) {
            dark->set_emoji_repository(emoji_repository);
        }
    } else {
        ESP_LOGW("UiApp", "表情动画资源加载失败，Home 页面将使用无表情回退");
    }
#endif

    if (ui.RegisterScreen(&HomePresenter::Screen()) != 0 ||
        ui.RegisterScreen(&QrCodePresenter::Screen()) != 0 ||
        ui.SetDefaultScreen(UI_PAGE_HOME) != 0) {
        return false;
    }
    return true;
}

}  // UiApp 命名空间
