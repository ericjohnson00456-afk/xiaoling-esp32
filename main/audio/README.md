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

`Start()` 之后，ADC 采集、AFE/AEC 和工作任务常驻运行；只有上报开启时
才执行上行 Opus 编码。普通业务状态切换不启停这些任务。固件 OTA 可以调用 `Stop()/Start()`
暂停并恢复整个音频服务。

AEC 能力由板型配置和 codec 参考声道共同决定；具备能力的设备在单工、
半双工、全双工及音乐/TTS 播放期间始终启用。会话模式只决定云端麦克风
上行策略，不关闭本地 AEC/唤醒处理链。

## 采集和上行

当前 ESP32-S3 的独立人声 AGC 已注释关闭，AEC、NS 与麦克风前置增益
仍按原参数运行。原 AGC 设计保留在代码中，恢复时需同时恢复初始化和
NS 后调用。

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

- `EnableAudioUpload(true/false)`：控制上行 Opus 编码/网络消费，不影响采集、
  AFE/AEC、NS、唤醒和下行解码。
- `PopOpusPacket()`：取出待上报的 Opus 包；未打开上报时返回空。

打开一次新的上报会话时会清除旧的 Opus 窗口，从当前声音开始发送，避免误发
唤醒前缓存。上报关闭时不生成无消费者的 Opus；上报开启而网络消费暂时
跟不上时，固定队列覆盖最旧包，不会反向阻塞实时采集。
上报启停都清理 PCM/Opus 缓存并推进 generation；编码线程在新 generation
的第一帧前复位编码器，在途旧帧不得进入新会话。
这避免空闲状态持续编码占用 BOX3 的大量 CPU、挤占降噪和唤醒预算。

Opus 包就绪通知直接交给网络消费，不经过 `AppEvent`。录音测试另外使用
`EnableAudioTesting()`；少数板级功能可以通过 `ReadAudioData()` 读取标准化 PCM。

配网状态下单击开始录音，再次单击停止；串口也可以执行
`test audio start` 和 `test audio stop`。录制最多 10 秒，
取样点为实际送入云端编码器的 AFE/AEC 输出。停止后在独立大栈任务中
生成同一段录音的两份版本，按「AFE PCM → 双短提示音 → Opus 往返 PCM」
顺序回放。测试 Opus 与云端使用相同的 16 kHz、20 ms、16 kbps CBR、
complexity 0 参数，但编码状态独立。日志同时报告 ADC MIC/REF 电平、
AFE 帧电平分位数、整段 RMS、峰值和削波比例。
采集任务依靠 I2S 等待数据自然阻塞，并每 100 ms 主动让出一个调度 tick，
不能在每个 10 ms 音频帧上再固定睡眠 10 ms。测试还记录录音墙钟时长、
ADC/AFE 采样时长及 ADC 覆盖率（超过录音上限时按 10 秒计算）；BOX codec
额外统计 I2S RX 队列溢出增量，其它未实现统计的 codec 返回 -1。
判断录音完整性应同时检查覆盖率和 RX 溢出，而不只检查回放采样率。
使用 I2S RX 的 codec 在 16 kHz 下报告 DMA 块的 240 点，由采集任务整块读取，
再按 AFE/唤醒模型要求拼帧；非 I2S 输入继续按消费者的帧长读取。这避免
I2S RX 队列接近满时跳过未读完的 DMA 尾部（此时溢出计数可能为 0）。
两段测试及分隔提示音回放时临时使用 100% 扬声器音量；等待 DMA 尾音播完
后恢复回放前的音量。取消回放或停止音频服务也会恢复，临时音量不写入 NVS。

模拟麦克风增益由各 codec 自行配置；BOX3 与 CoreS3 当前都是 30 dB。
ESP32-S3 上带参考声道的 codec 使用 ×2 前置 MIC PCM 增益，其它 codec
维持 ×5，参考声道始终不放大。当前处理顺序为可用的 AEC → NS →
测试回放/云端编码。保留但暂未启用的独立人声 AGC 设计为数字自适应模式：
内部压缩增益 18 dB、目标包络 -6 dBFS、limiter 开启；额外增益最多 12 dB，
输出峰值限制为 -1 dBFS。NS 后独立 VAD 原设计仅在人声阶段采用 AGC 输出，
并在人声结束时平滑退回旁路。后续重新启用时仍需近讲、远讲和 TTS 双讲测试。
前置放大时
超出 int16 范围会饱和裁剪，并输出 AEC 前削波诊断；NS 无法恢复已经
被削波的人声。
没有 NSNet 模型时，在
AFE/AEC 输出之后使用独立的 WebRTC NS（10 ms 帧，中等强度）；ESP-SR 2.2.2
的 VC AFE 直接接入 WebRTC NS 会在初始化时崩溃。AEC 消除本机回声，NS
抑制背景噪声。
ESP-BOX-3 启用设备端 AEC 的构建暂时跳过独立 WebRTC NS，以减轻全双工
播放时的 CPU1 负载；AEC 仍运行，麦克风背景噪声抑制能力需实机复测。
评估降噪时应先在无播放、无人讲话的条件下采集，再录制正常距离讲话样本；降低数字
增益会同时降低人声和噪声，不能单凭 RMS 下降认定信噪比改善。

