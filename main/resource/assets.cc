#include "assets.h"
#include "board.h"
#include "display/display.h"
#include "application.h"
#include "lvgl_theme.h"
#include "display/lcd_display.h"
#include "audio/wake_words/micro_wake_word/config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/md5.h>
#include <cbin_font.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#include <array>

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

    // Keep compatibility with descriptors whose model field points to a
    // differently named file, while preferring the same-stem pair.
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
    void* ptr = nullptr;
    size_t size = 0;
    cJSON* root = nullptr;
    if (GetAssetData("index.json", ptr, size)) {
        root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    } else {
        // respak.bin from the ARCS-MINI resource layout intentionally contains
        // only font files and has no legacy index.json. Provide the canonical
        // font path so the existing theme application path still works.
        root = cJSON_CreateObject();
        if (root != nullptr) {
            std::vector<std::string> font_paths;
            if (ListAssetFiles("/font", font_paths) && !font_paths.empty()) {
                cJSON_AddStringToObject(root, "text_font", font_paths.front().c_str());
            }
        }
    }
    if (root == nullptr) {
        ESP_LOGE(TAG, "The index.json file is not valid");
        return false;
    }

    cJSON* version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(version)) {
        if (version->valuedouble > 1) {
            ESP_LOGE(TAG, "The assets version %d is not supported, please upgrade the firmware", version->valueint);
            return false;
        }
    }

#if CONFIG_USE_MICRO_WAKE_WORD
    cJSON *mwwmodel = cJSON_GetObjectItem(root, "mwwmodel");
    if (cJSON_IsString(mwwmodel)) {
        std::string mwwmodel_file = mwwmodel->valuestring;
        if (GetAssetData(mwwmodel_file, ptr, size)) {
            auto& app = Application::GetInstance();
            app.GetAudioService().SetMicroWakeWordModel((const void*)ptr, size);
        } else {
            ESP_LOGE(TAG, "The MWW model file %s is not found", mwwmodel_file.c_str());
        }
    } else {
        auto& wake_assets = WakeWordStore();
        std::vector<std::string> wake_files;
        bool loaded = false;
        if (wake_assets.ListAssetFiles("/", wake_files)) {
            std::sort(wake_files.begin(), wake_files.end());
            for (const auto& path : wake_files) {
                if (HasSuffix(path, ".json") && LoadWakeWordPair(wake_assets, path)) {
                    loaded = true;
                    break;
                }
            }
        }

        // Compatibility with the previous split-resource manifest format.
        if (!loaded && wake_assets.GetAssetData("/index.json", ptr, size)) {
            cJSON* wake_index = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
            const cJSON* model = wake_index ? cJSON_GetObjectItem(wake_index, "model") : nullptr;
            const cJSON* name = wake_index ? cJSON_GetObjectItem(wake_index, "wake_word") : nullptr;
            if (cJSON_IsString(model) && cJSON_IsString(name)) {
                std::string model_path = model->valuestring;
                if (model_path.empty() || model_path.front() != '/') {
                    model_path = "/" + model_path;
                }
                if (wake_assets.GetAssetData(model_path, ptr, size) && ptr != nullptr && size != 0) {
                    MicroWakeWordConfig config;
                    config.wake_word = name->valuestring;
                    if (wake_index != nullptr) {
                        ParseMicroWakeWordConfig(wake_index, config);
                    }
                    Application::GetInstance().GetAudioService().SetMicroWakeWordModel(
                        ptr, size, config.wake_word, config);
                    loaded = true;
                }
            }
            cJSON_Delete(wake_index);
        }
        if (!loaded) {
            ESP_LOGE(TAG, "The wake_word resource manifest or MWW model is missing");
        }
    }
#else // CONFIG_USE_MICRO_WAKE_WORD
    cJSON* srmodels = cJSON_GetObjectItem(root, "srmodels");
    if (cJSON_IsString(srmodels)) {
        std::string srmodels_file = srmodels->valuestring;
        if (GetAssetData(srmodels_file, ptr, size)) {
            if (models_list_ != nullptr) {
                esp_srmodel_deinit(models_list_);
                models_list_ = nullptr;
            }
            models_list_ = srmodel_load(static_cast<uint8_t*>(ptr));
            if (models_list_ != nullptr) {
                auto& app = Application::GetInstance();
                app.GetAudioService().SetModelsList(models_list_);
            } else {
                ESP_LOGE(TAG, "Failed to load srmodels.bin");
            }
        } else {
            ESP_LOGE(TAG, "The srmodels file %s is not found", srmodels_file.c_str());
        }
    }
