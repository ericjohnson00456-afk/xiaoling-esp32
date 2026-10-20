#ifndef XIAOLING_CONFIG_H
#define XIAOLING_CONFIG_H

#include "shell/cmd/kv/kv_user.h"

#include <atomic>
#include <cstdint>
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

namespace detail {

inline std::atomic<int32_t>& CachedDeviceMode() {
    static std::atomic<int32_t> mode{-1};
    return mode;
}

}  // namespace detail

// Read the environment once on an internal-RAM startup task. Network workers
// may use the resulting host without entering NVS while Flash cache is off.
inline void Refresh() {
    detail::CachedDeviceMode().store(KvUser::GetDeviceMode(),
                                     std::memory_order_release);
}

inline void Initialize() {
    Refresh();
}

inline Environment GetEnvironment() {
    switch (detail::CachedDeviceMode().load(std::memory_order_acquire)) {
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
