#pragma once

class SystemReset final {
public:
    // 等待指定毫秒后重启；传 0 时立即重启。
    static void Restart(int delay_ms = 0);
    static void ResetToFactory();

private:
    SystemReset() = delete;

    static void ResetNvsFlash();
};
