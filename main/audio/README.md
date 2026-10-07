# 音频服务

`AudioService` 是整机唯一的音频硬件服务。它长期持有 codec、音频处理器、
唤醒模型和 Opus 编解码器，并维护唯一的 ADC 采集链和 DAC 输出链。

它不决定当前是在对话、播放音乐还是响闹钟，也不处理播放通道之间的抢占关系。

## 职责边界

- `AudioService`：ADC/DAC、PCM 队列、重采样、AFE/AEC/VAD、唤醒检测和
  Opus 编解码。
- `AudioOutput`：`AudioService` 对播放器暴露的最小 PCM 输出接口。
- `AudioPlayer`：统一管理 TTS、tone、music、alarm 通道、播放列表和播放事件。
- `EspSimplePlayerBackend`：网络拉流、音频解码、暂停和恢复底层播放流。
- `PlaybackIntentManager`：由 `Application` 持有，决定语音、音乐和闹钟之间的
  抢占及恢复关系。
- `Application`：连接音频回调、`AppEvent`、小聆会话、播放器和 UI。

因此业务模块不直接写 DAC，也不直接操作 `AudioService` 的内部任务和队列。

## 初始化和生命周期

`Application::InitializeAudio()` 在启动阶段执行一次：

1. `AudioService::Initialize(codec)` 装配基础音频链。
   - 启动硬件 codec。
   - 加载唤醒模型资源。
   - 创建 Opus 编解码器和输入重采样器。
   - 创建 `AudioProcessor`，安装 PCM 输出和 VAD 回调并完成初始化。
   - 创建音频硬件空闲计时器。
2. 初始化 `AudioPlayer` 和 `PlaybackIntentManager`；播放器在此绑定底层 DAC
   输出回调。
3. 注册 TTS URL、播放器生命周期、DAC 输出、唤醒词和 VAD 的 `AppEvent`
   订阅者。
4. 绑定 `AudioService` 的唤醒/VAD、Opus 上行回调，以及播放器接纳 TTS URL
   后的通知回调。
5. `AudioService::Start()` 启动采集、输出、Opus 和必要的唤醒旁路任务。
6. 打开唤醒词检测。

`Start()` 之后，ADC 采集、AFE/AEC 和 Opus 编码常驻运行。普通业务状态切换
只改变数据是否被消费，不启停这些任务。固件 OTA 可以调用 `Stop()/Start()`
暂停并恢复整个音频服务。

AEC 能力由板型配置和 codec 参考声道共同决定。硬件支持时默认开启，随后由
小聆会话的单工、半双工或全双工策略决定是否继续使用。会话模式不属于
`AudioService` 的初始化职责。

## 采集和上行

```text
AudioCodec ADC
    -> AudioInputTask（读取麦克风和参考声道，转换为 16 kHz PCM）
    -> AudioProcessor（AFE/AEC/VAD；不支持 AFE 时直通麦克风 PCM）
    -> audio_encode_queue_（最多 8 帧，满时丢最旧帧）
    -> OpusCodecTask（16 kHz Opus）
    -> opus_packet_queue_（只保留最近 2.4 秒）
    -> 小聆网络上行队列
```

云端上行主要使用两个接口：

- `EnableAudioUpload(true/false)`：控制网络是否消费新 Opus，不影响采集、
  AFE/AEC 和编码任务。
- `PopOpusPacket()`：取出待上报的 Opus 包；未打开上报时返回空。

打开一次新的上报会话时会清除旧的 Opus 窗口，从当前声音开始发送，避免误发
唤醒前缓存。网络不消费时，固定队列覆盖最旧包，不会反向阻塞实时采集。

Opus 包就绪通知直接交给网络消费，不经过 `AppEvent`。录音测试另外使用
`EnableAudioTesting()`；少数板级功能可以通过 `ReadAudioData()` 读取标准化 PCM。

## 唤醒和 VAD

唤醒检测是采集链的旁路消费者：

- MicroWakeWord 在 AEC 开启时消费 `AudioProcessor` 输出；未使用 AEC 时由
  `AudioInputTask` 直接提供标准化麦克风 PCM。
- ESP-SR 根据芯片和模型包选择 AFE、自定义命令词或直接 Wakenet 实现，
  输入帧均由 `AudioInputTask` 提供。

`AudioService` 只触发唤醒词和 VAD 回调。`Application` 将它们转换为
`WakeWordDetected` 和 `VadChanged` 事件，再由统一的 `AppEvent` 任务分发。

## 播放和下行

播放器 URL/本地资源链：

```text
Application / PlaybackIntentManager
    -> AudioPlayer（选择 TTS、tone、music、alarm 通道）
    -> EspSimplePlayerBackend（拉流和解码）
    -> AudioOutput::PushPcm()
    -> AudioService::audio_playback_queue_
    -> AudioOutputTask
    -> AudioCodec DAC
```

服务端实时 Opus 下行链：

```text
服务端 Opus
    -> AudioService::PushPacketToDecodeQueue()
    -> OpusCodecTask
    -> audio_playback_queue_
    -> AudioOutputTask
    -> AudioCodec DAC
```

所有音源最终经过同一个 `AudioOutputTask` 写入 DAC，避免多个模块并发操作扬声器。
播放器 PCM 会在 `AudioService` 中完成采样率转换、首播预缓冲和尾帧处理。

音乐被 TTS 或提示音抢占时，播放器后端保留网络流和解码位置，
`AudioService::SuspendPlayback()` 暂存已经解码但尚未送到 DAC 的 PCM；恢复时先
放回这些 PCM，再从原来的流位置继续播放。

## 播放事件

- 播放器后端事件先进入 `AudioPlayer`，由它维护通道状态，再发布统一的
  `Playback*` AppEvent。
- PCM 真正写入 codec 后，`AudioService` 通过输出回调通知 `AudioPlayer`。
- `AudioPlayer` 为输出通知补充 TTS、音乐、闹钟等通道信息，再发布
  `PlayerOutputStarted` 或合并后的 `PlayerOutputProgress` 事件。

`AudioService` 不直接发布播放器业务事件，也不识别当前 PCM 属于哪个业务意图。

## 队列原则

- 实时采集永不等待业务消费者，积压时丢弃旧数据。
- 上行 Opus 只保留固定时间窗口，避免网络停顿耗尽内存。
- 播放队列按调用参数等待，保证已接纳的音频可以顺序输出。
- 播放器的较长 PCM 缓冲使用 PSRAM，实时采集和 AFE 数据保持短队列。
- TF 本地音乐按 PCM 字节数使用低/高水位反压，避免共享 SPI 启动阶段持续
  超前读取；网络 TTS 保留独立的深缓存以吸收云端生成停顿。
- 任务只由 `Initialize()/Start()/Stop()` 管理，业务代码不直接操作任务句柄。

## 目录

```text
audio/
├── audio_service.*       音频硬件服务和固定数据链
├── audio_output.h        播放器使用的最小物理输出接口
├── codecs/               硬件 codec 基类及板级实现
├── processors/           AFE/AEC/VAD 和直通处理器
├── wake_words/           唤醒词接口及各类实现
└── opus_encoder/         16 kHz 上行 Opus 编码封装
```
