#ifndef SHELL_KV_USER_COMMAND_H
#define SHELL_KV_USER_COMMAND_H

#include <cstdint>
#include <esp_err.h>

namespace KvUser {

inline constexpr char kNamespace[] = "user";
inline constexpr char kDisableAppUpdate[] = "user.disable_app_update";
inline constexpr char kDisableWakewordUpdate[] = "user.disable_wakeword_update";
inline constexpr char kDisableToneUpdate[] = "user.disable_tone_update";
inline constexpr char kDisableEmojiUpdate[] = "user.disable_emoji_update";
inline constexpr char kDeviceMode[] = "user.device_mode";
inline constexpr char kIntMode[] = "user.int_mode";
inline constexpr char kUiLanguage[] = "user.ui_language";
inline constexpr char kFullDuplexTimeoutMs[] = "user.full_duplex_timeout_ms";
inline constexpr char kIdleExitTimeoutMs[] = "user.idle_exit_timeout_ms";

int32_t GetDeviceMode();
void SetDeviceMode(int32_t value);
int32_t GetIntMode();
void SetIntMode(int32_t value);
int32_t GetUiLanguage();
void SetUiLanguage(int32_t value);
int32_t GetFullDuplexTimeoutMs();
void SetFullDuplexTimeoutMs(int32_t value);
int32_t GetIdleExitTimeoutMs();
void SetIdleExitTimeoutMs(int32_t value);

}  // namespace KvUser

namespace KvOta {

enum class UpdateType {
    App,
    WakeWord,
    Tone,
    Emoji,
};

const char* LogicalKey(UpdateType type);
bool IsDisabled(UpdateType type);
int32_t GetValue(UpdateType type);
void SetDisabled(UpdateType type, int32_t value);
int HandleKvOtaCommand(int argc, char** argv);

}  // namespace KvOta

esp_err_t RegisterKvCommands();

#endif  // SHELL_KV_USER_COMMAND_H
