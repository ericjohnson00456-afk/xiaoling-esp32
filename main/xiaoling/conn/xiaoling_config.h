#ifndef XIAOLING_CONFIG_H
#define XIAOLING_CONFIG_H

#include "shell/cmd/kv/kv_user.h"

#include <string>

#define LSC_HOST             "api.listenai.com"
#define LSC_HOST_STAGING     "staging-api.listenai.com"
#define LSC_HOST_INTEGRATION "integration-api.listenai.com"

namespace XiaolingConfig {

constexpr const char* kDeviceId = "";
constexpr const char* kUplinkAudioEncoding = "opus-wb";

enum class Environment {
    Production = 0,
    Staging = 1,
    Integration = 2,
};

inline Environment GetEnvironment() {
    switch (KvUser::GetDeviceMode()) {
        case static_cast<int32_t>(Environment::Staging):
            return Environment::Staging;
        case static_cast<int32_t>(Environment::Integration):
            return Environment::Integration;
        default:
            return Environment::Production;
    }
}

inline const char* GetHost() {
    switch (GetEnvironment()) {
        case Environment::Staging: return LSC_HOST_STAGING;
        case Environment::Integration: return LSC_HOST_INTEGRATION;
        case Environment::Production:
        default: return LSC_HOST;
    }
}

inline const char* GetEnvironmentName() {
    switch (GetEnvironment()) {
        case Environment::Staging: return "staging";
        case Environment::Integration: return "integration";
        case Environment::Production:
        default: return "production";
    }
}

inline std::string GetTokenUrl() {
    return std::string("http://") + GetHost() + "/v1/auth/tokens";
}

inline std::string GetWebSocketUrl() {
    return std::string("ws://") + GetHost();
}

}  // namespace XiaolingConfig

#endif  // XIAOLING_CONFIG_H
