# microWakeWord 唤醒候选的人声过滤

当前 `CONFIG_USE_MICRO_WAKE_WORD` 路径接入 ESPHome 官方 v2 神经网络 VAD。
它替换此前独立 WebRTC VAD 的候选过滤，不改变 AFE VAD 的其他用途。
这是可编译的实验接入；离线结果显示默认配置仍会漏掉部分加噪女声，不能声称已解决误唤醒。

## 模型与判定

官方来源：
[models/v2/vad.json](https://github.com/esphome/micro-wake-word-models/blob/9a72109cc935f77e34cf8ffe7b868155c37ec201/models/v2/vad.json)。
模型、描述文件和 Apache-2.0 许可证保存在
`main/audio/wake_words/micro_wake_word/models/vad/`，`SOURCE.json` 记录固定提交和 SHA256。

- 上游提交：`9a72109cc935f77e34cf8ffe7b868155c37ec201`。
- `vad.tflite`：34328 字节，SHA256 `7aa4db6d5fb7c5358609f6931e7847d303c16a43d178638bc14104f50d7eff5f`。
- 默认阈值 **0.5**，取最近 **5 次评分的平均值**，严格大于阈值才通过，不能用最大值代替。
- 与 ESPHome 2026.9.1 对齐，阈值先转为 `uint8_t(0.5 * 255) = 127`。
  本模型输出 scale=1/256、zero_point=0，因此日志中的反量化有效阈值为 **0.49609375**。
- 输入 `[1,3,40]`：10 ms 特征步长，每 30 ms 得到一次评分，五次评分的历史跨度约 150 ms。
  没有另加 WebRTC 480 ms 人声保留期，也不等待唤醒之后再录一段人声。
- 描述文件标注 `trained_languages: [en]`；不能据此保证中文、轻声或所有音色上的泛化。

CMake 从上游描述文件生成 `model_vad.h` 中的默认常量，将 VAD 权重嵌入应用。
替换唤醒词资源分区不会替换该 VAD；VAD 变更需要重新编译应用。
VAD tensor arena 为 16772 字节，另有 1024 字节资源变量 arena，以及 interpreter、评分等开销；
16772 不是总 RAM 占用。两个 arena 按现有策略分配在 PSRAM。

## 连续推理和生命周期

- 16 kHz 单声道 PCM 经同一个前端生成特征，唤醒模型和 VAD 都持续推理。
  VAD 不控制唤醒模型能否收到音频，二者特征步长必须一致。
- 只有唤醒模型产生新评分、窗口平均分达标后，才检查 VAD 的窗口平均分。
- VAD 拒绝时保留唤醒评分和流式状态，下一次新评分继续检查，不触发已接受唤醒的冷却或复位。
- VAD 通过后才启动 BEFORE20/AFTER20 诊断评分采集并上报唤醒事件。
  保留原有 AFTER20 采集结束后复位行为。
- 启动、输入不连续或 generation 改变、采集结束后复位时，同时重置前端、两个模型和评分历史。
  停止时卸载两个模型。
- VAD 加载、复位、推理失败时停止检测并报错，不使用旧 VAD 分数放行。
- 原 `wake_speech_gate.h` 与其测试保留作历史实现，当前运行路径不再实例化该 WebRTC gate。

## 日志

启动日志含 `VAD backend=esphome-neural`、`cutoff`、`effective`、`window`、`score_step`。
候选日志为 `Wake VAD ACCEPT/REJECT`，同时记录 `wake_avg`、`vad_avg` 和 `pcm_sample_ms`。
样本时钟在输入复位后从零开始；连续被拦截的同一段候选只记录首次 REJECT，避免每 30 ms 刷屏。

`Wake PCM discontinuity` 表示输入丢帧/会话变化，前端与两个模型历史都将重建。
VAD 识别语音活动，电视、其他人说话及某些噪声仍可能通过；它不能确认说的是正确唤醒词。

## 2026-10-21 离线验证

验证脚本：`C:/Users/tanha/Documents/Playground/wake_vad_review_20261021/evaluate_neural_gate.py`。
完整逐文件结果：同目录 `neural_vad/evaluation.json`；可读报告 `neural_vad/report.html`。

回放使用当前 v12 权重 SHA256
`09bf1757bc7a97e21f4bdeee1089b7bcce7d97fb0e85522341a6766c9c04ac2a`，
唤醒阈值 0.85、窗口 15，ESPHome 前端四舍五入（+333），TFLite Micro 主机内核。
镜像当前固件的 74 个特征预热、仅新评分判定、接受后 AFTER20 以及输入复位。
这不是完整 ESP32 硬件模拟：没有复现 ESP-NN Xtensa 内核、任务时序、队列丢帧或实际扬声器响应。

| 数据 | 不加 VAD | 加官方默认 VAD | 解释 |
|---|---:|---:|---|
| 用户轻声录音 `wake-00001.wav`，22.68 秒 | 3 次事件 | 3 次事件 | 3.77 / 6.61 / 11.94 秒，均通过；事件数不是整段说话次数 |
| 已用于补训的原始女声录音，7.01 秒、三次唤醒词 | 3 次事件 | 3 次事件 | 已见说话人，不能作为独立测试 |
| 同一女声三次短语派生的 81 个变速/音量/噪声文件 | 67/81 文件有事件 | 60/81 文件有事件 | VAD 额外漏掉 7 个文件；不是 81 个独立说话人 |
| 4 个已见 TTS 音色文件（2 女、2 男） | 3/4 文件有事件 | 3/4 文件有事件 | 仅为小规模回归检查 |
| noise_test 目录的 4 个已见环境文件，共 327.42 秒 | 2 次误事件 | 2 次误事件 | 两个人群交谈文件各一次，未降低事件总数 |
| 曾用于噪声训练的地铁/办公室/车站 3 个文件，共 900.01 秒 | 0 次事件 | 0 次事件 | 已见噪声，仅作回归 |

女声额外漏检文件：

- `female_1_speed0.85_babble_gain-6_snr+5.wav`
- `female_1_speed1.00_babble_gain-6_snr+5.wav`
- `female_1_speed1.15_metro_gain-6_snr+5.wav`
- `female_2_speed0.85_metro_gain-6_snr+10.wav`
- `female_3_speed0.85_metro_gain-6_snr+5.wav`
- `female_3_speed1.00_metro_gain-6_snr+5.wav`
- `female_3_speed1.15_metro_gain-6_snr+10.wav`

`Babble_1.wav` 的误事件发生在 29.39 秒，VAD 平均分 0.9961，直接通过。
`Babble_2.wav` 的 3.56 秒候选被拒，但 5.66 秒仍触发误事件；拦下一次候选不等于消除该段误唤醒。
这七个环境文件均出现在 v12 的 augmentation_record.json 中，包含 noise_test 目录的四个文件，均属已见训练背景。目录名不能作为独立测试的证据。约 20.46 分钟的混合环境回放不足以估计长期部署误唤醒率，也没有设备上曾误触发的纯背景录音。

结论：官方 VAD 可以接入并能保留这两段原始真人录音的当前候选，但默认 0.5 对部分低信噪比女声仍偏严，
本次样本不能证明它降低了总体误唤醒。继续调门限应同时看女声漏检和人群交谈负样本；
唤醒模型本身没有产生候选时，VAD 无法补回。

旧串口日志的设备模型为 0.88/窗口 10，当时 WebRTC gate 14 次候选中拒绝 12 次；
本次工作区是 v12 的 0.85/窗口 15，不能把两组事件直接当成同一模型的前后唤醒率。
本次 VAD 接入未修改唤醒权重或描述文件。

## 构建和测试

- `tests/audio/test_vad_probability_policy.cc`：平均值与最大值的区别、零填充、严格阈值边界，
  并用 4096 次滚动更新对照 ESPHome 原始 uint8 求和规则。
- `tests/audio/test_wake_score_trace.cc`：接受事件的前后评分采集与复位回归。
- 两项 MSVC C++17 `/W4 /WX` 主机测试通过。
- ESP-IDF 5.4.4、M5Stack Core S3 / ESP32-S3 当前配置构建和打包通过；
  包位于 `build/m5stack-core-s3-xiaoling_v2.0.0.zip`。
- 构建仍报告已有的重复 Kconfig 符号诊断，未出现本次代码的编译错误。
- 用户随后已烧录并反馈主观体验“还不错”。尚无定量设备测试记录，设备 RAM、实时推理耗时、不同男女声召回和长期误唤醒表现仍待测量。