#endif // CONFIG_USE_MICRO_WAKE_WORD

    // New split resource packages keep wake-word models in wake_word.bin.
    // Prefer the conventional srmodels.bin name when present; cae_esr.bin is
    // accepted for the ARCS-MINI package and ignored if unsupported by ESP-SR.
 #if !CONFIG_USE_MICRO_WAKE_WORD
    if (models_list_ == nullptr) {
        auto& wake_assets = WakeWordStore();
        const char* candidates[] = {"/srmodels.bin", "/cae_esr.bin"};
        for (const char* candidate : candidates) {
            if (!wake_assets.GetAssetData(candidate, ptr, size) || ptr == nullptr || size == 0) {
                continue;
            }
            models_list_ = srmodel_load(static_cast<uint8_t*>(ptr));
            if (models_list_ != nullptr) {
                Application::GetInstance().GetAudioService().SetModelsList(models_list_);
                break;
            }
            ESP_LOGW(TAG, "Wake-word resource is not an ESP-SR model: %s", candidate);
        }
    }
 #endif

    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");

    cJSON* font = cJSON_GetObjectItem(root, "text_font");
    if (cJSON_IsString(font)) {
        std::string fonts_text_file = font->valuestring;
        if (GetAssetData(fonts_text_file, ptr, size)) {
            auto text_font = std::make_shared<LvglCBinFont>(ptr);
            if (text_font->font() == nullptr) {
                ESP_LOGE(TAG, "Failed to load fonts.bin");
                return false;
            }
            if (light_theme != nullptr) {
                light_theme->set_text_font(text_font);
            }
            if (dark_theme != nullptr) {
                dark_theme->set_text_font(text_font);
            }
        } else {
            ESP_LOGE(TAG, "The font file %s is not found", fonts_text_file.c_str());
        }
    }

    cJSON* emoji_collection = cJSON_GetObjectItem(root, "emoji_collection");
    if (cJSON_IsArray(emoji_collection)) {
        auto custom_emoji_repository = std::make_shared<EmojiRepository>();
        int emoji_count = cJSON_GetArraySize(emoji_collection);
        for (int i = 0; i < emoji_count; i++) {
            cJSON* emoji = cJSON_GetArrayItem(emoji_collection, i);
            if (cJSON_IsObject(emoji)) {
                cJSON* name = cJSON_GetObjectItem(emoji, "name");
                cJSON* file = cJSON_GetObjectItem(emoji, "file");
                cJSON* eaf = cJSON_GetObjectItem(emoji, "eaf");
                if (cJSON_IsString(name) && cJSON_IsString(file) && (NULL== eaf)) {
                    if (!GetAssetData(file->valuestring, ptr, size)) {
                        ESP_LOGE(TAG, "Emoji %s image file %s is not found", name->valuestring, file->valuestring);
                        continue;
                    }
                    custom_emoji_repository->AddEmoji(name->valuestring, new LvglRawImage(ptr, size));
                }
            }
        }
        if (light_theme != nullptr) {
            light_theme->set_emoji_repository(custom_emoji_repository);
        }
        if (dark_theme != nullptr) {
            dark_theme->set_emoji_repository(custom_emoji_repository);
        }
    }

    cJSON* skin = cJSON_GetObjectItem(root, "skin");
    if (cJSON_IsObject(skin)) {
        cJSON* light_skin = cJSON_GetObjectItem(skin, "light");
        if (cJSON_IsObject(light_skin) && light_theme != nullptr) {
            cJSON* text_color = cJSON_GetObjectItem(light_skin, "text_color");
            cJSON* background_color = cJSON_GetObjectItem(light_skin, "background_color");
            cJSON* background_image = cJSON_GetObjectItem(light_skin, "background_image");
            if (cJSON_IsString(text_color)) {
                light_theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
            }
            if (cJSON_IsString(background_color)) {
                light_theme->set_background_color(LvglTheme::ParseColor(background_color->valuestring));
                light_theme->set_chat_background_color(LvglTheme::ParseColor(background_color->valuestring));
            }
            if (cJSON_IsString(background_image)) {
                if (!GetAssetData(background_image->valuestring, ptr, size)) {
                    ESP_LOGE(TAG, "The background image file %s is not found", background_image->valuestring);
                    return false;
                }
                auto background_image = std::make_shared<LvglCBinImage>(ptr);
                light_theme->set_background_image(background_image);
            }
        }
        cJSON* dark_skin = cJSON_GetObjectItem(skin, "dark");
        if (cJSON_IsObject(dark_skin) && dark_theme != nullptr) {
            cJSON* text_color = cJSON_GetObjectItem(dark_skin, "text_color");
            cJSON* background_color = cJSON_GetObjectItem(dark_skin, "background_color");
            cJSON* background_image = cJSON_GetObjectItem(dark_skin, "background_image");
            if (cJSON_IsString(text_color)) {
                dark_theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
            }
            if (cJSON_IsString(background_color)) {
                dark_theme->set_background_color(LvglTheme::ParseColor(background_color->valuestring));
                dark_theme->set_chat_background_color(LvglTheme::ParseColor(background_color->valuestring));
            }
            if (cJSON_IsString(background_image)) {
                if (!GetAssetData(background_image->valuestring, ptr, size)) {
                    ESP_LOGE(TAG, "The background image file %s is not found", background_image->valuestring);
                    return false;
                }
                auto background_image = std::make_shared<LvglCBinImage>(ptr);
                dark_theme->set_background_image(background_image);
            }
        }
    }

    auto display = Board::GetInstance().GetDisplay();
    ESP_LOGI(TAG, "Refreshing display theme...");

    auto current_theme = display->GetTheme();
    if (current_theme != nullptr) {
        display->SetTheme(current_theme);
    }

    // Parse hide_subtitle configuration
    cJSON* hide_subtitle = cJSON_GetObjectItem(root, "hide_subtitle");
    if (cJSON_IsBool(hide_subtitle)) {
        bool hide = cJSON_IsTrue(hide_subtitle);
        auto lcd_display = dynamic_cast<LcdDisplay*>(display);
        if (lcd_display != nullptr) {
            lcd_display->SetHideSubtitle(hide);
            ESP_LOGI(TAG, "Set hide_subtitle to %s", hide ? "true" : "false");
        }
    }



    cJSON_Delete(root);
    return true;
}

