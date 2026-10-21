# 串口录制唤醒模型输入

适用于启用 `CONFIG_USE_MICRO_WAKE_WORD` 且配置了 TF 卡的固件。
在串口终端中输入命令并发送回车：

```text
wake-record start
wake-record status
wake-record stop
```

`start` 自动挂载 TF 卡，在 `/sdcard/recordings/` 下创建
`wake-00001.wav`、`wake-00002.wav` 等新文件，不覆盖已有录音。
`status` 显示路径、已写入的 PCM 字节数、音频时长、排队样本数和错误。
`stop` 等待排队音频写完，补齐 WAV 长度并关闭文件；命令返回后再拔卡。
请在断电或重启前执行 `stop`，否则 WAV 文件头可能未完成。

文件为 16 kHz、16 位有符号小端 PCM、单声道 WAV，每秒 PCM 占 32,000 字节。
取样点在 `MicroWakeWord::GenerateFeaturesForWindow` 中，紧接唤醒输入环形缓冲区
读取之后、VAD 和音频特征提取之前。保存的是模型实际消费的处理器输出，
包含静音与被 VAD 拒绝的候选音频，不是原始麦克风/回声参考双通道数据。

录制命令不启停唤醒模型。模型暂停、设备对话期间未喂唤醒帧时，不会产生音频，
恢复喂帧后继续写入同一文件；暂停时间不补静音。唤醒队列在取样前丢弃的帧也不
在录音中，因此 WAV 时长是模型消费的音频时长，不一定等于真实经过的时间。

独立低优先级任务写卡，模型线程只复制 PCM 到 PSRAM 缓冲区。
缓冲区预算为两秒音频；如果 TF 卡写入过慢导致缓冲区满，自动停止并尽量保存
此前连续接收的音频，在日志和 `status` 中报告错误，不静默丢帧继续录制。
达到 WAV/FAT32 文件大小上限时也会停止。卡满、拔卡、写入/收尾失败会报告错误；
物理拔卡后的文件完整性无法保证。

主机测试（项目根目录）：

```powershell
& 'D:\Dev\Language\C\LLVM\bin\clang++.exe' -std=c++17 -Wall -Wextra -Werror -I main tests/audio/test_wake_recording.cc -o tmp/test_wake_recording.exe
& '.\tmp\test_wake_recording.exe'
```

测试覆盖环形缓冲区顺序/回绕、溢出前完整音频保留、WAV 字段/长度回填和大小边界。
实机仍需验证：连续录音与回放、重复开始/停止、无卡/卡满、录制中唤醒/暂停/恢复，
并检查录制期间唤醒吞吐与丢帧日志。