## 唤醒和 VAD

唤醒检测是采集链的旁路消费者：

- MicroWakeWord 始终消费 `AudioProcessor` 输出，经有界短队列交给 CPU1
  上的独立任务推理，与 ADC feed 分核。启用 AFE 的 S3 输入
  为可用的 AEC → NS 后的 16 kHz PCM，
  与测试录音/云端编码的取样点相同；空闲、扬声器播放及尾音期间均不切回
  原始 MIC。关闭 AEC 不改变取样点，NS 继续运行；没有 AFE/NS 的其它板型
  则使用其现有处理器输出，并不会凭空获得降噪能力。
  ADC 采集任务不执行唤醒模型；队列仅在唤醒旁路丢弃过旧帧。
  唤醒任务为 9、AFE/NS 与编码为 8，网络播放器解码/输入为 6，避免高负载播放
  将唤醒推理持续压到无法及时执行；
  唤醒任务仍每 50 ms 主动让步一次，避免持续推理饿死 Idle。
  启停会清队列并更新 generation，推理与模型启停串行化，避免旧输入污染
  新一轮唤醒检测。其 TFLite Micro、ESP-NN 和特征提取组件版本固定为
  ESPHome 2026.9.1 的组合；本工程的 AFE/AEC 输入与任务调度仍独立维护。
  触发时日志以同一 `trace` 编号打印触发前 20 次真实推理评分和单列的
  `trigger` 评分，继续采满触发后 20 次评分再打印 `AFTER20`。采样期暂缓
  原有唤醒复位，收满后恢复；不修改模型阈值或滑窗判定规则。
- ESP-SR 根据芯片和模型包选择 AFE、自定义命令词或直接 Wakenet 实现，
  输入帧均由 `AudioInputTask` 提供。

`AudioService` 只触发唤醒词和 VAD 回调。唤醒直接投递应用紧急队列；
VAD 仍由 `AppEvent` 分发。播放器事件经事件队列后也在应用主任务处理，
避免提示音结束恢复暂存 URL 与新唤醒推进 epoch 并发。

本地模型的唤醒回调只直接投递 Application 紧急队列，不在模型锁内执行业务，
不等待事件任务分发。独立唤醒推理及运行期应用控制均为 9，高于编码/AFE 的 8。
启动/OTA/激活阶段的应用任务仍为 5。待命、录音、
TTS 播放期间均可唤醒替换对话：先推进 epoch、冻结旧回复游标并停止本地声音，
提交本地 tone，再按协议顺序发送旧会话 event/cancel 和新 rid 的 start。
旧 IAT/TTS/字幕、迟到 finish 与播放器终态均校验代次；不会关闭新录音。
started 确认改为异步，音频上报在当前新会话确认后开启，超时由一秒时钟检查。
无 AEC（含运行时关闭 AEC）的唤醒使用播放终态门控：tone 提交前关闭上报、
暂停重复唤醒，ADC 继续读，原始输入在进入处理器前替换为静音。读取前后的
保护代次不同也丢弃整块输入，防止短 tone 或解除保护跨过在途 ADC 读取。
PCM/Opus 缓存及在途编码在保护两端均被作废，不在提示音结束后补发。
等同一 epoch 的 tone 自然结束及 I2S DMA 排空后，才恢复唤醒并启动新录音
会话/超时计时；不固定或截断用户自定义 tone 的播放长度。缺失资源、播放
失败、取消、断网和模式切换均释放保护，旧完成回调不能开放新会话。
有 AEC 且已开启时保留提示音和录音并行，唤醒参数不变。
本地 ROMFS tone 不等 16 KiB 预缓冲，第一块 PCM 即输出；解码临时升为 8，
每 10 ms 让步一 tick，仍低于唤醒的 9。TTS 拉流/常态解码为 7，音乐为 6，
常态保留 50 ms 解码让步预算。TTS PCM 低于 8 KiB 时解码临时为 8，
并每 10 ms 让步一 tick；达到 16 KiB 后恢复为 7，与 LCD 同级轮转。
水位查询校验活动播放会话，音乐/提示音不使用这个 TTS 补缓冲策略。
AFE 后处理和 Opus 编码每 100 ms 最多让步
一个 tick，积压时也给媒体任务和 Idle 留出机会。AFE/编码使用共同的 tick
周期；每轮只让步一次，即使音频帧从未落在周期首 tick，也会给 Idle 和
媒体任务运行机会。S3 SPI LCD 渲染
优先级为 7，与 TTS 解码同级轮转，仍低于 AFE/编码的 8 和唤醒的 9。
LCD 使用两块 20 行 DMA 缓冲，总内存与原来的单块 40 行相同；等待 DMA
完成时阻塞而非忙等，让音频任务和 Idle 使用这段时间。显示内容和唤醒参数不变。
主动停止 TTS 时中止数据总线和 HTTP socket，取消 IO close 默认的 1000 ms
自然结束预等待（仍保留任务 deinit/join），并临时提高旧解码关闭任务优先级，
本地 tone 使用独立的后端控制/解码任务，不再等待旧 HTTP 流完全关闭。
所有后端的清队列、排空等待和 codec 进度均绑定播放 session，旧终态
不能清掉新 tone，也不能把旧输出时钟加到新会话。
停止还会撤销排队的旧播放命令；输出层记录最近启动 ID，迟到的旧 start
即使在活动队列已清空后也不能抢走新 tone。暂停音乐的显式恢复不受此限制。
串口 `test wake HiLina` 走同一会话替换控制链（不测试模型识别率）；
`test tts_url <http_url>` 可在待命/录音态测试真实 HTTP/MP3/DAC TTS 打断。

