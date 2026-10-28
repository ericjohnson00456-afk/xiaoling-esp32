#include "kv_user.h"

#include "system/settings.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace KvOta {
namespace {

constexpr char kAppStorageKey[] = "dis_app_ota";
constexpr char kWakeWordStorageKey[] = "dis_wake_ota";
constexpr char kToneStorageKey[] = "dis_tone_ota";
constexpr char kEmojiStorageKey[] = "dis_emoji_ota";

const char* StorageKey(UpdateType type) {
    switch (type) {
        case UpdateType::App: return kAppStorageKey;
        case UpdateType::WakeWord: return kWakeWordStorageKey;
        case UpdateType::Tone: return kToneStorageKey;
        case UpdateType::Emoji: return kEmojiStorageKey;
    }
    return "";
}

bool ParseInt32(const char* text, int32_t& value) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    value = static_cast<int32_t>(parsed);
    return true;
}

bool ParseType(const char* name, UpdateType& type) {
    if (name == nullptr) return false;
    if (std::strcmp(name, "app") == 0 ||
        std::strcmp(name, KvUser::kDisableAppUpdate) == 0) {
        type = UpdateType::App;
    } else if (std::strcmp(name, "wakeword") == 0 ||
               std::strcmp(name, "wake_word") == 0 ||
               std::strcmp(name, KvUser::kDisableWakewordUpdate) == 0) {
        type = UpdateType::WakeWord;
    } else if (std::strcmp(name, "tone") == 0 ||
               std::strcmp(name, KvUser::kDisableToneUpdate) == 0) {
        type = UpdateType::Tone;
    } else if (std::strcmp(name, "emoji") == 0 ||
               std::strcmp(name, KvUser::kDisableEmojiUpdate) == 0) {
        type = UpdateType::Emoji;
    } else {
        return false;
    }
    return true;
}

void PrintUsage() {
    std::printf(
        "Usage:\n"
        "  kv ota show\n"
        "  kv ota get <app|wakeword|tone|emoji|user.key>\n"
        "  kv ota set <app|wakeword|tone|emoji|user.key> <int>\n");
}

}  // namespace

const char* LogicalKey(UpdateType type) {
    switch (type) {
        case UpdateType::App: return KvUser::kDisableAppUpdate;
        case UpdateType::WakeWord: return KvUser::kDisableWakewordUpdate;
        case UpdateType::Tone: return KvUser::kDisableToneUpdate;
        case UpdateType::Emoji: return KvUser::kDisableEmojiUpdate;
    }
    return "";
}

bool IsDisabled(UpdateType type) {
    return GetValue(type) != 0;
}

int32_t GetValue(UpdateType type) {
    Settings settings(KvUser::kNamespace, false);
    // 唤醒词资源默认不参与 OTA；配置 disable_wakeword_update=0 可显式恢复更新。
    // 其他资源保持未配置时允许 OTA 的兼容行为。
    const int32_t default_value = type == UpdateType::WakeWord ? 1 : 0;
    return settings.GetInt(StorageKey(type), default_value);
}

void SetDisabled(UpdateType type, int32_t value) {
    Settings settings(KvUser::kNamespace, true);
    settings.SetInt(StorageKey(type), value);
}

int HandleKvOtaCommand(int argc, char** argv) {
    if (argc <= 1 || std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return 0;
    }

    if (std::strcmp(argv[1], "show") == 0) {
        if (argc != 2) {
            PrintUsage();
            return 1;
        }
        for (const auto type : {UpdateType::App, UpdateType::WakeWord,
                                UpdateType::Tone, UpdateType::Emoji}) {
            std::printf("%s=%ld\n", LogicalKey(type),
                        static_cast<long>(GetValue(type)));
        }
        return 0;
    }

    if (std::strcmp(argv[1], "get") == 0) {
        if (argc != 3) {
            PrintUsage();
            return 1;
        }
        UpdateType type;
        if (!ParseType(argv[2], type)) {
            std::printf("unknown OTA KV: %s\n", argv[2]);
            return 1;
        }
        std::printf("%s=%ld\n", LogicalKey(type),
                    static_cast<long>(GetValue(type)));
        return 0;
    }

    if (std::strcmp(argv[1], "set") == 0) {
        if (argc != 4) {
            PrintUsage();
            return 1;
        }
        UpdateType type;
        int32_t value = 0;
        if (!ParseType(argv[2], type) || !ParseInt32(argv[3], value)) {
            std::printf("invalid OTA KV or int value\n");
            return 1;
        }
        SetDisabled(type, value);
        std::printf("flash set %s=%ld success\n", LogicalKey(type), static_cast<long>(value));
        return 0;
    }

    PrintUsage();
    return 1;
}

}  // namespace KvOta
