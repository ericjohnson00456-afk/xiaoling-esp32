#pragma once

#include <cstddef>
#include <cstdint>

// 等待流式数据时不要空转；有数据时也不能无限抢占 CPU。
// EOF 必须保留原语义，不能通过额外的永久 GMF job 阻止自然结束。
class DecodeYieldPolicy {
public:
    static constexpr int64_t kIntervalUs = 50 * 1000;
    void Reset(int64_t now_us) { last_yield_us_ = now_us; }
    bool ShouldYield(int64_t now_us, size_t bytes, bool done,
                     int64_t interval_us = kIntervalUs) const {
        return !done && (bytes == 0 || now_us - last_yield_us_ >= interval_us);
    }
private:
    int64_t last_yield_us_ = 0;
};
