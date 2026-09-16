#include "xiaoling/xiaoling_events.h"

#include <algorithm>
#include <utility>

XiaolingEventBus& XiaolingEventBus::GetInstance() {
    // 订阅者也是进程级单例；保持事件总线到固件进程结束，避免静态析构顺序问题。
    static auto* instance = new XiaolingEventBus();
    return *instance;
}

XiaolingEventBus::SubscriptionId XiaolingEventBus::Subscribe(
        XiaolingEventType type, Callback callback) {
    if (!callback) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = next_id_++;
    subscriptions_.push_back(Subscription{id, type, std::move(callback)});
    return id;
}

void XiaolingEventBus::Unsubscribe(SubscriptionId id) {
    if (id == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    subscriptions_.erase(
        std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                       [id](const Subscription& subscription) {
                           return subscription.id == id;
                       }),
        subscriptions_.end());
}

void XiaolingEventBus::Publish(XiaolingEventType type, std::string text) {
    std::vector<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& subscription : subscriptions_) {
            if (subscription.type == type) {
                callbacks.push_back(subscription.callback);
            }
        }
    }

    const XiaolingEvent event{type, std::move(text)};
    for (const auto& callback : callbacks) {
        callback(event);
    }
}
