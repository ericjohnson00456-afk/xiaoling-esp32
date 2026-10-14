#include "event/app_event.h"

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    AppEvent::Epoch epoch;
    const uint32_t first = epoch.Current();
    AppEvent::Event old{.id = AppEvent::Id::XiaolingTtsUrl,
                        .epoch = &epoch, .generation = first};
    AppEvent::Event global{.id = AppEvent::Id::XiaolingConnected};
    assert(old.IsCurrent() && global.IsCurrent());
    const uint32_t next = epoch.Advance();
    assert(next != first && next != 0);
    assert(!old.IsCurrent() && global.IsCurrent());
    AppEvent::Event fresh{.id = AppEvent::Id::XiaolingIatStart,
                          .epoch = &epoch, .generation = next};
    assert(fresh.IsCurrent());
    // 同一检查也保护从事件回调二次投递到主任务的 finish/IAT/TTS 工作。
    auto delayed_finish = [&epoch, next]() { return epoch.IsCurrent(next); };
    epoch.Advance();
    assert(!delayed_finish() && !fresh.IsCurrent());
    const auto before = epoch.Current();
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&epoch]() {
            for (int j = 0; j < 1000; ++j) epoch.Advance();
        });
    }
    for (auto& thread : threads) thread.join();
    assert(epoch.Current() == before + 4000);
    std::puts("event epoch: stale replies, deferred work, global events, concurrency passed");
}
