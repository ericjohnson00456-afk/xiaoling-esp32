# 应用事件回调整理计划

本文档用于记录应用事件循环和音频回调的迁移步骤。每个阶段必须独立编译；涉及任务上下文或音频时序变化时，必须烧录并观察日志后再进入下一阶段。

## 总体边界

- `app_main`：负责初始化、注册订阅和启动模块，不长期承载业务回调。
- `app_event`：优先级为 6，串行处理低频业务控制事件。
- 音频、播放器和网络任务：继续处理实时数据及可能阻塞的工作。
- UI 任务：只执行界面更新。
- 事件订阅使用函数指针和 `context`，不依赖捕获式回调。
- 发布成功的事件会调用全部订阅者；回调不得阻塞。

## 明确保留原执行通道的回调

- Opus 包上行：`on_opus_packet_available`。
- PCM 输入输出、AFE、Opus 编解码和 WebSocket 音频包。
- HTTP 下载、图片下载、音乐 URL 解析等阻塞操作。
- 播放进度不逐块入队，只更新最新值或使用可合并事件。

## 阶段一：应用事件循环基础设施

- [x] 新增 `main/event/app_event.h` 和 `main/event/app_event.cc`。
- [x] 使用函数指针、订阅 ID 和调用者 `context`。
- [x] 建立普通、紧急两个 FIFO 队列。
- [x] 事件发布时复制 payload，发布者返回后数据仍然有效。
- [x] 创建优先级 6 的 `app_event` 任务。
- [x] 增加队列满、订阅表满、回调超时日志。
- [x] 在音频和网络任务启动前初始化事件循环。
- [x] 编译、烧录并确认事件任务启动正常。

## 阶段二：播放器生命周期事件

- [x] GMF 回调只发布内部 `PlaybackBackendEvent`。
- [x] `AudioPlayer` 在 `app_event` 任务中更新通道状态。
- [x] 音乐自然结束时由 `AudioPlayer` 自动下一首。
- [x] 保留音乐 URL 失败重试逻辑。
- [x] 发布 StreamReady、Started、Paused、Stopped、Completed、Failed 公共事件。
- [x] Application 先使用一个统一处理器承接现有 TTS 逻辑。
- [x] 删除播放回调中的 `Application::Schedule()`。
- [x] 编译、烧录并验证启动 tone。
- [ ] 交互验证 TTS、音乐自动下一首和音乐抢占恢复。

当前验证记录：增量编译和应用分区烧录通过；启动 tone 正常完成，`app_event`
任务剩余栈约 6.2 KiB，未出现队列满、慢回调或崩溃。云端更新后的 wake-word
资源暂时与 ESP32 格式不兼容，本轮不把唤醒词作为阻塞项。串口没有播放测试命令，
因此 TTS、音乐自动下一首及抢占恢复仍需通过按键或小程序交互验证。

## 阶段三：拆分播放事件订阅者

- [x] Application 只维护 TTS 会话和设备状态。
- [x] UI 订阅播放开始或首帧输出事件，更新说话状态和表情。
- [x] UI 在播放结束后按 `DeviceStatus` 恢复显示，不自行决定业务状态。
- [x] AlarmService 直接订阅 Alarm 通道终态事件。
- [x] 删除 `SetAlarmEventCallback()`。
- [x] 编译、烧录并验证启动、联网和闹钟同步流程。
- [ ] 交互验证 TTS、音乐和闹钟播放；唤醒提示音暂不验收。

当前验证记录：编译和应用分区烧录通过；启动 tone、联网、WebSocket 连接和闹钟
云端同步正常，未出现事件队列错误或崩溃。TTS、音乐和闹钟播放仍需交互触发验证；
唤醒提示音因云端 wake-word 资源暂不兼容而跳过。

## 阶段四：AudioService 控制事件

