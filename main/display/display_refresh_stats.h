#pragma once

#include <algorithm>
#include <cstdint>

// 只在 LVGL 任务中使用；统计真实绘制而非空刷新定时器，耗时包含调度等待。
class DisplayRefreshStats {
public:
    struct Report {
        uint32_t frames = 0;
        int64_t interval_us = 0;
        int64_t render_us = 0;
        int64_t max_render_us = 0;
        int64_t max_gap_us = 0;
        int64_t flush_wait_us = 0;
    };

    void BeginRender(int64_t now_us) {
        if (window_start_us_ < 0) window_start_us_ = now_us;
        if (last_render_us_ >= 0) {
            report_.max_gap_us = std::max(report_.max_gap_us, now_us - last_render_us_);
        }
        last_render_us_ = render_start_us_ = now_us;
    }
    void EndRender(int64_t now_us) {
        if (render_start_us_ < 0) return;
        const auto elapsed = now_us - render_start_us_;
        ++report_.frames;
        report_.render_us += elapsed;
        report_.max_render_us = std::max(report_.max_render_us, elapsed);
        render_start_us_ = -1;
    }
    void BeginFlushWait(int64_t now_us) { flush_start_us_ = now_us; }
    void EndFlushWait(int64_t now_us) {
        if (flush_start_us_ >= 0) report_.flush_wait_us += now_us - flush_start_us_;
        flush_start_us_ = -1;
    }
    bool TakeReport(int64_t now_us, Report& report) {
        if (window_start_us_ < 0 || now_us - window_start_us_ < 5'000'000 || report_.frames == 0) {
            return false;
        }
        report_.interval_us = now_us - window_start_us_;
        report = report_;
        report_ = {};
        window_start_us_ = now_us;
        return true;
    }

private:
    int64_t window_start_us_ = -1;
    int64_t last_render_us_ = -1;
    int64_t render_start_us_ = -1;
    int64_t flush_start_us_ = -1;
    Report report_;
};
