#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace micro_wake_word {

// 触发分数单列，前后组各记录最多 20 次真正的模型推理输出。
class WakeScoreTrace {
public:
    static constexpr size_t kCount = 20;

    void Reset() {
        history_next_ = 0;
        history_count_ = 0;
        before_count_ = 0;
        after_count_ = 0;
        collecting_ = false;
    }

    // 收满后组时返回 true；触发当次在 Begin() 前已经 Push，不计入后组。
    bool Push(float score) {
        bool complete = false;
        if (collecting_) {
            after_[after_count_++] = score;
            if (after_count_ == kCount) {
                collecting_ = false;
                complete = true;
            }
        }
        history_[history_next_] = score;
        history_next_ = (history_next_ + 1) % history_.size();
        history_count_ = std::min(history_count_ + 1, history_.size());
        return complete;
    }

    bool Begin() {
        if (collecting_ || history_count_ == 0) return false;
        ++id_;
        before_count_ = history_count_ - 1;
        const size_t first =
            (history_next_ + history_.size() - history_count_) % history_.size();
        for (size_t i = 0; i < before_count_; ++i) {
            before_[i] = history_[(first + i) % history_.size()];
        }
        trigger_ = history_[(history_next_ + history_.size() - 1) % history_.size()];
        after_count_ = 0;
        collecting_ = true;
        return true;
    }

    bool Collecting() const { return collecting_; }
    uint32_t Id() const { return id_; }
    float Trigger() const { return trigger_; }
    size_t BeforeCount() const { return before_count_; }
    const auto& Before() const { return before_; }
    const auto& After() const { return after_; }

private:
    std::array<float, kCount + 1> history_{};
    std::array<float, kCount> before_{};
    std::array<float, kCount> after_{};
    size_t history_next_ = 0;
    size_t history_count_ = 0;
    size_t before_count_ = 0;
    size_t after_count_ = 0;
    uint32_t id_ = 0;
    float trigger_ = 0.0f;
    bool collecting_ = false;
};

}  // namespace micro_wake_word