每 5 秒的「唤醒吞吐」日志报告生产/推理帧率、丢帧、推理耗时与最大排队
时间。16 kHz/20 ms 链路正常持续输入应约为 50 帧/秒；仅降低模型阈值不能
补回因调度而丢弃的唤醒词片段。
播放器在流水线启动前包装最终输出端口，空 PCM 且非 EOF 时阻塞一个 tick，
有 PCM 时网络媒体每 50 ms、本地 tone 每 10 ms 主动让步；delegate 原 acquire/release、错误和
EOF 语义，不添加永久运行的 GMF job。HTTP EAGAIN 重试也进行短暂退避。
在待命/配网状态可执行 `test audio_stress start`，使用同一上行 Opus 编码
器模拟持续编码负载，但不发送网络音频、不触发业务唤醒。待命状态下再用
`test tts_url <http_url>` 让测试音频走实际 TTS 通道，检查同时负载的帧率。
`test audio_stress stop` 停止压力编码并停止该测试播放；正常开启上报会
自动退出压力模式。压力模式不等于实际云端 ASR 测试，网络发送开销需另测。

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

TTS 首播门槛为 96 KiB（16 kHz/mono/16-bit 约 3 秒），恢复门槛 64 KiB；
短回复在 EOF 时直接播完，不等待达到水位。云端分块流的生成可能慢于
播放速度，因此优先完整播报，首帧会比原来的 4 KiB/200 ms 策略晚。
等待首帧期间暂停录音空闲超时；新唤醒、取消和播放失败仍按当前会话代次收尾。
ROMFS tone 首块直接输出。预缓冲等待中若会话换成 tone，会重新读取新
会话水位，不沿用旧 TTS 的门槛。网络 TTS 保留 32 KiB 压缩数据环缓冲；
首帧之前读取 256 字节；云端 chunked 实时 TTS 保持 256 字节读取，避免
`esp_http_client_read()` 为凑满 1 KiB 而积攒多个 MP3 帧。非分块响应
已有 PCM 后以 1 KiB 拉流，保留下载文件的吞吐效率。
音乐/TF 缓冲不变。

TTS 取走队尾时不判定欠载：当前帧仍在 DMA 播放，后续 PCM 可能马上到达。
输出任务实际等不到新帧超过 60 ms 才进入恢复缓冲。短暂空队列立即续播，
不会反复人为添加 200 ms 的恢复等待；采样率和语速不变。
TTS 已解码 PCM 的高/低水位为 128/112 KiB。满水位时生产者
阻塞，播放回落后再继续解码，避免高优先级解码一次提前占用数秒 CPU。

AGC 峰值保护与额外增益算法仍保留：能量使用 uint64 累加，逐样本混合使用
float；当前并未调用它，也不运行独立 WebRTC AGC。每 5 秒的「AFE 后处理」
日志报告帧率和处理耗时，连同唤醒/编码吞吐观察负载，不等同于纯 CPU 用时。

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
  超前读取；网络 TTS 使用短启动缓冲、有上限的等待和独立压缩数据缓存。
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
