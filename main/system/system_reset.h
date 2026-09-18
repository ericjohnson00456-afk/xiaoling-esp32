#pragma once

class SystemReset final {
public:
    static void ResetToFactory();

private:
    SystemReset() = delete;

    static void ResetNvsFlash();
    static void RestartInSeconds(int seconds);
};
