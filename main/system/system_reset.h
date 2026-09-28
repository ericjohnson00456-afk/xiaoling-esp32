#pragma once

namespace SystemReset {

// 等待指定毫秒后重启；传 0 时立即重启。
void Restart(int delay_ms = 0);
void ResetToFactory();

}  // namespace SystemReset