Assets::Assets()
    : AssetStore("respak", "assets", true),
      tone_store_("tone", nullptr, false),
      emoji_store_("emoji", nullptr, false),
      wake_word_store_("wake_word", nullptr, false) {}

AssetStore& Assets::SelectStore(Resource resource) {
    AssetStore* selected = this;
    switch (resource) {
        case Resource::Respak: selected = this; break;
        case Resource::Tone: selected = &tone_store_; break;
        case Resource::Emoji: selected = &emoji_store_; break;
        case Resource::WakeWord: selected = &wake_word_store_; break;
    }
    if (selected != this && !selected->partition_valid()) {
        selected->Mount();
    }
    // Older partition tables have one "assets" package. Keep it usable until
    // the split resource table is flashed.
    if (selected != this && !selected->partition_valid()) {
        return *this;
    }
    return *selected;
}

AssetStore& Assets::ToneStore() { return SelectStore(Resource::Tone); }
AssetStore& Assets::EmojiStore() { return SelectStore(Resource::Emoji); }
AssetStore& Assets::WakeWordStore() { return SelectStore(Resource::WakeWord); }

bool Assets::Download(std::string url,
                      std::function<void(int progress, size_t speed)> progress_callback) {
    return DownloadResource(Resource::Respak, std::move(url), std::move(progress_callback));
}

bool Assets::DownloadResource(Resource resource, std::string url,
                              std::function<void(int progress, size_t speed)> progress_callback) {
    return DownloadResource(resource, url, 0, {}, std::move(progress_callback));
}

