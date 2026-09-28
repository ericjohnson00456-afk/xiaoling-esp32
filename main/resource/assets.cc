#include "assets.h"
#include "application.h"
#include "board.h"
#include "display/display.h"
#include "player/resource/localized_audio.h"
#include "player/resource/tone_model.h"
#include "lvgl_theme.h"
#include "audio/wake_words/micro_wake_word/config.h"
#include "ui/utils/emoji/emoji_repository.h"

#include <cJSON.h>
#include <esp_log.h>
#include <cbin_font.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#define TAG "Assets"

namespace {

bool HasSuffix(const std::string& value, const char* suffix) {
    const size_t suffix_size = std::strlen(suffix);
    return value.size() >= suffix_size &&
           value.compare(value.size() - suffix_size, suffix_size, suffix) == 0;
}

std::string ReplaceJsonSuffixWithModel(const std::string& descriptor_path) {
    if (!HasSuffix(descriptor_path, ".json")) {
        return {};
    }
    return descriptor_path.substr(0, descriptor_path.size() - 5) + ".tflite";
}

bool ParseMicroWakeWordConfig(const cJSON* descriptor, MicroWakeWordConfig& config) {
    const cJSON* micro = cJSON_GetObjectItemCaseSensitive(descriptor, "micro");
    if (micro == nullptr) {
        return true;
    }
    if (!cJSON_IsObject(micro)) {
        ESP_LOGW(TAG, "Ignoring invalid wake-word micro configuration");
        return true;
    }

    const cJSON* cutoff = cJSON_GetObjectItemCaseSensitive(micro, "probability_cutoff");
    if (cJSON_IsNumber(cutoff) && cutoff->valuedouble > 0.0 && cutoff->valuedouble <= 1.0) {
        config.probability_cutoff = static_cast<float>(cutoff->valuedouble);
    }

    const cJSON* sliding = cJSON_GetObjectItemCaseSensitive(micro, "sliding_window_size");
    if (cJSON_IsNumber(sliding) && sliding->valuedouble >= 1.0 &&
        sliding->valuedouble <= 64.0) {
        config.sliding_window_size = static_cast<size_t>(sliding->valuedouble);
    }

    const cJSON* step = cJSON_GetObjectItemCaseSensitive(micro, "feature_step_size");
    if (cJSON_IsNumber(step) && step->valuedouble >= 1.0 && step->valuedouble <= 100.0) {
        config.feature_step_size = static_cast<uint8_t>(step->valuedouble);
    }

    const cJSON* arena = cJSON_GetObjectItemCaseSensitive(micro, "tensor_arena_size");
    if (cJSON_IsNumber(arena) && arena->valuedouble >= 16.0 * 1024.0 &&
        arena->valuedouble <= 1024.0 * 1024.0) {
        config.tensor_arena_size = static_cast<size_t>(arena->valuedouble);
    }
    return true;
}

bool LoadWakeWordPair(AssetStore& wake_assets, const std::string& descriptor_path) {
    void* descriptor_data = nullptr;
    size_t descriptor_size = 0;
    if (!wake_assets.GetAssetData(descriptor_path, descriptor_data, descriptor_size)) {
        return false;
    }

    cJSON* descriptor = cJSON_ParseWithLength(
        static_cast<char*>(descriptor_data), descriptor_size);
    if (descriptor == nullptr) {
        ESP_LOGW(TAG, "Invalid wake-word descriptor: %s", descriptor_path.c_str());
        return false;
    }

    const cJSON* wake_word = cJSON_GetObjectItemCaseSensitive(descriptor, "wake_word");
    if (!cJSON_IsString(wake_word) || wake_word->valuestring == nullptr) {
        ESP_LOGW(TAG, "Wake-word descriptor has no wake_word: %s", descriptor_path.c_str());
        cJSON_Delete(descriptor);
        return false;
    }

    MicroWakeWordConfig config;
    config.wake_word = wake_word->valuestring;
    ParseMicroWakeWordConfig(descriptor, config);
    std::string model_path = ReplaceJsonSuffixWithModel(descriptor_path);
    void* model_data = nullptr;
    size_t model_size = 0;
    bool model_found = !model_path.empty() &&
                       wake_assets.GetAssetData(model_path, model_data, model_size);

    // Prefer a same-stem model and honor an explicit model field when present.
    if (!model_found) {
        const cJSON* model = cJSON_GetObjectItemCaseSensitive(descriptor, "model");
        if (cJSON_IsString(model) && model->valuestring != nullptr) {
            model_path = model->valuestring;
            if (model_path.empty() || model_path.front() != '/') {
                const size_t slash = descriptor_path.find_last_of('/');
                model_path = descriptor_path.substr(0, slash + 1) + model_path;
            }
            model_found = wake_assets.GetAssetData(model_path, model_data, model_size);
        }
    }

    if (model_found && model_data != nullptr && model_size != 0) {
        Application::GetInstance().GetAudioService().SetMicroWakeWordModel(
            model_data, model_size, config.wake_word, config);
        ESP_LOGI(TAG, "Loaded MWW model from %s (%s, cutoff=%.3f, window=%u, step=%u, arena=%u)",
                 model_path.c_str(), config.wake_word.c_str(), config.probability_cutoff,
                 static_cast<unsigned>(config.sliding_window_size),
                 static_cast<unsigned>(config.feature_step_size),
                 static_cast<unsigned>(config.tensor_arena_size));
        cJSON_Delete(descriptor);
        return true;
    }

    cJSON_Delete(descriptor);
    return false;
}

}  // namespace

