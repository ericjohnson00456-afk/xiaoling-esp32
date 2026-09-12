#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

// 可被多个 Presenter 同时订阅的线程安全 Model 基类。
template <typename State>
class ObservableModel {
public:
    using SubscriptionId = uint32_t;
    using Observer = std::function<void(const State&)>;

    State Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    SubscriptionId Subscribe(Observer observer, bool notify_immediately = true) {
        if (!observer) {
            return 0;
        }

        SubscriptionId id;
        State snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = next_id_++;
            observers_.push_back({id, observer});
            snapshot = state_;
        }
        if (notify_immediately) {
            observer(snapshot);
        }
        return id;
    }

    void Unsubscribe(SubscriptionId id) {
        if (id == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        observers_.erase(
            std::remove_if(observers_.begin(), observers_.end(),
                           [id](const Subscription& item) { return item.id == id; }),
            observers_.end());
    }

protected:
    void Publish(State state) {
        std::vector<Observer> observers;
        State snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = std::move(state);
            snapshot = state_;
            observers.reserve(observers_.size());
            for (const auto& item : observers_) {
                observers.push_back(item.observer);
            }
        }
        for (const auto& observer : observers) {
            observer(snapshot);
        }
    }

private:
    struct Subscription {
        SubscriptionId id;
        Observer observer;
    };

    mutable std::mutex mutex_;
    State state_{};
    std::vector<Subscription> observers_;
    SubscriptionId next_id_ = 1;
};