bool Assets::VerifyResource(Resource resource, size_t expected_size,
                            const std::array<uint8_t, 16>& expected_md5) {
    AssetStore* target = this;
    switch (resource) {
        case Resource::Respak: target = this; break;
        case Resource::Tone: target = &tone_store_; break;
        case Resource::Emoji: target = &emoji_store_; break;
        case Resource::WakeWord: target = &wake_word_store_; break;
    }
    if (target != this && !target->partition_valid()) {
        target->Mount();
    }
    const esp_partition_t* partition = target->partition();
    if (partition == nullptr || expected_size == 0 || expected_size > partition->size) {
        ESP_LOGE(TAG, "Invalid resource partition or expected size: %u",
                 static_cast<unsigned>(expected_size));
        return false;
    }

    constexpr size_t kBufferSize = 4096;
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kBufferSize]);
    if (!buffer) {
        return false;
    }

    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    size_t offset = 0;
    bool success = true;
    while (offset < expected_size) {
        const size_t chunk = std::min(kBufferSize, expected_size - offset);
        if (esp_partition_read(partition, offset, buffer.get(), chunk) != ESP_OK) {
            success = false;
            break;
        }
        mbedtls_md5_update(&md5, buffer.get(), chunk);
        offset += chunk;
    }
    std::array<uint8_t, 16> actual{};
    mbedtls_md5_finish(&md5, actual.data());
    mbedtls_md5_free(&md5);
    success = success && actual == expected_md5;
    ESP_LOGI(TAG, "Resource %s (%u bytes)", success ? "is current" : "needs update",
             static_cast<unsigned>(expected_size));
    return success;
}

bool Assets::DownloadResource(Resource resource, const std::string& url, size_t expected_size,
                              const std::array<uint8_t, 16>& expected_md5,
                              std::function<void(int progress, size_t speed)> progress_callback) {
    AssetStore* target = this;
    switch (resource) {
        case Resource::Respak: target = this; break;
        case Resource::Tone: target = &tone_store_; break;
        case Resource::Emoji: target = &emoji_store_; break;
        case Resource::WakeWord: target = &wake_word_store_; break;
    }
    if (target != this && !target->partition_valid()) {
        target->Mount();
    }
    // Never write a tone/emoji/wake-word URL into the legacy respak/assets
    // partition when a split partition has not been flashed yet.
    if (target != this && target->partition() == nullptr) {
        ESP_LOGE(TAG, "Split resource partition is unavailable");
        return false;
    }
    target->Unmount();
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        target->Mount();
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr || !http->Open("GET", url) || http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to open resource URL: %s", url.c_str());
        target->Mount();
        return false;
    }
    const size_t content_length = http->GetBodyLength();
    if (content_length == 0 || target->partition() == nullptr ||
        content_length > target->partition()->size ||
        (expected_size != 0 && content_length != expected_size)) {
        ESP_LOGE(TAG, "Invalid resource size: %u", static_cast<unsigned>(content_length));
        http->Close();
        target->Mount();
        return false;
    }

    constexpr size_t kBufferSize = 4096;
    constexpr size_t kSectorSize = 4096;
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[kBufferSize]);
    if (!buffer) { http->Close(); target->Mount(); return false; }
    const size_t erase_size = (content_length + kSectorSize - 1) / kSectorSize * kSectorSize;
    if (esp_partition_erase_range(target->partition(), 0, erase_size) != ESP_OK) {
        http->Close(); target->Mount(); return false;
    }
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    size_t written = 0;
    size_t recent_read = 0;
    int64_t last_report_time = esp_timer_get_time();
    while (written < content_length) {
        int ret = http->Read(buffer.get(), std::min(kBufferSize, content_length - written));
        if (ret <= 0 || esp_partition_write(target->partition(), written, buffer.get(), ret) != ESP_OK) {
            ESP_LOGE(TAG, "Resource download/write failed at %u", static_cast<unsigned>(written));
            mbedtls_md5_free(&md5);
            http->Close(); target->Mount(); return false;
        }
        mbedtls_md5_update(&md5, reinterpret_cast<const uint8_t*>(buffer.get()), ret);
        written += static_cast<size_t>(ret);
        recent_read += static_cast<size_t>(ret);
        const int64_t now = esp_timer_get_time();
        if (progress_callback && (now - last_report_time >= 1000000 || written == content_length)) {
            progress_callback(static_cast<int>(written * 100 / content_length), recent_read);
            last_report_time = now;
            recent_read = 0;
        }
    }
    http->Close();
    std::array<uint8_t, 16> actual{};
    mbedtls_md5_finish(&md5, actual.data());
    mbedtls_md5_free(&md5);
    if (expected_size != 0 && actual != expected_md5) {
        ESP_LOGE(TAG, "Downloaded resource MD5 mismatch");
        target->Mount();
        return false;
    }
    if (!target->Mount()) return false;
    return true;
}
