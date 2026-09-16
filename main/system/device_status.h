#ifndef DEVICE_STATUS_H
#define DEVICE_STATUS_H

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <esp_log.h>

// 设备全局状态（enum + 状态机，header-only）。语音会话状态（听/说）不在这里，
// 见 xiaoling/xiaoling_events.h。
enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateAudioTesting,
    kDeviceStateFatalError
};

// 设备状态的唯一数据源：校验状态转移、通知订阅者。
// application 是唯一写者；LED/显示/板卡通过 GetState() 或
// BoardEvents::GetRuntimeState() 只读。
class DeviceStatus final {
public:
    // 订阅者也是进程级单例；保持到固件进程结束，避免静态析构顺序问题。
    static DeviceStatus& GetInstance() {
        static auto* instance = new DeviceStatus();
        return *instance;
    }

    DeviceStatus(const DeviceStatus&) = delete;
    DeviceStatus& operator=(const DeviceStatus&) = delete;

    static DeviceState GetState() { return GetInstance().current_state_.load(); }

    // 尝试转移状态；非法转移会被拒绝并记日志，返回是否成功。
    static bool TransitionTo(DeviceState new_state) {
        return GetInstance().DoTransitionTo(new_state);
    }

    static bool CanTransitionTo(DeviceState target) {
        auto& self = GetInstance();
        return self.IsValidTransition(self.current_state_.load(), target);
    }

    // 状态变更回调，参数为 (old_state, new_state)，在 TransitionTo() 调用者的上下文执行。
    using StateCallback = std::function<void(DeviceState, DeviceState)>;

    static int AddStateChangeListener(StateCallback callback) {
        auto& self = GetInstance();
        std::lock_guard<std::mutex> lock(self.mutex_);
        const int id = self.next_listener_id_++;
        self.listeners_.emplace_back(id, std::move(callback));
        return id;
    }

    static void RemoveStateChangeListener(int listener_id) {
        auto& self = GetInstance();
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.listeners_.erase(
            std::remove_if(self.listeners_.begin(), self.listeners_.end(),
                [listener_id](const auto& p) { return p.first == listener_id; }),
            self.listeners_.end());
    }

    static const char* GetStateName(DeviceState state) {
        static const char* const kStateStrings[] = {
            "unknown",
            "starting",
            "wifi_configuring",
            "idle",
            "connecting",
            "listening",
            "speaking",
            "upgrading",
            "activating",
            "audio_testing",
            "fatal_error",
            "invalid_state"
        };
        if (state >= 0 && state <= kDeviceStateFatalError) {
            return kStateStrings[state];
        }
        return kStateStrings[kDeviceStateFatalError + 1];
    }

private:
    DeviceStatus() = default;

    bool DoTransitionTo(DeviceState new_state) {
        const DeviceState old_state = current_state_.load();

        // No-op if already in the target state
        if (old_state == new_state) {
            return true;
        }

        if (!IsValidTransition(old_state, new_state)) {
            ESP_LOGW("DeviceStatus", "Invalid state transition: %s -> %s",
                     GetStateName(old_state), GetStateName(new_state));
            return false;
        }

        current_state_.store(new_state);
        ESP_LOGI("DeviceStatus", "State: %s -> %s",
                 GetStateName(old_state), GetStateName(new_state));

        NotifyStateChange(old_state, new_state);
        return true;
    }

    bool IsValidTransition(DeviceState from, DeviceState to) const {
        // Allow transition to the same state (no-op)
        if (from == to) {
            return true;
        }

        // Valid state transitions based on the state diagram
        switch (from) {
            case kDeviceStateUnknown:
                // Can only go to starting
                return to == kDeviceStateStarting;

            case kDeviceStateStarting:
                // Can go to wifi configuring or activating
                return to == kDeviceStateWifiConfiguring ||
                       to == kDeviceStateActivating;

            case kDeviceStateWifiConfiguring:
                // Can go to activating (after wifi connected) or audio testing
                return to == kDeviceStateActivating ||
                       to == kDeviceStateAudioTesting;

            case kDeviceStateAudioTesting:
                // Can go back to wifi configuring
                return to == kDeviceStateWifiConfiguring;

            case kDeviceStateActivating:
                // Can go to upgrading, idle, or back to wifi configuring (on error)
                return to == kDeviceStateUpgrading ||
                       to == kDeviceStateIdle ||
                       to == kDeviceStateWifiConfiguring;

            case kDeviceStateUpgrading:
                // Can go to idle (upgrade failed) or activating
                return to == kDeviceStateIdle ||
                       to == kDeviceStateActivating;

            case kDeviceStateIdle:
                // Can go to connecting, listening (manual mode), speaking, activating, upgrading, or wifi configuring
                return to == kDeviceStateConnecting ||
                       to == kDeviceStateListening ||
                       to == kDeviceStateSpeaking ||
                       to == kDeviceStateActivating ||
                       to == kDeviceStateUpgrading ||
                       to == kDeviceStateWifiConfiguring;

            case kDeviceStateConnecting:
                // Can go to idle (failed) or listening (success)
                return to == kDeviceStateIdle ||
                       to == kDeviceStateListening;

            case kDeviceStateListening:
                // Can go to speaking or idle
                return to == kDeviceStateSpeaking ||
                       to == kDeviceStateIdle;

            case kDeviceStateSpeaking:
                // Can go to listening or idle
                return to == kDeviceStateListening ||
                       to == kDeviceStateIdle;

            case kDeviceStateFatalError:
                // Cannot transition out of fatal error
                return false;

            default:
                return false;
        }
    }

    void NotifyStateChange(DeviceState old_state, DeviceState new_state) {
        std::vector<StateCallback> callbacks_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_copy.reserve(listeners_.size());
            for (const auto& [id, cb] : listeners_) {
                callbacks_copy.push_back(cb);
            }
        }

        for (const auto& cb : callbacks_copy) {
            cb(old_state, new_state);
        }
    }

    std::atomic<DeviceState> current_state_{kDeviceStateUnknown};
    std::mutex mutex_;
    std::vector<std::pair<int, StateCallback>> listeners_;
    int next_listener_id_{0};
};

#endif // DEVICE_STATUS_H