bool Assets::Apply() {
    bool loaded = true;
    if (!LoadFonts()) {
        ESP_LOGE(TAG, "Required UI fonts are unavailable in respak");
        loaded = false;
    }
    if (!LocalizedAudio::GetInstance().Initialize(RespakStore())) {
        ESP_LOGE(TAG, "Failed to initialize localized audio from respak");
        loaded = false;
    }
    if (!ReloadTone()) {
        ESP_LOGE(TAG, "Failed to load tone resources");
        loaded = false;
    }
    if (!LoadEmoji()) {
        ESP_LOGE(TAG, "Failed to load emoji resources");
        loaded = false;
    }
    if (!LoadWakeWord(false)) {
        loaded = false;
    }
    return loaded;
}

bool Assets::ReloadTone() {
    return ToneModel::GetInstance().Initialize(ToneStore(), "/");
}

bool Assets::ReloadEmoji() {
    return LoadEmoji();
}

bool Assets::ReloadWakeWord() {
    return LoadWakeWord(true);
}

bool Assets::LoadEmoji() {
    auto repository = std::make_shared<EmojiRepository>();
    if (!repository->LoadFromAssets(EmojiStore())) {
        return false;
    }

    auto apply_repository = [&repository]() {
        auto& themes = LvglThemeManager::GetInstance();
        if (auto* light = themes.GetTheme("light")) {
            light->set_emoji_repository(repository);
        }
        if (auto* dark = themes.GetTheme("dark")) {
            dark->set_emoji_repository(repository);
        }
    };

    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        ESP_LOGE(TAG, "Cannot install emoji resources without a display");
        return false;
    }
    DisplayLockGuard lock(display);
    if (!lock.locked()) {
        ESP_LOGE(TAG, "Failed to lock display while installing emoji resources");
        return false;
    }
    apply_repository();
    return true;
}

bool Assets::LoadWakeWord(bool force_reload) {
    auto& wake_assets = WakeWordStore();
#if CONFIG_USE_MICRO_WAKE_WORD
    (void)force_reload;
    std::vector<std::string> wake_files;
    if (wake_assets.ListAssetFiles("/", wake_files)) {
        std::sort(wake_files.begin(), wake_files.end());
        for (const auto& path : wake_files) {
            if (HasSuffix(path, ".json") && LoadWakeWordPair(wake_assets, path)) {
                return true;
            }
        }
    }
    ESP_LOGE(TAG, "The wake_word resource descriptor or TFLite model is missing");
#else
    if (models_list_ != nullptr) {
        if (!force_reload) {
            return true;
        }
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
        esp_srmodel_deinit(models_list_);
        models_list_ = nullptr;
    }

    void* model_data = nullptr;
    size_t model_size = 0;
    const char* candidates[] = {"/srmodels.bin", "/cae_esr.bin"};
    for (const char* candidate : candidates) {
        if (!wake_assets.GetAssetData(candidate, model_data, model_size) ||
            model_data == nullptr || model_size == 0) {
            continue;
        }

        models_list_ = srmodel_load(static_cast<uint8_t*>(model_data));
        if (models_list_ != nullptr) {
            Application::GetInstance().GetAudioService().SetModelsList(models_list_);
            ESP_LOGI(TAG, "Loaded ESP-SR model from %s", candidate);
            return true;
        }
        ESP_LOGW(TAG, "Wake-word resource is not an ESP-SR model: %s", candidate);
    }
    ESP_LOGE(TAG, "The wake_word ESP-SR model is missing");
#endif
    return false;
}