- [x] 唤醒词发布紧急 `WakeWordDetected` 事件。
- [x] VAD 发布可合并的 `VadChanged` 事件。
- [x] 第一段播放器 PCM 发布 `PlayerOutputStarted` 事件。
- [x] 播放进度改为原子最新值或可合并事件。
- [x] 首帧和进度事件携带 `session_id` 与 `PlaybackChannel`，订阅者按通道处理。
- [x] `on_opus_packet_available` 保持直接交接 Opus 包。
- [x] 删除对应的 `MAIN_EVENT_WAKE_WORD_DETECTED` 和 `MAIN_EVENT_VAD_CHANGE`。
- [x] 编译、烧录并验证事件任务、播放器首帧和播放终态。
- [ ] 交互验证 VAD、全双工、半双工和 IAT 打断；唤醒词暂不验收。

当前验证记录：编译和应用分区烧录通过；启动 tone 的首段 PCM 与播放终态正常，
`app_event` 剩余栈约 5.8 KiB，未出现队列满、慢回调或崩溃。唤醒词测试按约定
跳过，其余语音交互模式仍需交互触发验证。

## 阶段五：简化音频初始化

- [x] 新增 `Application::InitializeAudio()`。
- [x] 新增 `BindAudioEventPublishers()`。
- [x] 新增 `RegisterAudioEventSubscribers()`。
- [x] 先完成订阅和回调绑定，再启动 AudioService 任务。
- [x] `SetTtsUrlCallback()` 只在初始化时注册一次。
- [x] 将实现集中到 `main/application_audio.cc`。
- [x] 清理 Application 初始化中的大段音频回调。
- [x] 编译、烧录并验证启动 tone、联网、WebSocket、MCP 和闹钟同步。
- [ ] 交互完成 TTS、音乐自动下一首、抢占恢复和闹钟播放回归。

当前验证记录：完整编译和应用分区烧录通过；启动后音频任务、`app_event`、UI、
网络及协议任务正常，启动 tone 的首段 PCM 和自然结束事件正常，`app_event` 剩余栈
约 5.8 KiB，未出现事件队列满、慢回调或崩溃。wake-word 资源与 ESP32 暂不兼容，
按约定不纳入本轮验收。

## 阶段六：音频采集链常驻

- [x] AFE/AEC 在 `AudioService::Initialize()` 中只初始化一次。
- [x] `Start()` 后 ADC、AFE/AEC 和 Opus 编码链保持运行。
- [x] 删除会反复启停处理器的 `EnableVoiceProcessing()`。
- [x] `EnableAudioUpload()` 只控制网络是否消费 Opus。
- [x] Opus 使用固定的 2.4 秒最新窗口，队列满后覆盖旧包且不阻塞采集。
- [x] 新上报会话清理历史窗口，避免误发唤醒前音频。
- [x] 重复调用 `Start()` 不会重复创建任务。
- [x] 删除当前协议不可用的服务端 AEC 配置、时间戳队列和数据字段。
- [x] AudioService 自行判断 AEC 能力，支持的板型默认开启。
- [x] Application 不再保存 AEC 状态；小聆全/半双工策略按需通知 AudioService。
- [x] 删除 AudioService 事件标志组；处理链固定常驻，唤醒和录音测试仅使用原子开关。
- [x] 使用中文重写 `main/audio/README.md`，记录采集链与播放链边界。
- [x] 完整编译通过。
- [ ] 烧录并检查空闲、聆听、TTS 和音乐期间的 CPU、内存及音频日志。

当前验证记录：ESP-IDF 5.4 完整编译通过，固件大小 `0x3a71c0`，最小应用分区
剩余 `0x48e40`（7%）。目标板对应的 `/dev/ttyACM0` 尚未恢复，仅检测到
`/dev/ttyUSB0`，因此未向不确定设备烧录。

## 后续阶段

音频事件稳定后，再用相同方式迁移网络、WebSocket、MCP 和 OTA。迁移完成前保留 `Application::Run()`、尚未迁移的 `MAIN_EVENT_*` 和 `Schedule()`。
