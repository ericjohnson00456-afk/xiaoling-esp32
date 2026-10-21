# microWakeWord 唤醒事件的人声过滤

当前 `CONFIG_USE_MICRO_WAKE_WORD` 路径在候选达标后检查独立的 ESP-SR
WebRTC VAD。只有近期存在人声的候选，才会调用 `on_wake_word_detected`，
进入应用唤醒流程。VAD 不控制音频是否送入唤醒模型，模型始终连续推理。

## 音频与时序

- 使用处理器输出、单声道 16 kHz PCM，和模型输入一致。
- VAD 随前端消费的音频同步推进，每 10 ms 分析一次，不读取可能领先于
  唤醒队列的 AFE `voice_detected_` 状态。
- 使用 `VAD_MODE_0`，即本项目 ESP-SR 头文件注明的最宽松模式，优先保留轻声。
- 近期人声保留时长由 `sliding_window_size * feature_step_size + 30 ms` 得到。
  当前窗口 15、步长 10 ms，对应 180 ms；用途是容纳评分平滑和前端窗口延迟，
  并非经真人评测确定的最佳阈值。VAD 自身的判定延续也会影响实际行为。
- 保留期按已消费 PCM 的样本数计算，不随任务调度、队列等待改变。
- 队列帧序号不连续或 generation 改变时，清除模型、前端和 VAD 历史。
  启停与输入复位也会重新创建 VAD，清除内部判定延续状态。
- 独立 VAD 不依赖 AFE VAD 开关，AEC 模式关闭 AFE VAD 时仍执行检查。

## 日志与异常

- `wake VAD gate enabled`：启动成功，打印模式、分析帧长和保留期。
- `Wake VAD ACCEPT`：候选附近有 VAD 人声结果，允许上报。
- `Wake VAD REJECT`：近期未检测到人声，拦截该候选。清除候选评分和诊断 trace，
  保留连续推理状态，不进入接受唤醒后的冷却/复位流程。
- `Wake PCM discontinuity`：唤醒输入丢帧/会话变化，重建检测历史。
- VAD 创建失败时停用唤醒检测并报错，不绕过检查放行。

## 验证边界

`tests/audio/test_wake_speech_gate.cc` 用确定性分类器替身验证分帧、近期人声
过期、复位、分配失败和 PCM 顺序；不代表真实 VAD 的声学成绩。
在 Visual Studio 开发者命令行中，可在项目根目录执行：

```bat
cl /nologo /utf-8 /std:c++17 /EHsc /W4 /WX /I main/audio/wake_words/micro_wake_word /I managed_components/espressif__esp-sr/include/esp32s3 tests/audio/test_wake_speech_gate.cc /Febuild/test_wake_speech_gate.exe /Fobuild/test_wake_speech_gate.obj
build\test_wake_speech_gate.exe
```

需要在设备上对比原有男女声、轻声/远距离录音，以及曾误触发的纯背景噪声，
记录 ACCEPT/REJECT 与实际唤醒次数，才能判断收益和漏唤醒变化。
VAD 识别的是语音活动：电视、旁人说话及某些噪声仍可能通过；它不能鉴定
是否由现场真人说出正确唤醒词。
