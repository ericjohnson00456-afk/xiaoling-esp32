#include "display/display_refresh_stats.h"

#include <cassert>
#include <cstdio>

int main() {
    DisplayRefreshStats stats;
    DisplayRefreshStats::Report report;
    assert(!stats.TakeReport(6'000'000, report)); // 空刷新不计为绘制帧。
    stats.EndRender(0); // 未开始的绘制不计数。
    stats.EndFlushWait(0);
    stats.BeginRender(0);
    stats.BeginFlushWait(1000);
    stats.EndFlushWait(3000);
    stats.EndRender(10'000);
    assert(!stats.TakeReport(4'999'999, report));
    stats.BeginRender(100'000);
    stats.EndRender(120'000);
    assert(stats.TakeReport(5'000'000, report));
    assert(report.frames == 2);
    assert(report.interval_us == 5'000'000);
    assert(report.render_us == 30'000);
    assert(report.max_render_us == 20'000);
    assert(report.max_gap_us == 100'000);
    assert(report.flush_wait_us == 2000);
    assert(!stats.TakeReport(10'000'000, report)); // 上次报告的帧不会重复上报。
    stats.BeginRender(10'000'000);
    stats.EndRender(10'005'000);
    assert(stats.TakeReport(10'005'000, report));
    assert(report.frames == 1 && report.render_us == 5000);
    assert(report.flush_wait_us == 0);
    std::puts("display refresh: real frames, wall time, DMA waits and report reset passed");
}
