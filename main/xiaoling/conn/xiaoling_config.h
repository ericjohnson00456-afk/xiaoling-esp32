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

inline const char* GetHost() {
    switch (KvUser::GetDeviceMode()) {
        case 1: return LSC_HOST_STAGING;
        case 2: return LSC_HOST_INTEGRATION;
        default: return LSC_HOST;
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
