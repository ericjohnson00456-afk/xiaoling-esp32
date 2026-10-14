#pragma once

#include <atomic>
#include <cstdint>

namespace AppEvent {

// 队列事件和二次调度任务可携带同一代次；Advance 后旧工作不再生效。
// 被事件引用的 Epoch 必须比事件队列活得更久（例如静态对象）。
class Epoch {
public:
    uint32_t Current() const { return generation_.load(); }
    bool IsCurrent(uint32_t generation) const { return generation == Current(); }
    uint32_t Advance() {
        uint32_t previous = generation_.load();
        uint32_t next;
        do {
            next = previous + 1;
            if (next == 0) next = 1;
        } while (!generation_.compare_exchange_weak(previous, next));
        return next;
    }

private:
    std::atomic<uint32_t> generation_{1};
};

struct EpochSnapshot {
    const Epoch* epoch = nullptr;
    uint32_t generation = 0;
    bool IsCurrent() const { return epoch == nullptr || epoch->IsCurrent(generation); }
};

}  // namespace AppEvent