bool Assets::LoadFonts() {
    auto& respak = RespakStore();
    const uint32_t current_generation = respak.generation();
    if (current_generation != 0 && loaded_fonts_generation_ == current_generation) {
        auto& theme_manager = LvglThemeManager::GetInstance();
        auto light_theme = theme_manager.GetTheme("light");
        auto dark_theme = theme_manager.GetTheme("dark");
        const auto theme_ready = [](LvglTheme* theme) {
            return theme != nullptr && theme->text_font() != nullptr &&
                   theme->icon_font() != nullptr && theme->large_icon_font() != nullptr;
        };
        if ((light_theme == nullptr || theme_ready(light_theme)) && theme_ready(dark_theme)) {
            return true;
        }
    }

    std::vector<std::string> paths;
    if (!respak.ListAssetFiles("/font", paths) || paths.empty()) {
        return false;
    }
    std::sort(paths.begin(), paths.end());

    constexpr char kTextFontFilename[] = "font_puhui_common_20_4.bin";
    std::string text_path;
    std::string icon_path;
    std::string large_icon_path;
    for (const auto& path : paths) {
        const size_t slash = path.find_last_of('/');
        const std::string filename = path.substr(slash == std::string::npos ? 0 : slash + 1);
        if (text_path.empty() && filename == kTextFontFilename) {
            text_path = path;
        } else if (filename == "font_awesome_20_4.bin") {
            icon_path = path;
        } else if (filename == "font_awesome_30_4.bin") {
            large_icon_path = path;
        }
    }

    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");
    auto load_font = [&](const std::string& path,
                         const char* role,
                         auto setter) -> bool {
        if (path.empty()) {
            ESP_LOGE(TAG, "Required %s font is missing from respak", role);
            return false;
        }
        void* data = nullptr;
        size_t size = 0;
        if (!respak.GetAssetData(path, data, size) || data == nullptr || size == 0) {
            ESP_LOGE(TAG, "The %s font file %s is not found", role, path.c_str());
            return false;
        }
        auto font = std::make_shared<LvglCBinFont>(data);
        if (font->font() == nullptr) {
            ESP_LOGE(TAG, "Failed to load %s font file %s", role, path.c_str());
            return false;
        }
        if (light_theme != nullptr) {
            setter(light_theme, font);
        }
        if (dark_theme != nullptr) {
            setter(dark_theme, font);
        }
        ESP_LOGI(TAG, "Loaded %s font from %s", role, path.c_str());
        return true;
    };

    const bool text_loaded = load_font(text_path, "text", [](LvglTheme* theme, const std::shared_ptr<LvglFont>& font) {
        theme->set_text_font(font);
    });
    const bool icon_loaded = load_font(icon_path, "icon", [](LvglTheme* theme, const std::shared_ptr<LvglFont>& font) {
        theme->set_icon_font(font);
    });
    const bool large_icon_loaded = load_font(large_icon_path, "large icon", [](LvglTheme* theme, const std::shared_ptr<LvglFont>& font) {
        theme->set_large_icon_font(font);
    });
    const bool loaded = text_loaded && icon_loaded && large_icon_loaded;
    if (loaded) {
        loaded_fonts_generation_ = current_generation;
    }
    return loaded;
}

Assets::Assets()
    : respak_store_("respak", nullptr, false),
      tone_store_("tone", nullptr, false),
      emoji_store_("emoji", nullptr, false),
      wake_word_store_("wake_word", nullptr, false) {}

AssetStore& Assets::SelectStore(AssetStore& store) {
    if (!store.partition_valid()) {
        store.Mount();
    }
    return store;
}

AssetStore& Assets::RespakStore() { return SelectStore(respak_store_); }
AssetStore& Assets::ToneStore() { return SelectStore(tone_store_); }
AssetStore& Assets::EmojiStore() { return SelectStore(emoji_store_); }
AssetStore& Assets::WakeWordStore() { return SelectStore(wake_word_store_); }
