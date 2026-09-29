#include "lvgl_theme_setup.h"

#include "resource/resource.h"
#include "board.h"
#include "display/display.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "ui/utils/emoji/emoji_repository.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cbin_font.h>
#include <esp_log.h>
#include <memory>
#include <vector>

namespace {

constexpr char kTag[] = "LvglThemeSetup";

void RegisterLcdThemes() {
#ifndef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    auto light_theme = std::make_unique<LvglTheme>("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
#endif

    auto dark_theme = std::make_unique<LvglTheme>("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    auto& theme_manager = LvglThemeManager::GetInstance();
#ifndef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    theme_manager.RegisterTheme("light", std::move(light_theme));
#endif
    theme_manager.RegisterTheme("dark", std::move(dark_theme));
}

}  // namespace

bool LoadResourceFonts() {
    static bool loaded = false;
    if (loaded) {
        auto& themes = LvglThemeManager::GetInstance();
        const auto ready = [](LvglTheme* theme) {
            return theme != nullptr && theme->text_font() != nullptr &&
                   theme->icon_font() != nullptr && theme->large_icon_font() != nullptr;
        };
        auto* light = themes.GetTheme("light");
        if ((light == nullptr || ready(light)) && ready(themes.GetTheme("dark"))) {
            return true;
        }
    }

    std::vector<std::string> paths;
    if (!Resource::List(ResourcePackage::Respak, "/font", paths) || paths.empty()) {
        return false;
    }
    std::sort(paths.begin(), paths.end());

    std::string text_path;
    std::string icon_path;
    std::string large_icon_path;
    for (const auto& path : paths) {
        const size_t slash = path.find_last_of('/');
        const std::string filename = path.substr(slash == std::string::npos ? 0 : slash + 1);
        if (text_path.empty() && filename == "font_puhui_common_20_4.bin") {
            text_path = path;
        } else if (filename == "font_awesome_20_4.bin") {
            icon_path = path;
        } else if (filename == "font_awesome_30_4.bin") {
            large_icon_path = path;
        }
    }

    auto& themes = LvglThemeManager::GetInstance();
    auto* light = themes.GetTheme("light");
    auto* dark = themes.GetTheme("dark");
    const auto load = [&](const std::string& path, const char* role, auto setter) {
        ResourceData asset;
        if (path.empty() ||
            !Resource::Get(ResourcePackage::Respak, path, asset)) {
            ESP_LOGE(kTag, "Required %s font is missing from respak", role);
            return false;
        }
        auto font = std::make_shared<LvglCBinFont>(
            const_cast<uint8_t*>(asset.address));
        if (font->font() == nullptr) {
            ESP_LOGE(kTag, "Failed to load %s font file %s", role, path.c_str());
            return false;
        }
        if (light != nullptr) setter(light, font);
        if (dark != nullptr) setter(dark, font);
        ESP_LOGI(kTag, "Loaded %s font from %s", role, path.c_str());
        return true;
    };

    const bool text_loaded = load(text_path, "text", [](LvglTheme* theme,
                                  const std::shared_ptr<LvglFont>& font) {
        theme->set_text_font(font);
    });
    const bool icon_loaded = load(icon_path, "icon", [](LvglTheme* theme,
                                  const std::shared_ptr<LvglFont>& font) {
        theme->set_icon_font(font);
    });
    const bool large_icon_loaded = load(large_icon_path, "large icon", [](LvglTheme* theme,
                                        const std::shared_ptr<LvglFont>& font) {
        theme->set_large_icon_font(font);
    });
    if (text_loaded && icon_loaded && large_icon_loaded) {
        loaded = true;
        return true;
    }
    return false;
}

bool LoadEmojiResources(bool lock_display) {
    auto repository = std::make_shared<EmojiRepository>();
    if (!repository->LoadFromResources()) {
        return false;
    }

    const auto install = [&repository]() {
        auto& themes = LvglThemeManager::GetInstance();
        if (auto* light = themes.GetTheme("light")) {
            light->set_emoji_repository(repository);
        }
        if (auto* dark = themes.GetTheme("dark")) {
            dark->set_emoji_repository(repository);
        }
    };
    if (lock_display) {
        auto* display = Board::GetInstance().GetDisplay();
        if (display == nullptr) {
            return false;
        }
        DisplayLockGuard lock(display);
        if (!lock.locked()) {
            return false;
        }
        install();
    } else {
        install();
    }
    return true;
}

LvglTheme* InitializeAndGetLcdTheme() {
    RegisterLcdThemes();

    // LCD 会在 Application::Initialize() 之前创建基础 UI，因此字体必须在
    // 返回主题前完成加载，不能推迟到统一资源加载阶段。
    if (!LoadResourceFonts()) {
        ESP_LOGE(kTag, "respak 中缺少必要 UI 字体，且未启用内置回退字体");
        std::abort();
    }
    if (!LoadEmojiResources(false)) {
        ESP_LOGW(kTag, "emoji 资源表不可用");
    }

#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    return LvglThemeManager::GetInstance().GetTheme("dark");
#else
    Settings settings("display", false);
    const std::string theme_name = settings.GetString("theme", "light");
    return LvglThemeManager::GetInstance().GetTheme(theme_name);
#endif
}
