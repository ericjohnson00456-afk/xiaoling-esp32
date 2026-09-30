# 音频服务

`AudioService` 是整机唯一的音频数据入口和出口。它不负责对话、音乐等业务状态，只负责稳定地产生和播放音频数据。

## 初始化和生命周期

`Application::InitializeAudio()` 按以下顺序执行一次：

1. `AudioService::Initialize(codec)` 启动 codec，加载唤醒资源，再创建 Opus 编解码器和 AFE/AEC。
2. 注册音频事件回调。
3. `AudioService::Start()` 启动采集、输出和 Opus 编解码任务。
4. 打开唤醒词检测。

`Start()` 之后，ADC 采集、AFE/AEC 和 Opus 编码始终运行。普通业务状态切换不得启停这些任务；固件 OTA 是允许调用 `Stop()/Start()` 的系统级例外。

AEC 能力由 `AudioService` 根据板型配置和 codec 参考声道判断；硬件支持时默认开启。单工、半双工和全双工属于小聆会话策略，不在音频初始化中决定。

## 采集链

```text
AudioCodec ADC
    -> AudioInputTask（读取并转换为 16 kHz PCM）
    -> AudioProcessor（AFE/AEC/VAD；不支持 AFE 时直通单声道 PCM）
    -> audio_encode_queue_（最多 8 帧，满时丢最旧帧）
    -> OpusCodecTask
    -> opus_packet_queue_（只保存最近 2.4 秒，满时覆盖最旧包）
```

业务层只使用两个接口：

- `EnableAudioUpload(true/false)`：控制网络是否消费新 Opus，不影响采集和编码。
- `PopOpusPacket()`：取出待上报的 Opus 包；关闭上报时返回空。

开始一次新的上报会话时会清掉历史窗口，从当前声音开始发送，避免把唤醒前缓存误发到云端。网络不消费时，固定队列自行覆盖旧包，不会反向阻塞实时采集。

唤醒词检测是采集链的旁路消费者。开启 AEC 时可以读取 AFE 输出，否则读取标准化后的麦克风 PCM；检测结果发布到应用事件循环。

## 播放链

```text
GMF/本地播放器产生 PCM -----------+
                                  +-> audio_playback_queue_
服务端 Opus -> OpusCodecTask -> PCM+          -> AudioOutputTask -> AudioCodec DAC
```

播放器负责 TTS、tone、music、alarm 的播放意图和切歌逻辑；`AudioService` 只维护一个物理 DAC 写入任务。这样不会出现多个模块同时写扬声器的问题。

## 队列原则

- 实时采集永不等待业务消费者，积压时丢旧数据。
- 播放队列允许按调用参数等待，保证完整音频可以顺序输出。
- PCM 队列保持短小；较长缓存使用压缩后的 Opus 包。
- 业务代码不直接操作内部任务和队列。
